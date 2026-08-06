#!/usr/bin/env python3
"""Negative and helper-level regression tests for build_showcase.py.

The watchability cases consume only real checked-in lang_trace output. Scratch files in
this self-test are ordinary marker files for path/publication tests, never trace files.
"""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
from typing import Any, Callable, Optional


sys.dont_write_bytecode = True
import build_showcase as showcase  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def expect_showcase_error(action: Callable[[], object], text: str) -> None:
    try:
        action()
    except showcase.ShowcaseError as error:
        require(
            text in str(error),
            f"expected error containing {text!r}, got {str(error)!r}",
        )
        return
    raise RuntimeError(f"expected ShowcaseError containing {text!r}")


def executable() -> Path:
    return Path(sys.executable).resolve()


def _in_memory_managed_manifest(
    demos: tuple[showcase.Demo, ...],
    note: str = showcase.MANIFEST_NOTE,
    include_verify_events: bool = True,
) -> dict[str, Any]:
    digest = "0" * 64
    artifacts: list[dict[str, Any]] = [
        {
            "id": "schema",
            "type": "schema",
            "label": "measured",
            "path": "SCHEMA.md",
            "desc": "Authoritative deterministic trace-bundle and manifest schema.",
            "schedule": None,
            "sizes": {"SCHEMA.md": 1},
            "sha256": {"SCHEMA.md": digest},
        }
    ]
    for demo in demos:
        artifacts.extend(
            (
                {
                    "id": f"{demo.id}-source",
                    "type": "source",
                    "label": "measured",
                    "path": f"traces/{demo.id}/program.lang",
                    "desc": f"Executed source for {demo.id}.",
                    "schedule": demo.schedule,
                    "sizes": {"program.lang": 1},
                    "sha256": {"program.lang": digest},
                },
                {
                    "id": demo.id,
                    "type": "trace-bundle",
                    "label": "measured",
                    "path": f"traces/{demo.id}",
                    "desc": demo.desc,
                    "schedule": demo.schedule,
                    "sizes": {name: 1 for name in showcase.BUNDLE_FILES},
                    "sha256": {
                        name: digest for name in showcase.BUNDLE_FILES
                    },
                    **(
                        {"verify_events": demo.verify_events}
                        if include_verify_events
                        else {}
                    ),
                },
            )
        )
    return {"note": note, "artifacts": artifacts}


def protected_repository_paths_are_rejected() -> None:
    for protected in (
        showcase.REPOSITORY_ROOT / ".git",
        showcase.REPOSITORY_ROOT / "src",
        showcase.REPOSITORY_ROOT / "include",
        showcase.REPOSITORY_ROOT / "tools",
        showcase.REPOSITORY_ROOT / "demos",
        showcase.REPOSITORY_ROOT / "docs",
        showcase.REPOSITORY_ROOT / "src" / "nested-showcase",
    ):
        expect_showcase_error(
            lambda protected=protected: showcase._validate_paths(
                executable(), protected.resolve(), None
            ),
            "protected repository path",
        )


def output_and_reference_overlap_is_rejected() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "output"
        nested_reference = output / "reference"
        nested_reference.mkdir(parents=True)
        expect_showcase_error(
            lambda: showcase._validate_paths(
                executable(), output.resolve(), nested_reference.resolve()
            ),
            "must not overlap",
        )

        reference = root / "reference"
        nested_output = reference / "output"
        nested_output.mkdir(parents=True)
        expect_showcase_error(
            lambda: showcase._validate_paths(
                executable(), nested_output.resolve(), reference.resolve()
            ),
            "must not overlap",
        )

        expect_showcase_error(
            lambda: showcase._validate_paths(
                executable(), executable().parent, None
            ),
            "overlaps --lang-trace",
        )


def final_symlink_paths_are_rejected() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        target = root / "target"
        target.mkdir()
        linked_output = root / "linked-output"
        linked_output.symlink_to(target, target_is_directory=True)
        expect_showcase_error(
            lambda: showcase._resolve_user_path(linked_output, "--output"),
            "contains a symlink",
        )


def unmanaged_existing_output_is_preserved() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary) / "unmanaged"
        output.mkdir()
        marker = output / "keep.txt"
        marker.write_bytes(b"keep me\n")
        expect_showcase_error(
            lambda: showcase._validate_paths(
                executable(), output.resolve(), None
            ),
            "not an empty or managed showcase directory",
        )
        require(marker.read_bytes() == b"keep me\n", "unmanaged output was changed")


def empty_and_managed_outputs_are_allowed() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        empty = Path(temporary) / "empty"
        empty.mkdir()
        showcase._validate_paths(executable(), empty.resolve(), None)
    showcase._validate_paths(
        executable(), (showcase.REPOSITORY_ROOT / "showcase").resolve(), None
    )

    former_demos = tuple(
        demo
        for demo in showcase.DEMOS
        if demo.id in showcase.FORMER_T4_DEMO_IDS
    )
    require(
        tuple(demo.id for demo in former_demos) == showcase.FORMER_T4_DEMO_IDS,
        "former-T4 in-memory demo profile drifted",
    )
    former_inventory = showcase._managed_inventory(showcase.FORMER_T4_DEMO_IDS)
    former_manifest = _in_memory_managed_manifest(former_demos)
    require(
        showcase._matches_managed_showcase_profile(
            former_inventory, former_manifest
        ),
        "exact former-T4 managed profile was rejected",
    )
    require(
        showcase._matches_managed_showcase_profile(
            showcase._managed_inventory(),
            _in_memory_managed_manifest(showcase.DEMOS),
        ),
        "exact current managed profile was rejected",
    )

    legacy_note = (
        "Measured by the lang_trace emitter at commit "
        + "a" * 40
        + "; every trace byte comes from real program execution."
    )
    require(
        showcase._matches_managed_showcase_profile(
            former_inventory,
            _in_memory_managed_manifest(
                former_demos, legacy_note, include_verify_events=False
            ),
        ),
        "exact historical three-demo managed note was rejected",
    )
    legacy_prefix = "Measured by the lang_trace emitter at commit "
    for malformed_note in (
        legacy_prefix,
        legacy_prefix + "garbage",
        legacy_prefix
        + "g" * 40
        + "; every trace byte comes from real program execution.",
    ):
        require(
            not showcase._matches_managed_showcase_profile(
                former_inventory,
                _in_memory_managed_manifest(
                    former_demos,
                    malformed_note,
                    include_verify_events=False,
                ),
            ),
            "former-T4 profile accepted a malformed historical note",
        )
    require(
        not showcase._matches_managed_showcase_profile(
            showcase._managed_inventory(),
            _in_memory_managed_manifest(showcase.DEMOS, legacy_note),
        ),
        "current profile accepted a historical three-demo note",
    )

    missing_inventory = dict(former_inventory)
    missing_inventory.pop("traces/tree_churn/events.jsonl")
    require(
        not showcase._matches_managed_showcase_profile(
            missing_inventory, former_manifest
        ),
        "former-T4 profile accepted missing inventory",
    )
    extra_inventory = dict(former_inventory)
    extra_inventory["unexpected"] = "file"
    require(
        not showcase._matches_managed_showcase_profile(
            extra_inventory, former_manifest
        ),
        "former-T4 profile accepted extra inventory",
    )

    former_artifacts = former_manifest["artifacts"]
    stripped_artifacts = [
        {"id": artifact["id"], "label": artifact["label"]}
        for artifact in former_artifacts
    ]
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory,
            {"note": showcase.MANIFEST_NOTE, "artifacts": stripped_artifacts},
        ),
        "former-T4 profile accepted stripped artifact metadata",
    )

    wrong_id_artifacts = deepcopy(former_artifacts)
    wrong_id_artifacts[-1]["id"] = "wrong-id"
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory,
            {"note": showcase.MANIFEST_NOTE, "artifacts": wrong_id_artifacts},
        ),
        "former-T4 profile accepted a wrong artifact ID",
    )
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory,
            {
                "note": showcase.MANIFEST_NOTE,
                "artifacts": former_artifacts[:-1],
            },
        ),
        "former-T4 profile accepted a missing artifact ID",
    )
    duplicate_id_artifacts = deepcopy(former_artifacts)
    duplicate_id_artifacts[-1]["id"] = duplicate_id_artifacts[0]["id"]
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory,
            {
                "note": showcase.MANIFEST_NOTE,
                "artifacts": duplicate_id_artifacts,
            },
        ),
        "former-T4 profile accepted a duplicate artifact ID",
    )
    wrong_label_artifacts = deepcopy(former_artifacts)
    wrong_label_artifacts[0]["label"] = "synthetic"
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory,
            {
                "note": showcase.MANIFEST_NOTE,
                "artifacts": wrong_label_artifacts,
            },
        ),
        "former-T4 profile accepted a wrong artifact label",
    )
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory,
            {"note": "wrong note", "artifacts": former_artifacts},
        ),
        "former-T4 profile accepted a wrong manifest note",
    )

    invariant_near_misses = (
        ("type", "wrong-type"),
        ("path", "wrong/path"),
        ("schedule", "wrong-schedule"),
        ("verify_events", "wrong-mode"),
    )
    for field, value in invariant_near_misses:
        malformed_manifest = deepcopy(former_manifest)
        artifact_index = -1 if field == "verify_events" else 1
        malformed_manifest["artifacts"][artifact_index][field] = value
        require(
            not showcase._matches_managed_showcase_profile(
                former_inventory, malformed_manifest
            ),
            f"former-T4 profile accepted wrong artifact {field}",
        )

    malformed_hash_manifest = deepcopy(former_manifest)
    malformed_hash_manifest["artifacts"][0]["sha256"]["SCHEMA.md"] = "bad"
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory, malformed_hash_manifest
        ),
        "former-T4 profile accepted malformed artifact metadata",
    )

    extra_top_level = deepcopy(former_manifest)
    extra_top_level["unexpected"] = True
    require(
        not showcase._matches_managed_showcase_profile(
            former_inventory, extra_top_level
        ),
        "former-T4 profile accepted extra manifest metadata",
    )

    nesting_depth = max(10_000, sys.getrecursionlimit() * 10)
    deeply_nested_json = "[" * nesting_depth + "0" + "]" * nesting_depth
    require(
        showcase._decode_managed_manifest(deeply_nested_json) is None,
        "deeply nested manifest JSON did not fail closed",
    )


def complete_tree_comparison_rejects_drift() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        left = root / "left"
        right = root / "right"
        (left / "nested").mkdir(parents=True)
        (right / "nested").mkdir(parents=True)
        (left / "nested" / "marker.txt").write_bytes(b"abc")
        (right / "nested" / "marker.txt").write_bytes(b"abc")
        require(
            showcase._compare_trees(left, right, "selftest") == (1, 3),
            "equal tree comparison returned wrong totals",
        )
        showcase._validate_paths(
            executable(), (root / "fresh-output").resolve(), right.resolve()
        )
        (left / "extra.txt").write_bytes(b"extra")
        expect_showcase_error(
            lambda: showcase._compare_trees(left, right, "selftest"),
            "file-set drift",
        )


def failed_publication_restores_previous_output() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        workspace = Path(temporary)
        output = workspace / "output"
        output.mkdir()
        missing_staging = workspace / "missing-staging"
        try:
            showcase._publish(missing_staging, output)
        except OSError:
            pass
        else:
            raise RuntimeError("publication with missing staging unexpectedly passed")
        require(output.is_dir(), "previous output directory was not restored")
        require(not any(output.iterdir()), "restored output directory is not empty")
        require(
            not list(workspace.glob(".output.recovery-*")),
            "publication rollback left a recovery directory behind",
        )


def captured_output_is_revalidated_before_replacement() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "output"
        output.mkdir()
        showcase._validate_paths(executable(), output.resolve(), None)

        marker = output / "concurrent-marker.txt"
        marker.write_bytes(b"must survive\n")
        staged = root / "staged"
        staged.mkdir()
        (staged / "replacement.txt").write_bytes(b"replacement\n")
        expect_showcase_error(
            lambda: showcase._publish(staged, output),
            "captured output is not an empty or managed showcase directory",
        )
        require(marker.read_bytes() == b"must survive\n", "mutated output was lost")
        require(staged.is_dir(), "staged tree moved after rejected revalidation")
        require(
            not list(root.glob(".output.recovery-*")),
            "successful restoration left a recovery directory behind",
        )


def reappearing_output_preserves_previous_backup() -> None:
    class ReappearingStaged:
        def rename(self, output: Path) -> None:
            output.mkdir()
            (output / "concurrent-marker.txt").write_bytes(b"new occupant\n")
            raise OSError("forced publication failure")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        output = root / "output"
        output.mkdir()
        expect_showcase_error(
            lambda: showcase._publish(ReappearingStaged(), output),
            "previous output retained at",
        )
        require(
            (output / "concurrent-marker.txt").read_bytes() == b"new occupant\n",
            "reappearing output was changed",
        )
        recovery_roots = list(root.glob(".output.recovery-*"))
        require(len(recovery_roots) == 1, "previous output was not retained once")
        backup = recovery_roots[0] / "previous-output"
        require(backup.is_dir(), "retained previous output is missing")


def _index_moving_floor_real_events(
    events: list[dict[str, Any]],
) -> dict[str, Any]:
    event_by_seq: dict[int, dict[str, Any]] = {}
    major_collection_by_seq: dict[int, Optional[int]] = {}
    major_collections: dict[int, dict[str, Any]] = {}
    transactions: dict[int, dict[str, Any]] = {}
    move_stack: list[int] = []
    active_collection: Optional[tuple[int, str]] = None

    for event in events:
        kind = event["kind"]
        seq = event["seq"]
        require(
            type(seq) is int and seq not in event_by_seq,
            "real moving_floor event sequence is malformed",
        )
        if kind == "gc" and event.get("op") == "collection_begin":
            require(
                active_collection is None and not move_stack,
                "real moving_floor collection nesting is unbalanced",
            )
            collection_id = event["collection_id"]
            collection_kind = event["collection_kind"]
            active_collection = (collection_id, collection_kind)
            if collection_kind == "major":
                require(
                    collection_id not in major_collections,
                    "real moving_floor repeats a major collection ID",
                )
                major_collections[collection_id] = {
                    "positive_mark_seqs": [],
                    "source_mutator_seqs": [],
                    "qualifying_transactions": [],
                    "death_seqs": [],
                }

        active_major = (
            active_collection[0]
            if active_collection is not None
            and active_collection[1] == "major"
            else None
        )
        event_by_seq[seq] = event
        major_collection_by_seq[seq] = active_major

        if kind == "gc" and event.get("op") == "move_begin":
            transaction_id = event["transaction_id"]
            expected_parent = move_stack[-1] if move_stack else None
            expected_collection = (
                active_collection[0] if active_collection is not None else None
            )
            require(
                transaction_id not in transactions
                and event["parent_transaction_id"] == expected_parent
                and event["depth"] == len(move_stack) + 1
                and event["collection_id"] == expected_collection,
                "real moving_floor move_begin nesting is malformed",
            )
            transactions[transaction_id] = {
                "cause": event["cause"],
                "collection_id": active_major,
                "tick": event["tick"],
                "parent_transaction_id": expected_parent,
                "depth": event["depth"],
                "compaction_relocation_seqs": [],
            }
            move_stack.append(transaction_id)
        elif kind == "relocate":
            require(bool(move_stack), "real relocation lacks a move transaction")
            if event.get("move_kind") == "compaction":
                transactions[move_stack[-1]]["compaction_relocation_seqs"].append(
                    seq
                )
        elif kind == "gc" and event.get("op") == "move_end":
            require(bool(move_stack), "real move_end lacks a move_begin")
            transaction_id = move_stack[-1]
            transaction = transactions[transaction_id]
            require(
                event["transaction_id"] == transaction_id
                and event["parent_transaction_id"]
                == transaction["parent_transaction_id"]
                and event["depth"] == transaction["depth"]
                and event["cause"] == transaction["cause"]
                and event["collection_id"]
                == (
                    active_collection[0]
                    if active_collection is not None
                    else None
                ),
                "real moving_floor move_end nesting is malformed",
            )
            move_stack.pop()
            if (
                transaction["cause"] == "incremental_compaction_step"
                and transaction["compaction_relocation_seqs"]
                and transaction["collection_id"] is not None
            ):
                major_collections[transaction["collection_id"]][
                    "qualifying_transactions"
                ].append(transaction_id)
        elif (
            kind == "mark_slice"
            and active_major is not None
            and type(event.get("size")) is int
            and event["size"] > 0
        ):
            major_collections[active_major]["positive_mark_seqs"].append(seq)
        elif kind == "die" and active_major is not None:
            major_collections[active_major]["death_seqs"].append(seq)

        if (
            kind in {"alloc", "update"}
            and active_major is not None
            and isinstance(event.get("src_pos"), dict)
        ):
            major_collections[active_major]["source_mutator_seqs"].append(seq)

        if kind == "gc" and event.get("op") == "collection_end":
            require(
                active_collection is not None
                and event["collection_id"] == active_collection[0]
                and event["collection_kind"] == active_collection[1]
                and not move_stack,
                "real moving_floor collection_end nesting is malformed",
            )
            active_collection = None

    require(
        active_collection is None and not move_stack,
        "real moving_floor trace has an unclosed scope",
    )
    qualifying_relocation_owner: dict[int, int] = {}
    for collection in major_collections.values():
        for transaction_id in collection["qualifying_transactions"]:
            for seq in transactions[transaction_id][
                "compaction_relocation_seqs"
            ]:
                qualifying_relocation_owner[seq] = transaction_id
    return {
        "events": events,
        "event_by_seq": event_by_seq,
        "major_collection_by_seq": major_collection_by_seq,
        "major_collections": major_collections,
        "transactions": transactions,
        "qualifying_relocation_owner": qualifying_relocation_owner,
    }


def _moving_floor_subset_evidence(
    index: dict[str, Any], retained_seqs: set[int], collection_id: int
) -> dict[str, Any]:
    collection = index["major_collections"][collection_id]
    event_by_seq = index["event_by_seq"]
    mark_ticks = sorted(
        {
            event_by_seq[seq]["tick"]
            for seq in collection["positive_mark_seqs"]
            if seq in retained_seqs
        }
    )
    source_mutators = [
        event_by_seq[seq]
        for seq in collection["source_mutator_seqs"]
        if seq in retained_seqs
    ]
    transaction_ids = [
        transaction_id
        for transaction_id in collection["qualifying_transactions"]
        if any(
            seq in retained_seqs
            for seq in index["transactions"][transaction_id][
                "compaction_relocation_seqs"
            ]
        )
    ]
    move_ticks = sorted(
        {
            index["transactions"][transaction_id]["tick"]
            for transaction_id in transaction_ids
        }
    )
    marking_mutators = (
        [
            event
            for event in source_mutators
            if mark_ticks[0] < event["tick"] < mark_ticks[-1]
        ]
        if mark_ticks
        else []
    )
    movement_updates = (
        [
            event
            for event in source_mutators
            if event["kind"] == "update"
            and move_ticks[0] < event["tick"] < move_ticks[-1]
        ]
        if move_ticks
        else []
    )
    return {
        "mark_ticks": mark_ticks,
        "marking_mutators": marking_mutators,
        "transaction_ids": transaction_ids,
        "move_ticks": move_ticks,
        "movement_updates": movement_updates,
        "ordered": bool(
            mark_ticks and move_ticks and mark_ticks[-1] < move_ticks[0]
        ),
    }


def _moving_floor_is_hero(evidence: dict[str, Any]) -> bool:
    return (
        len(evidence["mark_ticks"]) >= 4
        and len(evidence["transaction_ids"]) >= 3
        and len(evidence["move_ticks"]) >= 3
        and bool(evidence["marking_mutators"])
        and bool(evidence["movement_updates"])
        and evidence["ordered"]
    )


def _moving_floor_candidate_ids(
    index: dict[str, Any], retained_seqs: set[int]
) -> list[int]:
    return [
        collection_id
        for collection_id in index["major_collections"]
        if _moving_floor_is_hero(
            _moving_floor_subset_evidence(index, retained_seqs, collection_id)
        )
    ]


def _require_moving_floor_global_evidence(
    index: dict[str, Any], retained_seqs: set[int]
) -> None:
    promotions = sum(
        event["kind"] == "promote"
        for event in index["events"]
        if event["seq"] in retained_seqs
    )
    death_sizes = [
        sum(seq in retained_seqs for seq in collection["death_seqs"])
        for collection in index["major_collections"].values()
    ]
    require(promotions >= 8, "moving_floor subset lost promotion evidence")
    require(
        sum(size >= 3 for size in death_sizes) >= 2,
        "moving_floor subset lost collection-local death waves",
    )


def _moving_floor_retained_events(
    index: dict[str, Any], retained_seqs: set[int]
) -> list[dict[str, Any]]:
    return [
        event for event in index["events"] if event["seq"] in retained_seqs
    ]


def watchability_predicates_reject_dull_real_event_subsets() -> None:
    traces = showcase.REPOSITORY_ROOT / "showcase" / "traces"
    demos = {demo.id: demo for demo in showcase.DEMOS}
    real_events = {
        demo_id: [
            json.loads(line)
            for line in (traces / demo_id / "events.jsonl").read_text(
                encoding="utf-8"
            ).splitlines()
        ]
        for demo_id in demos
    }
    for demo_id, events in real_events.items():
        showcase._assert_watchability(
            demos[demo_id], events, showcase._event_counts(events)
        )

    tree_events = [
        event
        for event in real_events["tree_churn"]
        if event["kind"] != "relocate"
    ]
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["tree_churn"], tree_events, showcase._event_counts(tree_events)
        ),
        "nonzero relocate",
    )

    intern_events = [
        event
        for event in real_events["intern_pressure"]
        if not (event["kind"] == "intern" and event.get("hit") == 1)
    ]
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["intern_pressure"],
            intern_events,
            showcase._event_counts(intern_events),
        ),
        "intern hit",
    )

    ephemeron_events = [
        event
        for event in real_events["ephemeron_lifecycle"]
        if event["kind"] != "die"
    ]
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["ephemeron_lifecycle"],
            ephemeron_events,
            showcase._event_counts(ephemeron_events),
        ),
        "key did not die",
    )

    moving_events = real_events["moving_floor"]
    moving_index = _index_moving_floor_real_events(moving_events)
    all_retained_seqs = set(moving_index["event_by_seq"])
    moving_detail = showcase._assert_watchability(
        demos["moving_floor"],
        moving_events,
        showcase._event_counts(moving_events),
    )
    require(
        "ordered=1" in moving_detail,
        "moving_floor WATCH detail did not prove mark-before-move ordering",
    )
    initial_candidates = _moving_floor_candidate_ids(
        moving_index, all_retained_seqs
    )
    require(initial_candidates, "real moving_floor trace has no hero collection")

    hero_collection_id: Optional[int] = None
    retained_move_ticks: Optional[tuple[int, int]] = None
    retained_move_transaction_ids: set[int] = set()
    for candidate_id in initial_candidates:
        candidate = _moving_floor_subset_evidence(
            moving_index, all_retained_seqs, candidate_id
        )
        transactions_by_tick: dict[int, list[int]] = {}
        for transaction_id in candidate["transaction_ids"]:
            tick = moving_index["transactions"][transaction_id]["tick"]
            transactions_by_tick.setdefault(tick, []).append(transaction_id)
        for movement_update in candidate["movement_updates"]:
            for lower_tick in candidate["move_ticks"]:
                for upper_tick in candidate["move_ticks"]:
                    if not lower_tick < movement_update["tick"] < upper_tick:
                        continue
                    transaction_ids = {
                        *transactions_by_tick[lower_tick],
                        *transactions_by_tick[upper_tick],
                    }
                    if len(transaction_ids) >= 3:
                        hero_collection_id = candidate_id
                        retained_move_ticks = (lower_tick, upper_tick)
                        retained_move_transaction_ids = transaction_ids
                        break
                if hero_collection_id is not None:
                    break
            if hero_collection_id is not None:
                break
        if hero_collection_id is not None:
            break
    require(
        hero_collection_id is not None
        and retained_move_ticks is not None
        and len(retained_move_transaction_ids) >= 3,
        "real moving_floor hero cannot isolate movement tick evidence",
    )
    hero = _moving_floor_subset_evidence(
        moving_index, all_retained_seqs, hero_collection_id
    )

    marking_witness = hero["marking_mutators"][0]
    interior_mark_ticks = hero["mark_ticks"][1:-1]
    preferred_interior_tick = next(
        (
            tick
            for tick in interior_mark_ticks
            if tick == marking_witness["tick"]
        ),
        interior_mark_ticks[0],
    )
    retained_mark_ticks = {
        hero["mark_ticks"][0],
        preferred_interior_tick,
        hero["mark_ticks"][-1],
    }
    require(
        len(retained_mark_ticks) == 3
        and min(retained_mark_ticks)
        < marking_witness["tick"]
        < max(retained_mark_ticks),
        "real moving_floor hero cannot isolate three marking ticks",
    )
    mark_subset_retained = set(all_retained_seqs)
    for collection_id, collection in moving_index["major_collections"].items():
        for seq in collection["positive_mark_seqs"]:
            event = moving_index["event_by_seq"][seq]
            if not (
                collection_id == hero_collection_id
                and event["tick"] in retained_mark_ticks
            ):
                mark_subset_retained.remove(seq)
    mark_subset_evidence = _moving_floor_subset_evidence(
        moving_index, mark_subset_retained, hero_collection_id
    )
    require(
        set(mark_subset_evidence["mark_ticks"]) == retained_mark_ticks
        and len(mark_subset_evidence["mark_ticks"]) == 3
        and mark_subset_evidence["marking_mutators"]
        and len(mark_subset_evidence["transaction_ids"]) >= 3
        and len(mark_subset_evidence["move_ticks"]) >= 3
        and mark_subset_evidence["movement_updates"]
        and mark_subset_evidence["ordered"],
        "three-tick subset lost non-target hero evidence",
    )
    _require_moving_floor_global_evidence(moving_index, mark_subset_retained)
    require(
        not _moving_floor_candidate_ids(moving_index, mark_subset_retained),
        "three-tick subset left an alternate hero collection",
    )
    too_few_mark_ticks = _moving_floor_retained_events(
        moving_index, mark_subset_retained
    )
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["moving_floor"],
            too_few_mark_ticks,
            showcase._event_counts(too_few_mark_ticks),
        ),
        "max_mark_ticks=3",
    )

    transactions_by_tick: dict[int, list[int]] = {}
    for transaction_id in hero["transaction_ids"]:
        tick = moving_index["transactions"][transaction_id]["tick"]
        transactions_by_tick.setdefault(tick, []).append(transaction_id)
    transaction_pair = {
        transactions_by_tick[retained_move_ticks[0]][0],
        transactions_by_tick[retained_move_ticks[1]][0],
    }
    transaction_subset_retained = {
        seq
        for seq in all_retained_seqs
        if moving_index["qualifying_relocation_owner"].get(seq)
        in transaction_pair
        or seq not in moving_index["qualifying_relocation_owner"]
    }
    transaction_subset_evidence = _moving_floor_subset_evidence(
        moving_index, transaction_subset_retained, hero_collection_id
    )
    # Three distinct movement ticks necessarily require at least three
    # transactions, so this direct transaction-count negative also fails the
    # logically coupled tick-count predicate without fabricating transactions.
    require(
        len(transaction_subset_evidence["transaction_ids"]) == 2
        and len(transaction_subset_evidence["mark_ticks"]) >= 4
        and transaction_subset_evidence["marking_mutators"]
        and transaction_subset_evidence["movement_updates"]
        and transaction_subset_evidence["ordered"],
        "two-transaction subset lost non-target hero evidence",
    )
    _require_moving_floor_global_evidence(
        moving_index, transaction_subset_retained
    )
    require(
        not _moving_floor_candidate_ids(
            moving_index, transaction_subset_retained
        ),
        "two-transaction subset left an alternate hero collection",
    )
    too_few_transactions = _moving_floor_retained_events(
        moving_index, transaction_subset_retained
    )
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["moving_floor"],
            too_few_transactions,
            showcase._event_counts(too_few_transactions),
        ),
        "max_move_transactions=2",
    )

    move_tick_subset_retained = {
        seq
        for seq in all_retained_seqs
        if moving_index["qualifying_relocation_owner"].get(seq)
        in retained_move_transaction_ids
        or seq not in moving_index["qualifying_relocation_owner"]
    }
    move_tick_subset_evidence = _moving_floor_subset_evidence(
        moving_index, move_tick_subset_retained, hero_collection_id
    )
    require(
        len(move_tick_subset_evidence["transaction_ids"]) >= 3
        and set(move_tick_subset_evidence["move_ticks"])
        == set(retained_move_ticks)
        and len(move_tick_subset_evidence["move_ticks"]) == 2
        and len(move_tick_subset_evidence["mark_ticks"]) >= 4
        and move_tick_subset_evidence["marking_mutators"]
        and move_tick_subset_evidence["movement_updates"]
        and move_tick_subset_evidence["ordered"],
        "two-move-tick subset lost non-target hero evidence",
    )
    _require_moving_floor_global_evidence(
        moving_index, move_tick_subset_retained
    )
    require(
        not _moving_floor_candidate_ids(moving_index, move_tick_subset_retained),
        "two-move-tick subset left an alternate hero collection",
    )
    too_few_move_ticks = _moving_floor_retained_events(
        moving_index, move_tick_subset_retained
    )
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["moving_floor"],
            too_few_move_ticks,
            showcase._event_counts(too_few_move_ticks),
        ),
        "max_move_ticks=2",
    )

    marking_witness_seqs = {
        event["seq"]
        for candidate_id in initial_candidates
        for event in _moving_floor_subset_evidence(
            moving_index, all_retained_seqs, candidate_id
        )["marking_mutators"]
    }
    marking_interleave_retained = all_retained_seqs - marking_witness_seqs
    marking_interleave_evidence = _moving_floor_subset_evidence(
        moving_index, marking_interleave_retained, hero_collection_id
    )
    require(
        len(marking_interleave_evidence["mark_ticks"]) >= 4
        and not marking_interleave_evidence["marking_mutators"]
        and len(marking_interleave_evidence["transaction_ids"]) >= 3
        and len(marking_interleave_evidence["move_ticks"]) >= 3
        and marking_interleave_evidence["movement_updates"]
        and marking_interleave_evidence["ordered"],
        "marking-interleave subset lost movement evidence",
    )
    _require_moving_floor_global_evidence(
        moving_index, marking_interleave_retained
    )
    require(
        not _moving_floor_candidate_ids(
            moving_index, marking_interleave_retained
        ),
        "marking-interleave subset left an alternate hero collection",
    )
    no_marking_interleave = _moving_floor_retained_events(
        moving_index, marking_interleave_retained
    )
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["moving_floor"],
            no_marking_interleave,
            showcase._event_counts(no_marking_interleave),
        ),
        "hero watchability failed",
    )

    movement_witness_seqs = {
        event["seq"]
        for candidate_id in initial_candidates
        for event in _moving_floor_subset_evidence(
            moving_index, all_retained_seqs, candidate_id
        )["movement_updates"]
    }
    movement_interleave_retained = all_retained_seqs - movement_witness_seqs
    movement_interleave_evidence = _moving_floor_subset_evidence(
        moving_index, movement_interleave_retained, hero_collection_id
    )
    require(
        len(movement_interleave_evidence["mark_ticks"]) >= 4
        and movement_interleave_evidence["marking_mutators"]
        and len(movement_interleave_evidence["transaction_ids"]) >= 3
        and len(movement_interleave_evidence["move_ticks"]) >= 3
        and not movement_interleave_evidence["movement_updates"]
        and movement_interleave_evidence["ordered"],
        "movement-interleave subset lost marking evidence",
    )
    _require_moving_floor_global_evidence(
        moving_index, movement_interleave_retained
    )
    require(
        not _moving_floor_candidate_ids(
            moving_index, movement_interleave_retained
        ),
        "movement-interleave subset left an alternate hero collection",
    )
    no_movement_interleave = _moving_floor_retained_events(
        moving_index, movement_interleave_retained
    )
    expect_showcase_error(
        lambda: showcase._assert_watchability(
            demos["moving_floor"],
            no_movement_interleave,
            showcase._event_counts(no_movement_interleave),
        ),
        "hero watchability failed",
    )


def verify_event_policy_is_explicit_and_manifested() -> None:
    expected_schedules = {
        "tree_churn": "combined_mark_compact",
        "intern_pressure": "combined_mark_compact",
        "ephemeron_lifecycle": "incremental_1",
        "moving_floor": "incremental_compact_3_1",
    }
    expected_modes = {
        "tree_churn": "sampled",
        "intern_pressure": "sampled",
        "ephemeron_lifecycle": "full",
        "moving_floor": "sampled",
    }
    require(
        {demo.id: demo.schedule for demo in showcase.DEMOS} == expected_schedules,
        "showcase schedule policy drifted",
    )
    require(
        {demo.id: demo.verify_events for demo in showcase.DEMOS} == expected_modes,
        "showcase verify-event policy drifted",
    )

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for demo in showcase.DEMOS:
            trace_directory = root / "traces" / demo.id
            trace_directory.mkdir(parents=True)
            command = showcase._lang_trace_command(
                Path("/lang_trace"),
                trace_directory / "program.lang",
                trace_directory,
                demo,
            )
            require(
                command.count("--schedule") == 1,
                f"{demo.id}: lang_trace command did not set schedule exactly once",
            )
            schedule_option = command.index("--schedule")
            require(
                command[schedule_option + 1] == expected_schedules[demo.id],
                f"{demo.id}: lang_trace command used the wrong schedule",
            )
            require(
                command.count("--verify-events") == 1,
                f"{demo.id}: lang_trace command did not set verify events exactly once",
            )
            option = command.index("--verify-events")
            require(
                command[option + 1] == expected_modes[demo.id],
                f"{demo.id}: lang_trace command used the wrong verify-event mode",
            )
            (trace_directory / "stats.json").write_text(
                json.dumps({"verify_events": {"mode": expected_modes[demo.id]}}),
                encoding="utf-8",
            )
            showcase._assert_verify_event_mode(trace_directory, demo)

        mismatched_demo = showcase.DEMOS[0]
        mismatched_directory = root / "traces" / mismatched_demo.id
        (mismatched_directory / "stats.json").write_text(
            json.dumps({"verify_events": {"mode": "full"}}),
            encoding="utf-8",
        )
        expect_showcase_error(
            lambda: showcase._assert_verify_event_mode(
                mismatched_directory, mismatched_demo
            ),
            "verify-events mode mismatch",
        )

    manifest = json.loads(
        (showcase.REPOSITORY_ROOT / "showcase" / "manifest.json").read_text(
            encoding="utf-8"
        )
    )
    require(
        manifest["note"] == showcase.MANIFEST_NOTE,
        "manifest contains stale or ambiguous emitter provenance",
    )
    require(
        all(artifact["label"] == "measured" for artifact in manifest["artifacts"]),
        "manifest contains an artifact not labeled measured",
    )
    bundle_modes = {
        artifact["id"]: artifact.get("verify_events")
        for artifact in manifest["artifacts"]
        if artifact["type"] == "trace-bundle"
    }
    bundle_schedules = {
        artifact["id"]: artifact.get("schedule")
        for artifact in manifest["artifacts"]
        if artifact["type"] == "trace-bundle"
    }
    require(
        bundle_schedules == expected_schedules,
        "manifest schedule policy does not match the emitter policy",
    )
    require(
        bundle_modes == expected_modes,
        "manifest verify-event policy does not match the emitter policy",
    )


def main() -> int:
    tests = (
        protected_repository_paths_are_rejected,
        output_and_reference_overlap_is_rejected,
        final_symlink_paths_are_rejected,
        unmanaged_existing_output_is_preserved,
        empty_and_managed_outputs_are_allowed,
        complete_tree_comparison_rejects_drift,
        failed_publication_restores_previous_output,
        captured_output_is_revalidated_before_replacement,
        reappearing_output_preserves_previous_backup,
        watchability_predicates_reject_dull_real_event_subsets,
        verify_event_policy_is_explicit_and_manifested,
    )
    for test in tests:
        test()
    print(f"SELFTEST OK cases={len(tests)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, showcase.ShowcaseError) as error:
        print(f"SELFTEST FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
