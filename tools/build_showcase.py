#!/usr/bin/env python3
"""Generate and pin measured lang_trace showcase bundles.

The four native trace files are created only by lang_trace. This packager copies the
measured program/stdout companions, runs the conservation checker, enforces workload
watchability, and builds a deterministic manifest over the resulting bytes.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
CONSERVATION_CHECKER = REPOSITORY_ROOT / "tools" / "trace_conservation_check.py"
SCHEMA_SOURCE = REPOSITORY_ROOT / "SCHEMA.md"
EMITTER_COMMIT = "fb6e2aae48a8f40f243fa913cead3dae69a0e74e"
SNAPSHOT_INTERVAL = 256
MAX_EVENTS_BYTES = 2_000_000
PROCESS_TIMEOUT_SECONDS = 120
NATIVE_TRACE_FILES = (
    "events.jsonl",
    "snapshots.jsonl",
    "stats.json",
    "positions.json",
)
BUNDLE_FILES = (*NATIVE_TRACE_FILES, "program.lang", "output.txt")
EVENT_KIND_ORDER = (
    "alloc",
    "mark_slice",
    "relocate",
    "promote",
    "die",
    "intern",
    "evict",
    "trap",
    "verify_step",
    "update",
    "gc",
)


class ShowcaseError(RuntimeError):
    """A deterministic generation, validation, or pinning failure."""


@dataclass(frozen=True)
class Demo:
    id: str
    schedule: str
    desc: str

    @property
    def source(self) -> Path:
        return REPOSITORY_ROOT / "demos" / f"{self.id}.lang"


DEMOS = (
    Demo(
        "tree_churn",
        "combined_mark_compact",
        "Binary-tree pruning, survivor promotion, movement, and final teardown waves.",
    ),
    Demo(
        "intern_pressure",
        "combined_mark_compact",
        "Dynamic weak string interning with canonical hits, misses, survivors, and evictions.",
    ),
    Demo(
        "ephemeron_lifecycle",
        "incremental_1",
        "Conditional-value survival followed by same-collection key/value death.",
    ),
)

PROTECTED_REPOSITORY_PATHS = tuple(
    REPOSITORY_ROOT / name
    for name in (
        ".git",
        "adr",
        "demos",
        "docs",
        "examples",
        "include",
        "src",
        "tests",
        "tools",
    )
)
MANAGED_ARTIFACT_IDS = {
    "schema",
    *(f"{demo.id}-source" for demo in DEMOS),
    *(demo.id for demo in DEMOS),
}


@dataclass(frozen=True)
class DemoResult:
    counts: Dict[str, int]
    checker_verdict: str
    watch_detail: str


@dataclass(frozen=True)
class CollectionEnd:
    collection_id: int
    begin_seq: int
    end_seq: int
    live: frozenset[int]
    ephemeron_refs: Tuple[int, ...]


class UnionFind:
    def __init__(self) -> None:
        self._parent: Dict[int, int] = {}

    def find(self, value: int) -> int:
        parent = self._parent.setdefault(value, value)
        if parent != value:
            self._parent[value] = self.find(parent)
        return self._parent[value]

    def union(self, left: int, right: int) -> None:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root != right_root:
            self._parent[right_root] = left_root


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ShowcaseError(message)


def _decode(data: bytes) -> str:
    return data.decode("utf-8", errors="replace").rstrip("\n")


def _read_events(path: Path) -> List[Dict[str, Any]]:
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8")
    except (OSError, UnicodeError) as error:
        raise ShowcaseError(f"could not read generated events {path}: {error}") from error
    events: List[Dict[str, Any]] = []
    for index, line in enumerate(text.splitlines()):
        try:
            event = json.loads(line)
        except (json.JSONDecodeError, RecursionError) as error:
            raise ShowcaseError(
                f"generated events line {index + 1} is not JSON: {error}"
            ) from error
        if not isinstance(event, dict):
            raise ShowcaseError(
                f"generated events line {index + 1} is not a JSON object"
            )
        events.append(event)
    return events


def _event_counts(events: Iterable[Dict[str, Any]]) -> Dict[str, int]:
    counted = Counter(str(event.get("kind")) for event in events)
    return {kind: counted[kind] for kind in EVENT_KIND_ORDER}


def _tree_watchability(
    demo: Demo, events: List[Dict[str, Any]], counts: Dict[str, int]
) -> str:
    for kind in ("relocate", "promote", "mark_slice", "die"):
        _require(
            counts[kind] > 0,
            f"{demo.id}: watchability requires nonzero {kind} events",
    )
    active_collection: Optional[int] = None
    death_waves: Counter[int] = Counter()
    old_deaths = 0
    for event in events:
        if event["kind"] == "gc" and event.get("op") == "collection_begin":
            active_collection = event["collection_id"]
        elif event["kind"] == "die":
            _require(
                active_collection is not None,
                f"{demo.id}: die event outside a logical collection",
            )
            death_waves[active_collection] += 1
            if event["gen"] == 1:
                old_deaths += 1
        elif event["kind"] == "gc" and event.get("op") == "collection_end":
            active_collection = None
    _require(
        len(death_waves) >= 2,
        f"{demo.id}: expected at least two distinct die waves",
    )
    largest_wave = max(death_waves.values(), default=0)
    _require(
        largest_wave >= 4,
        f"{demo.id}: final teardown did not produce a multi-object die wave",
    )
    _require(
        old_deaths > 0,
        f"{demo.id}: teardown never killed a promoted survivor",
    )
    return (
        f"relocate={counts['relocate']} promote={counts['promote']} "
        f"die_waves={len(death_waves)} largest_die_wave={largest_wave} "
        f"old_die={old_deaths}"
    )


def _intern_watchability(
    demo: Demo, events: List[Dict[str, Any]], counts: Dict[str, int]
) -> str:
    hits = sum(
        1
        for event in events
        if event["kind"] == "intern" and event.get("hit") == 1
    )
    misses = sum(
        1
        for event in events
        if event["kind"] == "intern" and event.get("hit") == 0
    )
    _require(hits > 0, f"{demo.id}: watchability requires an intern hit")
    _require(misses > 0, f"{demo.id}: watchability requires an intern miss")
    _require(counts["evict"] > 0, f"{demo.id}: watchability requires an eviction")
    _require(
        counts["mark_slice"] > 0,
        f"{demo.id}: chosen schedule produced no incremental mark slices",
    )
    return f"intern_hit={hits} intern_miss={misses} evict={counts['evict']}"


def _ephemeron_watchability(
    demo: Demo, events: List[Dict[str, Any]], counts: Dict[str, int]
) -> str:
    allocations = [
        event
        for event in events
        if event["kind"] == "alloc"
        and event.get("object_kind") == "ephemeron"
        and isinstance(event.get("refs"), list)
        and len(event["refs"]) == 2
    ]
    _require(
        len(allocations) == 1,
        f"{demo.id}: expected exactly one two-reference ephemeron allocation",
    )
    allocation = allocations[0]

    lineages = UnionFind()
    for event in events:
        if event["kind"] in {"relocate", "promote"}:
            lineages.union(event["id"], event["to_id"])
    owner_root = lineages.find(allocation["id"])
    key_root = lineages.find(allocation["refs"][0])
    value_root = lineages.find(allocation["refs"][1])
    _require(
        len({owner_root, key_root, value_root}) == 3,
        f"{demo.id}: ephemeron owner, key, and value lineages must be distinct",
    )

    live: Dict[int, Tuple[str, Tuple[int, ...]]] = {}
    active_collection: Optional[Tuple[int, int]] = None
    completed: List[CollectionEnd] = []
    deaths: List[Tuple[int, int, int]] = []
    clears: List[Tuple[int, int]] = []
    value_drops: List[int] = []
    key_drops: List[int] = []

    for event in events:
        kind = event["kind"]
        seq = event["seq"]
        if kind == "alloc":
            logical_id = lineages.find(event["id"])
            live[logical_id] = (
                event["object_kind"],
                tuple(lineages.find(reference) for reference in event["refs"]),
            )
        elif kind == "update":
            logical_id = lineages.find(event["id"])
            _require(
                logical_id in live,
                f"{demo.id}: update references an absent logical owner at seq {seq}",
            )
            old_kind, old_refs = live[logical_id]
            new_refs = tuple(lineages.find(reference) for reference in event["refs"])
            if event["src_pos"] is not None and old_kind == "pair":
                if value_root in old_refs and value_root not in new_refs:
                    value_drops.append(seq)
                if key_root in old_refs and key_root not in new_refs:
                    key_drops.append(seq)
            live[logical_id] = (event["object_kind"], new_refs)
            if (
                logical_id == owner_root
                and event["object_kind"] == "ephemeron"
                and event["src_pos"] is None
                and not new_refs
            ):
                _require(
                    active_collection is not None,
                    f"{demo.id}: ephemeron clear occurred outside a collection",
                )
                clears.append((active_collection[0], seq))
        elif kind == "die":
            logical_id = lineages.find(event["id"])
            _require(
                active_collection is not None,
                f"{demo.id}: lineage death occurred outside a collection",
            )
            deaths.append((active_collection[0], seq, logical_id))
            live.pop(logical_id, None)
        elif kind == "gc" and event.get("op") == "collection_begin":
            _require(
                active_collection is None,
                f"{demo.id}: nested logical collections are not watchable",
            )
            active_collection = (event["collection_id"], seq)
        elif kind == "gc" and event.get("op") == "collection_end":
            _require(
                active_collection is not None
                and active_collection[0] == event["collection_id"],
                f"{demo.id}: unbalanced logical collection at seq {seq}",
            )
            owner_state = live.get(owner_root)
            completed.append(
                CollectionEnd(
                    collection_id=event["collection_id"],
                    begin_seq=active_collection[1],
                    end_seq=seq,
                    live=frozenset(live),
                    ephemeron_refs=owner_state[1] if owner_state is not None else (),
                )
            )
            active_collection = None

    _require(
        len(value_drops) == 1,
        f"{demo.id}: expected one mutator value-holder edge removal, got {value_drops}",
    )
    _require(
        len(key_drops) == 1,
        f"{demo.id}: expected one mutator key-holder edge removal, got {key_drops}",
    )
    value_drop = value_drops[0]
    key_drop = key_drops[0]
    _require(
        value_drop < key_drop,
        f"{demo.id}: key edge dropped before the conditional-survival phase",
    )

    phase_one = [
        collection
        for collection in completed
        if value_drop < collection.end_seq < key_drop
    ]
    _require(
        len(phase_one) >= 2,
        f"{demo.id}: fewer than two collections proved conditional value survival",
    )
    for collection in phase_one:
        _require(
            {owner_root, key_root, value_root}.issubset(collection.live),
            f"{demo.id}: owner/key/value did not all survive collection "
            f"{collection.collection_id}",
        )
        _require(
            key_root in collection.ephemeron_refs
            and value_root in collection.ephemeron_refs,
            f"{demo.id}: ephemeron lost conditional refs during phase one",
        )

    after_key_drop = [
        collection for collection in completed if collection.end_seq > key_drop
    ]
    _require(
        bool(after_key_drop),
        f"{demo.id}: no collection completed after the key edge dropped",
    )
    target = min(after_key_drop, key=lambda collection: collection.end_seq)
    target_deaths = [root for cid, _seq, root in deaths if cid == target.collection_id]
    _require(
        target_deaths.count(key_root) == 1,
        f"{demo.id}: key did not die exactly once in collection {target.collection_id}",
    )
    _require(
        target_deaths.count(value_root) == 1,
        f"{demo.id}: value did not die with its key in collection "
        f"{target.collection_id}",
    )
    _require(
        owner_root not in target_deaths,
        f"{demo.id}: ephemeron owner died with its key",
    )
    target_clears = [seq for cid, seq in clears if cid == target.collection_id]
    _require(
        len(target_clears) == 1,
        f"{demo.id}: expected one owner clear in key-death collection, got "
        f"{target_clears}",
    )
    _require(
        owner_root in target.live
        and key_root not in target.live
        and value_root not in target.live
        and not target.ephemeron_refs,
        f"{demo.id}: key-death collection ended with the wrong lifecycle state",
    )
    _require(
        owner_root in live
        and key_root not in live
        and value_root not in live
        and live[owner_root][1] == (),
        f"{demo.id}: final replay did not retain the cleared ephemeron owner",
    )
    _require(
        counts["mark_slice"] > 0,
        f"{demo.id}: chosen schedule produced no incremental mark slices",
    )
    key_die_seq = next(
        seq
        for cid, seq, root in deaths
        if cid == target.collection_id and root == key_root
    )
    value_die_seq = next(
        seq
        for cid, seq, root in deaths
        if cid == target.collection_id and root == value_root
    )
    return (
        f"phase1_collections={len(phase_one)} "
        f"key_death_collection={target.collection_id} "
        f"key_die_seq={key_die_seq} value_die_seq={value_die_seq} "
        f"owner_clear_seq={target_clears[0]}"
    )


def _assert_watchability(
    demo: Demo, events: List[Dict[str, Any]], counts: Dict[str, int]
) -> str:
    if demo.id == "tree_churn":
        return _tree_watchability(demo, events, counts)
    if demo.id == "intern_pressure":
        return _intern_watchability(demo, events, counts)
    if demo.id == "ephemeron_lifecycle":
        return _ephemeron_watchability(demo, events, counts)
    raise AssertionError(f"unhandled demo {demo.id}")


def _require_exact_emitter_files(trace_directory: Path) -> None:
    expected = set((*NATIVE_TRACE_FILES, "program.lang"))
    actual: Set[str] = set()
    for entry in trace_directory.iterdir():
        if entry.is_symlink() or not entry.is_file():
            raise ShowcaseError(
                f"lang_trace produced a non-regular entry: {entry.name}"
            )
        actual.add(entry.name)
    if actual != expected:
        raise ShowcaseError(
            "lang_trace file-set drift: "
            f"expected={sorted(expected)} actual={sorted(actual)}"
        )


def _run_checker(trace_directory: Path) -> str:
    try:
        checked = subprocess.run(
            [sys.executable, str(CONSERVATION_CHECKER), str(trace_directory)],
            cwd=str(REPOSITORY_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=PROCESS_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        raise ShowcaseError(
            f"conservation checker timed out for {trace_directory.name} after "
            f"{PROCESS_TIMEOUT_SECONDS} seconds"
        ) from error
    if checked.returncode != 0:
        raise ShowcaseError(
            f"conservation checker rejected {trace_directory.name}: "
            f"{_decode(checked.stderr)}"
        )
    if checked.stderr:
        raise ShowcaseError(
            f"conservation checker wrote unexpected stderr for "
            f"{trace_directory.name}: {_decode(checked.stderr)}"
        )
    try:
        verdict = checked.stdout.decode("utf-8").strip()
    except UnicodeDecodeError as error:
        raise ShowcaseError("conservation checker emitted non-UTF-8 output") from error
    if not verdict.startswith("OK events=") or "\n" in verdict:
        raise ShowcaseError(
            f"conservation checker emitted an unexpected verdict: {verdict!r}"
        )
    return verdict


def _generate_demo(
    lang_trace: Path,
    root: Path,
    demo: Demo,
    source_bytes: bytes,
) -> DemoResult:
    trace_directory = root / "traces" / demo.id
    trace_directory.mkdir(parents=True)
    program = trace_directory / "program.lang"
    program.write_bytes(source_bytes)
    try:
        emitted = subprocess.run(
            [
                str(lang_trace),
                str(program),
                "--schedule",
                demo.schedule,
                "--out",
                str(trace_directory),
                "--snapshot-interval",
                str(SNAPSHOT_INTERVAL),
            ],
            cwd=str(REPOSITORY_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=PROCESS_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        raise ShowcaseError(
            f"lang_trace timed out for {demo.id} after "
            f"{PROCESS_TIMEOUT_SECONDS} seconds"
        ) from error
    if emitted.returncode != 0:
        raise ShowcaseError(
            f"lang_trace failed for {demo.id} with status {emitted.returncode}: "
            f"{_decode(emitted.stderr)}"
        )
    if emitted.stderr:
        raise ShowcaseError(
            f"lang_trace wrote unexpected stderr for {demo.id}: "
            f"{_decode(emitted.stderr)}"
        )
    _require_exact_emitter_files(trace_directory)
    (trace_directory / "output.txt").write_bytes(emitted.stdout)

    events_path = trace_directory / "events.jsonl"
    events_size = events_path.stat().st_size
    _require(
        events_size < MAX_EVENTS_BYTES,
        f"{demo.id}: events.jsonl is {events_size} bytes; limit is "
        f"<{MAX_EVENTS_BYTES}",
    )
    verdict = _run_checker(trace_directory)
    events = _read_events(events_path)
    counts = _event_counts(events)
    watch_detail = _assert_watchability(demo, events, counts)
    return DemoResult(counts, verdict, watch_detail)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _metadata(base: Path, names: Sequence[str]) -> Tuple[Dict[str, int], Dict[str, str]]:
    sizes: Dict[str, int] = {}
    digests: Dict[str, str] = {}
    for name in names:
        path = base / name
        _require(path.is_file() and not path.is_symlink(), f"missing artifact file {path}")
        sizes[name] = path.stat().st_size
        digests[name] = _sha256(path)
    return sizes, digests


def _write_manifest(root: Path) -> None:
    artifacts: List[Dict[str, Any]] = []
    schema_sizes, schema_hashes = _metadata(root, ("SCHEMA.md",))
    artifacts.append(
        {
            "id": "schema",
            "type": "schema",
            "label": "measured",
            "path": "SCHEMA.md",
            "desc": "Authoritative deterministic trace-bundle and manifest schema.",
            "schedule": None,
            "sizes": schema_sizes,
            "sha256": schema_hashes,
        }
    )
    for demo in DEMOS:
        trace_directory = root / "traces" / demo.id
        source_sizes, source_hashes = _metadata(trace_directory, ("program.lang",))
        artifacts.append(
            {
                "id": f"{demo.id}-source",
                "type": "source",
                "label": "measured",
                "path": f"traces/{demo.id}/program.lang",
                "desc": f"Executed source for {demo.id}.",
                "schedule": demo.schedule,
                "sizes": source_sizes,
                "sha256": source_hashes,
            }
        )
        bundle_sizes, bundle_hashes = _metadata(trace_directory, BUNDLE_FILES)
        artifacts.append(
            {
                "id": demo.id,
                "type": "trace-bundle",
                "label": "measured",
                "path": f"traces/{demo.id}",
                "desc": demo.desc,
                "schedule": demo.schedule,
                "sizes": bundle_sizes,
                "sha256": bundle_hashes,
            }
        )
    manifest = {
        "note": (
            "Measured by the lang_trace emitter at commit "
            f"{EMITTER_COMMIT}; every trace byte comes from real program execution."
        ),
        "artifacts": artifacts,
    }
    encoded = (json.dumps(manifest, ensure_ascii=True, indent=2) + "\n").encode(
        "utf-8"
    )
    (root / "manifest.json").write_bytes(encoded)


def _build_bundle(
    lang_trace: Path,
    output: Path,
    sources: Dict[str, bytes],
    schema_bytes: bytes,
    report: bool,
) -> Dict[str, DemoResult]:
    output.mkdir()
    (output / "SCHEMA.md").write_bytes(schema_bytes)
    results: Dict[str, DemoResult] = {}
    for demo in DEMOS:
        result = _generate_demo(lang_trace, output, demo, sources[demo.id])
        results[demo.id] = result
        if report:
            rendered_counts = " ".join(
                f"{kind}={result.counts[kind]}" for kind in EVENT_KIND_ORDER
            )
            print(f"CHECK {demo.id} {result.checker_verdict}")
            print(f"COUNTS {demo.id} {rendered_counts}")
            print(f"WATCH {demo.id} {result.watch_detail}")
    _write_manifest(output)
    return results


def _inventory(root: Path) -> Dict[str, str]:
    if root.is_symlink() or not root.is_dir():
        raise ShowcaseError(f"tree root is not a regular directory: {root}")
    inventory: Dict[str, str] = {}
    for current, directory_names, file_names in os.walk(root, followlinks=False):
        directory_names.sort()
        file_names.sort()
        current_path = Path(current)
        for name in (*directory_names, *file_names):
            path = current_path / name
            relative = path.relative_to(root).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                raise ShowcaseError(f"symlink is forbidden in showcase tree: {relative}")
            if stat.S_ISDIR(mode):
                inventory[relative] = "directory"
            elif stat.S_ISREG(mode):
                inventory[relative] = "file"
            else:
                raise ShowcaseError(
                    f"special file is forbidden in showcase tree: {relative}"
                )
    return inventory


def _compare_trees(left: Path, right: Path, purpose: str) -> Tuple[int, int]:
    left_inventory = _inventory(left)
    right_inventory = _inventory(right)
    left_paths = set(left_inventory)
    right_paths = set(right_inventory)
    if left_paths != right_paths:
        missing = sorted(right_paths - left_paths)
        extra = sorted(left_paths - right_paths)
        raise ShowcaseError(
            f"{purpose} file-set drift: missing={missing} extra={extra}"
        )
    file_count = 0
    byte_count = 0
    for relative in sorted(left_inventory):
        left_type = left_inventory[relative]
        right_type = right_inventory[relative]
        if left_type != right_type:
            raise ShowcaseError(
                f"{purpose} type drift at {relative}: "
                f"generated={left_type} reference={right_type}"
            )
        if left_type == "file":
            left_bytes = (left / relative).read_bytes()
            right_bytes = (right / relative).read_bytes()
            if left_bytes != right_bytes:
                raise ShowcaseError(f"{purpose} byte drift at {relative}")
            file_count += 1
            byte_count += len(left_bytes)
    return file_count, byte_count


def _path_is_within(path: Path, directory: Path) -> bool:
    return path == directory or directory in path.parents


def _paths_overlap(left: Path, right: Path) -> bool:
    return _path_is_within(left, right) or _path_is_within(right, left)


def _resolve_user_path(path: Path, option: str) -> Path:
    """Resolve a CLI path without silently accepting a final symlink."""
    lexical = Path(os.path.abspath(os.fspath(path)))
    if lexical.is_symlink():
        raise ShowcaseError(f"{option} contains a symlink: {lexical}")
    return lexical.resolve()


def _managed_inventory() -> Dict[str, str]:
    inventory = {
        "SCHEMA.md": "file",
        "manifest.json": "file",
        "traces": "directory",
    }
    for demo in DEMOS:
        directory = f"traces/{demo.id}"
        inventory[directory] = "directory"
        for name in BUNDLE_FILES:
            inventory[f"{directory}/{name}"] = "file"
    return inventory


def _is_managed_showcase(path: Path) -> bool:
    """Recognize only a complete tree produced by this packager."""
    try:
        if _inventory(path) != _managed_inventory():
            return False
        manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError, ShowcaseError):
        return False
    if not isinstance(manifest, dict):
        return False
    note = manifest.get("note")
    artifacts = manifest.get("artifacts")
    if not isinstance(note, str) or not note.startswith(
        "Measured by the lang_trace emitter at commit "
    ):
        return False
    if not isinstance(artifacts, list) or len(artifacts) != len(MANAGED_ARTIFACT_IDS):
        return False
    ids: Set[str] = set()
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            return False
        artifact_id = artifact.get("id")
        if not isinstance(artifact_id, str) or artifact.get("label") != "measured":
            return False
        ids.add(artifact_id)
    return ids == MANAGED_ARTIFACT_IDS


def _is_replaceable_output(path: Path) -> bool:
    return (
        path.is_dir()
        and not path.is_symlink()
        and (not any(path.iterdir()) or _is_managed_showcase(path))
    )


def _validate_paths(
    lang_trace: Path, output: Path, compare_to: Optional[Path]
) -> None:
    _require(
        lang_trace.is_file() and not lang_trace.is_symlink(),
        f"--lang-trace is not a regular file: {lang_trace}",
    )
    _require(
        os.access(lang_trace, os.X_OK),
        f"--lang-trace is not executable: {lang_trace}",
    )
    _require(
        not _path_is_within(REPOSITORY_ROOT, output),
        f"refusing dangerous output path: {output}",
    )
    for protected in PROTECTED_REPOSITORY_PATHS:
        _require(
            not _path_is_within(output, protected.resolve()),
            f"refusing protected repository path: {output}",
        )
    _require(
        not _paths_overlap(output, lang_trace),
        "--output overlaps --lang-trace",
    )
    inputs = [SCHEMA_SOURCE, CONSERVATION_CHECKER, *(demo.source for demo in DEMOS)]
    for input_path in inputs:
        resolved_input = input_path.resolve()
        _require(
            output != resolved_input and output not in resolved_input.parents,
            f"output path would contain required input {resolved_input}",
        )
    if compare_to is not None:
        _require(
            not _paths_overlap(output, compare_to),
            "--output and --compare-to must not overlap",
        )
    if output.exists() or output.is_symlink():
        _require(
            _is_replaceable_output(output),
            f"existing output is not an empty or managed showcase directory: {output}",
        )
    if compare_to is not None:
        _require(
            compare_to.is_dir() and not compare_to.is_symlink(),
            f"--compare-to is not a regular directory: {compare_to}",
        )


def _publish(staged: Path, output: Path) -> None:
    recovery_root: Optional[Path] = None
    backup: Optional[Path] = None
    try:
        if output.exists() or output.is_symlink():
            recovery_root = Path(
                tempfile.mkdtemp(
                    prefix=f".{output.name}.recovery-", dir=str(output.parent)
                )
            )
            backup = recovery_root / "previous-output"
            output.rename(backup)
            _require(
                _is_replaceable_output(backup),
                "captured output is not an empty or managed showcase directory; "
                "publication aborted",
            )
        staged.rename(output)
    except BaseException as error:
        if backup is not None and (backup.exists() or backup.is_symlink()):
            if not output.exists() and not output.is_symlink():
                try:
                    backup.rename(output)
                except OSError:
                    raise ShowcaseError(
                        "publication failed; previous output retained at "
                        f"{backup}"
                    ) from error
            else:
                raise ShowcaseError(
                    "publication failed; previous output retained at "
                    f"{backup}"
                ) from error
        if recovery_root is not None and recovery_root.exists():
            recovery_root.rmdir()
        raise
    if backup is not None:
        shutil.rmtree(backup)
    if recovery_root is not None:
        recovery_root.rmdir()


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="build deterministic measured showcase trace bundles"
    )
    parser.add_argument(
        "--lang-trace",
        required=True,
        type=Path,
        help="path to the built lang_trace executable",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="directory to replace with the validated generated bundle",
    )
    parser.add_argument(
        "--verify-stable",
        action="store_true",
        help="generate twice and require byte-identical complete trees",
    )
    parser.add_argument(
        "--compare-to",
        type=Path,
        help="require the generated complete tree to match this pinned tree",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        lang_trace = _resolve_user_path(args.lang_trace, "--lang-trace")
        output = _resolve_user_path(args.output, "--output")
        compare_to = (
            _resolve_user_path(args.compare_to, "--compare-to")
            if args.compare_to is not None
            else None
        )
        _validate_paths(lang_trace, output, compare_to)
        sources = {demo.id: demo.source.read_bytes() for demo in DEMOS}
        schema_bytes = SCHEMA_SOURCE.read_bytes()
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix=f".{output.name}.build-", dir=str(output.parent)
        ) as temporary:
            workspace = Path(temporary)
            first = workspace / "first"
            _build_bundle(lang_trace, first, sources, schema_bytes, report=True)
            stable_result: Optional[Tuple[int, int]] = None
            if args.verify_stable:
                second = workspace / "second"
                _build_bundle(lang_trace, second, sources, schema_bytes, report=False)
                stable_result = _compare_trees(first, second, "stability")
            pin_result: Optional[Tuple[int, int]] = None
            if compare_to is not None:
                pin_result = _compare_trees(first, compare_to, "showcase pin")
            build_inventory = _inventory(first)
            build_files = sum(kind == "file" for kind in build_inventory.values())
            build_bytes = sum(
                (first / relative).stat().st_size
                for relative, kind in build_inventory.items()
                if kind == "file"
            )
            _publish(first, output)
        if stable_result is not None:
            print(
                f"STABLE OK files={stable_result[0]} bytes={stable_result[1]}"
            )
        if pin_result is not None:
            print(f"PIN OK files={pin_result[0]} bytes={pin_result[1]}")
        else:
            print(f"BUILD OK files={build_files} bytes={build_bytes}")
        return 0
    except (OSError, ShowcaseError, subprocess.SubprocessError) as error:
        print(f"build_showcase: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
