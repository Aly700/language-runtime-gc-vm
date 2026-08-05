#!/usr/bin/env python3
"""Replay and verify a deterministic lang_trace bundle.

This module deliberately uses only the Python standard library so the honesty gate can
run anywhere the runtime itself is built.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import json
from pathlib import Path
import sys
from typing import Any, NoReturn


UINT32_LIMIT = 1 << 32
UINT64_MAX = (1 << 64) - 1
FIXED_EVENT_KEYS = (
    "tick",
    "seq",
    "kind",
    "id",
    "size",
    "gen",
    "from",
    "to",
    "refs",
    "src_pos",
)
REQUIRED_EVENT_KINDS = (
    "alloc",
    "mark_slice",
    "relocate",
    "promote",
    "die",
    "intern",
    "evict",
    "trap",
    "verify_step",
)
EVENT_KINDS = frozenset((*REQUIRED_EVENT_KINDS, "update", "gc"))
OBJECT_KINDS = frozenset(
    {
        "pair",
        "scalar_array",
        "ref_array",
        "str",
        "closure",
        "map",
        "weak_ref",
        "record",
        "variant",
        "ephemeron",
        "builder",
    }
)
COLLECTION_KINDS = frozenset({"major", "minor"})
MOVE_KINDS = frozenset({"compaction", "growth"})
FORWARD_KINDS = frozenset({"heap", "root", "registry"})
FORWARD_KIND_ORDER = ("heap", "root", "registry")
MOVE_CAUSES = frozenset(
    {
        "atomic_major",
        "atomic_minor",
        "incremental_death_accounting",
        "incremental_compaction_step",
        "incremental_compaction_finalize",
        "incremental_mark_compact",
        "map_growth",
        "builder_growth",
    }
)
GROWTH_CAUSES = frozenset({"map_growth", "builder_growth"})
COMPACTION_CAUSES = frozenset(
    {
        "atomic_major",
        "atomic_minor",
        "incremental_compaction_step",
        "incremental_mark_compact",
    }
)
DEATH_CAUSES = frozenset(
    {
        "atomic_major",
        "atomic_minor",
        "incremental_death_accounting",
        "incremental_mark_compact",
    }
)
IMMEDIATE_FORWARD_CAUSES = frozenset(
    {
        "atomic_major",
        "atomic_minor",
        "incremental_mark_compact",
        "map_growth",
        "builder_growth",
    }
)
VERIFY_STEP_CHECKS = frozenset(
    {
        "validate_after_collection",
        "incremental_tricolor",
        "shadow_marking",
        "shadow_compaction",
        "remembered_set",
        "weak_targets",
        "ephemerons",
        "intern_table",
    }
)
GC_OPS = frozenset(
    {
        "pause",
        "forward",
        "collection_begin",
        "collection_end",
        "move_begin",
        "move_end",
    }
)


def _render(value: object) -> str:
    if isinstance(value, set):
        value = sorted(value)
    try:
        return json.dumps(
            value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
        )
    except TypeError:
        return repr(value)


class TraceViolation(RuntimeError):
    """The first deterministic invariant violation in a trace bundle."""

    def __init__(
        self,
        invariant: str,
        seq: int | str,
        event: str,
        expected: object,
        actual: object,
    ) -> None:
        self.invariant = invariant
        self.seq = seq
        self.event = event
        self.expected = expected
        self.actual = actual
        super().__init__(self.__str__())

    def __str__(self) -> str:
        return (
            f"VIOLATION seq={self.seq} event={self.event} "
            f"invariant={self.invariant} expected={_render(self.expected)} "
            f"actual={_render(self.actual)}"
        )


def _violate(
    invariant: str,
    seq: int | str,
    event: str,
    expected: object,
    actual: object,
) -> NoReturn:
    raise TraceViolation(invariant, seq, event, expected, actual)


@dataclass
class TraceBundle:
    events: list[dict[str, Any]]
    snapshots: list[dict[str, Any]]
    stats: dict[str, Any]


@dataclass(frozen=True)
class HeapObject:
    id: int
    kind: str
    size: int
    generation: int
    refs: tuple[int, ...]


@dataclass
class MovementTransaction:
    transaction_id: int
    cause: str
    collection_id: int | None
    start_objects: dict[int, HeapObject]
    deaths: dict[int, HeapObject]
    relocations: int = 0
    promotions: int = 0
    pending_heap_forwards: Counter[tuple[int, int, int]] = field(
        default_factory=Counter
    )
    observed_heap_forwards: Counter[tuple[int, int, int]] = field(
        default_factory=Counter
    )


@dataclass(frozen=True)
class CheckSummary:
    events: int
    collections: int
    relocations: int
    peak_live_bytes: int
    final_live_bytes: int
    snapshots_verified: int

    def format_line(self) -> str:
        return (
            f"OK events={self.events} collections={self.collections} "
            f"relocations={self.relocations} "
            f"peak_live_bytes={self.peak_live_bytes} "
            f"final_live_bytes={self.final_live_bytes} "
            f"snapshots_verified={self.snapshots_verified}"
        )


class _DuplicateKey(ValueError):
    pass


def _object_from_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateKey(key)
        result[key] = value
    return result


def _reject_non_integer(value: str) -> NoReturn:
    raise ValueError(f"non-integer numeric value {value}")


def _parse_json_line(
    line: str, *, seq: int | str, event: str
) -> dict[str, Any]:
    try:
        value = json.loads(
            line,
            object_pairs_hook=_object_from_pairs,
            parse_float=_reject_non_integer,
            parse_constant=_reject_non_integer,
        )
    except _DuplicateKey as error:
        _violate("FORMAT", seq, event, "unique JSON object keys", str(error))
    except (json.JSONDecodeError, UnicodeError, ValueError, RecursionError) as error:
        _violate("FORMAT", seq, event, "valid integer-only JSON", str(error))
    if not isinstance(value, dict):
        _violate("SCHEMA", seq, event, "JSON object", type(value).__name__)
    return value


def _read_jsonl(path: Path, event: str, *, allow_empty: bool) -> list[dict[str, Any]]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        _violate("FORMAT", "file", event, f"readable {path.name}", str(error))
    if b"\r" in raw:
        _violate("FORMAT", "file", event, "LF-only framing", "CR byte")
    if raw and not raw.endswith(b"\n"):
        _violate("FORMAT", "file", event, "trailing LF", "missing")
    if not raw and not allow_empty:
        _violate("FORMAT", "file", event, "at least one JSON line", "empty file")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        _violate("FORMAT", "file", event, "UTF-8", str(error))
    lines = text[:-1].split("\n") if raw else []
    values: list[dict[str, Any]] = []
    for index, line in enumerate(lines):
        if not line:
            _violate("FORMAT", index, event, "non-empty JSON line", "blank")
        values.append(_parse_json_line(line, seq=index, event=event))
    return values


def load_trace_bundle(trace_directory: Path | str) -> TraceBundle:
    directory = Path(trace_directory)
    events = _read_jsonl(directory / "events.jsonl", "events.jsonl", allow_empty=True)
    snapshots = _read_jsonl(
        directory / "snapshots.jsonl", "snapshots.jsonl", allow_empty=False
    )
    stats_lines = _read_jsonl(
        directory / "stats.json", "stats.json", allow_empty=False
    )
    if len(stats_lines) != 1:
        _violate("SCHEMA", "file", "stats.json", "exactly one JSON object", len(stats_lines))
    return TraceBundle(events=events, snapshots=snapshots, stats=stats_lines[0])


def _is_uint(value: object, maximum: int = UINT64_MAX) -> bool:
    return type(value) is int and 0 <= value <= maximum


def _require_uint(
    value: object,
    *,
    seq: int | str,
    event: str,
    field: str,
    maximum: int = UINT64_MAX,
) -> int:
    if not _is_uint(value, maximum):
        _violate(
            "SCHEMA",
            seq,
            event,
            f"{field} unsigned integer <= {maximum}",
            value,
        )
    return value


def _require_nullable_uint(
    value: object,
    *,
    seq: int | str,
    event: str,
    field: str,
    maximum: int = UINT64_MAX,
) -> int | None:
    if value is None:
        return None
    return _require_uint(
        value, seq=seq, event=event, field=field, maximum=maximum
    )


def _require_choice(
    value: object,
    choices: frozenset[str],
    *,
    seq: int | str,
    event: str,
    field: str,
) -> str:
    if not isinstance(value, str) or value not in choices:
        _violate("SCHEMA", seq, event, {field: sorted(choices)}, value)
    return value


def _require_keys(
    value: dict[str, Any],
    expected: tuple[str, ...],
    *,
    seq: int | str,
    event: str,
) -> None:
    actual = tuple(value.keys())
    if actual != expected:
        _violate("SCHEMA", seq, event, list(expected), list(actual))


def _validate_src_pos(value: object, seq: int, kind: str) -> None:
    if value is None:
        return
    if not isinstance(value, dict):
        _violate("SCHEMA", seq, kind, "src_pos object or null", value)
    keys = tuple(value.keys())
    if keys not in {("line", "col"), ("line", "col", "fn")}:
        _violate(
            "SCHEMA",
            seq,
            kind,
            ["line", "col", "optional fn"],
            list(keys),
        )
    _require_uint(value["line"], seq=seq, event=kind, field="src_pos.line")
    _require_uint(value["col"], seq=seq, event=kind, field="src_pos.col")
    if "fn" in value and not isinstance(value["fn"], str):
        _violate("SCHEMA", seq, kind, "src_pos.fn string", value["fn"])


def _expect_nulls(event: dict[str, Any], seq: int, *fields: str) -> None:
    for field in fields:
        if event[field] is not None:
            _violate("SCHEMA", seq, event["kind"], f"{field}=null", event[field])


def _validate_object_shape(
    kind: str,
    size: int,
    refs: list[object],
    *,
    seq: int | str,
    event: str,
) -> None:
    fixed_width = {"pair": 1, "weak_ref": 1, "ephemeron": 1}
    expected_width = fixed_width.get(kind)
    if expected_width is not None and size != expected_width:
        _violate(
            "SCHEMA",
            seq,
            event,
            {"object_kind": kind, "size": expected_width},
            {"size": size},
        )
    maximum_refs = {
        "pair": 2,
        "scalar_array": 0,
        "ref_array": size,
        "str": 0,
        "closure": max(0, size - 1),
        "map": max(0, size - 1),
        "weak_ref": 1,
        "record": max(0, size - 1),
        "variant": max(0, size - 2),
        "ephemeron": 2,
        "builder": 0,
    }[kind]
    if len(refs) > maximum_refs:
        _violate(
            "SCHEMA",
            seq,
            event,
            {"object_kind": kind, "maximum_refs": maximum_refs},
            {"refs": len(refs)},
        )
    if kind == "map" and (size < 1 or size % 2 == 0):
        _violate(
            "SCHEMA", seq, event, "map size is positive and odd", size
        )
    if kind == "variant" and size < 2:
        _violate("SCHEMA", seq, event, "variant size >= 2", size)
    if kind == "builder":
        payload_slots = size - 1
        capacity = payload_slots * 8
        power_of_two_capacity = (
            8 <= capacity <= (1 << 31)
            and capacity & (capacity - 1) == 0
        )
        maximum_capacity_width = 536_870_913
        if not power_of_two_capacity and size != maximum_capacity_width:
            _violate(
                "SCHEMA",
                seq,
                event,
                "Builder size 1 + capacity/8 for the power-of-two capacity ladder",
                size,
            )


def _validate_event_schema(event: dict[str, Any], expected_seq: int) -> None:
    if len(event) < len(FIXED_EVENT_KEYS):
        _violate(
            "SCHEMA",
            expected_seq,
            "event",
            list(FIXED_EVENT_KEYS),
            list(event.keys()),
        )
    if tuple(event.keys())[: len(FIXED_EVENT_KEYS)] != FIXED_EVENT_KEYS:
        _violate(
            "SCHEMA",
            expected_seq,
            "event",
            list(FIXED_EVENT_KEYS),
            list(event.keys())[: len(FIXED_EVENT_KEYS)],
        )
    tick = _require_uint(event["tick"], seq=expected_seq, event="event", field="tick")
    seq = _require_uint(event["seq"], seq=expected_seq, event="event", field="seq")
    kind = event["kind"]
    if not isinstance(kind, str) or kind not in EVENT_KINDS:
        _violate("SCHEMA", expected_seq, "event", sorted(EVENT_KINDS), kind)
    if seq != expected_seq:
        _violate("EVENT_SEQUENCE", expected_seq, kind, expected_seq, seq)
    _require_nullable_uint(event["id"], seq=seq, event=kind, field="id")
    _require_nullable_uint(event["size"], seq=seq, event=kind, field="size")
    generation = _require_nullable_uint(
        event["gen"], seq=seq, event=kind, field="gen", maximum=1
    )
    _require_nullable_uint(
        event["from"], seq=seq, event=kind, field="from", maximum=UINT32_LIMIT - 1
    )
    _require_nullable_uint(
        event["to"], seq=seq, event=kind, field="to", maximum=UINT32_LIMIT - 1
    )
    refs = event["refs"]
    if refs is not None:
        if not isinstance(refs, list):
            _violate("SCHEMA", seq, kind, "refs array or null", refs)
        for index, reference in enumerate(refs):
            _require_uint(
                reference, seq=seq, event=kind, field=f"refs[{index}]"
            )
    _validate_src_pos(event["src_pos"], seq, kind)

    if kind == "alloc":
        _require_keys(event, (*FIXED_EVENT_KEYS, "object_kind"), seq=seq, event=kind)
        _expect_nulls(event, seq, "from", "to")
        if event["id"] is None or event["size"] is None or generation is None or refs is None:
            _violate("SCHEMA", seq, kind, "id/size/gen/refs non-null", event)
        if event["size"] == 0 or generation != 0:
            _violate("SCHEMA", seq, kind, "positive size and gen=0", [event["size"], generation])
        object_kind = _require_choice(
            event["object_kind"],
            OBJECT_KINDS,
            seq=seq,
            event=kind,
            field="object_kind",
        )
        _validate_object_shape(
            object_kind, event["size"], refs, seq=seq, event=kind
        )
    elif kind == "mark_slice":
        _require_keys(event, FIXED_EVENT_KEYS, seq=seq, event=kind)
        _expect_nulls(event, seq, "id", "gen", "from", "to", "refs", "src_pos")
        if event["size"] is None:
            _violate("SCHEMA", seq, kind, "size non-null", None)
    elif kind == "relocate":
        _require_keys(
            event, (*FIXED_EVENT_KEYS, "to_id", "move_kind"), seq=seq, event=kind
        )
        _expect_nulls(event, seq, "refs", "src_pos")
        if any(event[field] is None for field in ("id", "size", "gen", "from", "to")):
            _violate("SCHEMA", seq, kind, "movement fields non-null", event)
        if event["size"] == 0:
            _violate("SCHEMA", seq, kind, "positive size", 0)
        _require_uint(event["to_id"], seq=seq, event=kind, field="to_id")
        _require_choice(
            event["move_kind"],
            MOVE_KINDS,
            seq=seq,
            event=kind,
            field="move_kind",
        )
    elif kind == "promote":
        _require_keys(event, (*FIXED_EVENT_KEYS, "to_id"), seq=seq, event=kind)
        _expect_nulls(event, seq, "refs", "src_pos")
        if any(event[field] is None for field in ("id", "size", "gen", "from", "to")):
            _violate("SCHEMA", seq, kind, "movement fields non-null", event)
        if event["size"] == 0 or generation != 0:
            _violate("SCHEMA", seq, kind, "positive size and gen=0", [event["size"], generation])
        _require_uint(event["to_id"], seq=seq, event=kind, field="to_id")
    elif kind in {"die", "evict"}:
        _require_keys(event, FIXED_EVENT_KEYS, seq=seq, event=kind)
        _expect_nulls(event, seq, "from", "to", "refs", "src_pos")
        if event["id"] is None or event["size"] is None or generation is None:
            _violate("SCHEMA", seq, kind, "id/size/gen non-null", event)
        if event["size"] == 0:
            _violate("SCHEMA", seq, kind, "positive size", 0)
    elif kind == "intern":
        _require_keys(event, (*FIXED_EVENT_KEYS, "hit"), seq=seq, event=kind)
        _expect_nulls(event, seq, "from", "to", "refs")
        if event["id"] is None or event["size"] is None or generation is None:
            _violate("SCHEMA", seq, kind, "id/size/gen non-null", event)
        if event["size"] == 0 or type(event["hit"]) is not int or event["hit"] not in {0, 1}:
            _violate("SCHEMA", seq, kind, "positive size and hit 0|1", [event["size"], event["hit"]])
    elif kind == "trap":
        _require_keys(event, (*FIXED_EVENT_KEYS, "reason"), seq=seq, event=kind)
        _expect_nulls(event, seq, "id", "size", "gen", "from", "to", "refs")
        if not isinstance(event["reason"], str):
            _violate("SCHEMA", seq, kind, "reason string", event["reason"])
    elif kind == "verify_step":
        _require_keys(event, (*FIXED_EVENT_KEYS, "check"), seq=seq, event=kind)
        _expect_nulls(event, seq, "id", "gen", "from", "to", "refs", "src_pos")
        _require_choice(
            event["check"],
            VERIFY_STEP_CHECKS,
            seq=seq,
            event=kind,
            field="check",
        )
    elif kind == "update":
        _require_keys(event, (*FIXED_EVENT_KEYS, "object_kind"), seq=seq, event=kind)
        _expect_nulls(event, seq, "from", "to")
        if event["id"] is None or event["size"] is None or generation is None or refs is None:
            _violate("SCHEMA", seq, kind, "id/size/gen/refs non-null", event)
        if event["size"] == 0:
            _violate("SCHEMA", seq, kind, "positive size", event["size"])
        object_kind = _require_choice(
            event["object_kind"],
            OBJECT_KINDS,
            seq=seq,
            event=kind,
            field="object_kind",
        )
        _validate_object_shape(
            object_kind, event["size"], refs, seq=seq, event=kind
        )
    elif kind == "gc":
        _validate_gc_schema(event, seq)
    else:
        raise AssertionError(f"unhandled event kind {kind}")
    _ = tick


def _validate_gc_schema(event: dict[str, Any], seq: int) -> None:
    _expect_nulls(event, seq, "id", "size", "gen", "from", "to", "refs", "src_pos")
    op = _require_choice(
        event.get("op"), GC_OPS, seq=seq, event="gc", field="op"
    )
    if op == "pause":
        expected_keys = (*FIXED_EVENT_KEYS, "op", "collection_id")
        _require_keys(event, expected_keys, seq=seq, event="gc")
        _require_nullable_uint(
            event["collection_id"], seq=seq, event="gc", field="collection_id"
        )
        return
    if op == "forward":
        expected_keys = (
            *FIXED_EVENT_KEYS,
            "op",
            "collection_id",
            "from_id",
            "to_id",
            "owner_id",
            "forward_kind",
        )
        _require_keys(event, expected_keys, seq=seq, event="gc")
        _require_nullable_uint(
            event["collection_id"], seq=seq, event="gc", field="collection_id"
        )
        source_id = _require_uint(
            event["from_id"], seq=seq, event="gc", field="from_id"
        )
        destination_id = _require_uint(
            event["to_id"], seq=seq, event="gc", field="to_id"
        )
        owner_id = _require_nullable_uint(
            event["owner_id"], seq=seq, event="gc", field="owner_id"
        )
        if source_id == destination_id:
            _violate(
                "SCHEMA",
                seq,
                "gc",
                "unequal forward from_id and to_id",
                {"from_id": source_id, "to_id": destination_id},
            )
        forward_kind = _require_choice(
            event["forward_kind"],
            FORWARD_KINDS,
            seq=seq,
            event="gc",
            field="forward_kind",
        )
        expected_owner_presence = forward_kind == "heap"
        if (owner_id is not None) != expected_owner_presence:
            _violate(
                "SCHEMA",
                seq,
                "gc",
                {
                    "forward_kind": forward_kind,
                    "owner_id_non_null": expected_owner_presence,
                },
                {"owner_id": owner_id},
            )
        return
    if op in {"collection_begin", "collection_end"}:
        expected_keys = (
            *FIXED_EVENT_KEYS,
            "op",
            "collection_id",
            "collection_kind",
            "live_bytes",
            "live_objects",
        )
        _require_keys(event, expected_keys, seq=seq, event="gc")
        _require_uint(event["collection_id"], seq=seq, event="gc", field="collection_id")
        _require_choice(
            event["collection_kind"],
            COLLECTION_KINDS,
            seq=seq,
            event="gc",
            field="collection_kind",
        )
        _require_uint(event["live_bytes"], seq=seq, event="gc", field="live_bytes")
        _require_uint(event["live_objects"], seq=seq, event="gc", field="live_objects")
        return
    expected_keys = (
        *FIXED_EVENT_KEYS,
        "op",
        "transaction_id",
        "parent_transaction_id",
        "depth",
        "cause",
        "collection_id",
        "live_bytes",
        "live_objects",
    )
    _require_keys(event, expected_keys, seq=seq, event="gc")
    _require_uint(event["transaction_id"], seq=seq, event="gc", field="transaction_id")
    _require_nullable_uint(
        event["parent_transaction_id"],
        seq=seq,
        event="gc",
        field="parent_transaction_id",
    )
    depth = _require_uint(event["depth"], seq=seq, event="gc", field="depth")
    if depth == 0:
        _violate("SCHEMA", seq, "gc", "positive depth", depth)
    _require_choice(
        event["cause"],
        MOVE_CAUSES,
        seq=seq,
        event="gc",
        field="cause",
    )
    _require_nullable_uint(
        event["collection_id"], seq=seq, event="gc", field="collection_id"
    )
    _require_uint(event["live_bytes"], seq=seq, event="gc", field="live_bytes")
    _require_uint(event["live_objects"], seq=seq, event="gc", field="live_objects")


def _validate_snapshot_schema(snapshot: dict[str, Any], index: int) -> None:
    _require_keys(snapshot, ("tick", "seq", "live"), seq=index, event="snapshot")
    _require_uint(snapshot["tick"], seq=index, event="snapshot", field="tick")
    _require_uint(snapshot["seq"], seq=index, event="snapshot", field="seq")
    live = snapshot["live"]
    if not isinstance(live, list):
        _violate("SCHEMA", index, "snapshot", "live array", live)
    previous_base = -1
    for object_index, item in enumerate(live):
        if not isinstance(item, dict):
            _violate("SCHEMA", index, "snapshot", "live object", item)
        _require_keys(
            item,
            ("id", "kind", "size", "gen", "refs"),
            seq=index,
            event="snapshot",
        )
        object_id = _require_uint(
            item["id"], seq=index, event="snapshot", field=f"live[{object_index}].id"
        )
        object_kind = _require_choice(
            item["kind"],
            OBJECT_KINDS,
            seq=index,
            event="snapshot",
            field=f"live[{object_index}].kind",
        )
        size = _require_uint(
            item["size"], seq=index, event="snapshot", field=f"live[{object_index}].size"
        )
        if size == 0:
            _violate("SCHEMA", index, "snapshot", "positive size", size)
        _require_uint(
            item["gen"],
            seq=index,
            event="snapshot",
            field=f"live[{object_index}].gen",
            maximum=1,
        )
        if not isinstance(item["refs"], list):
            _violate("SCHEMA", index, "snapshot", "refs array", item["refs"])
        for ref_index, reference in enumerate(item["refs"]):
            _require_uint(
                reference,
                seq=index,
                event="snapshot",
                field=f"live[{object_index}].refs[{ref_index}]",
            )
        _validate_object_shape(
            object_kind,
            size,
            item["refs"],
            seq=index,
            event="snapshot",
        )
        base = object_id & 0xFFFF_FFFF
        if base <= previous_base:
            _violate("SNAPSHOT_CONSISTENCY", snapshot["seq"], "snapshot", "strict ascending base slots", base)
        previous_base = base


def _validate_stats_schema(stats: dict[str, Any]) -> None:
    expected_keys = (
        "live_bytes_final",
        "forwarded_reference_count",
        "forwarded_reference_totals",
        "pause_slices",
        "collection_count",
        "event_totals",
        "snapshot_interval",
        "ticks",
        "peak_live_bytes",
    )
    _require_keys(stats, expected_keys, seq="stats", event="stats")
    for field in expected_keys:
        if field in {"event_totals", "forwarded_reference_totals"}:
            continue
        _require_uint(stats[field], seq="stats", event="stats", field=field)
    if stats["snapshot_interval"] == 0:
        _violate(
            "SCHEMA",
            "stats",
            "stats",
            "positive snapshot_interval",
            stats["snapshot_interval"],
        )
    totals = stats["event_totals"]
    if not isinstance(totals, dict):
        _violate("SCHEMA", "stats", "stats", "event_totals object", totals)
    _require_keys(totals, REQUIRED_EVENT_KINDS, seq="stats", event="stats")
    for kind in REQUIRED_EVENT_KINDS:
        _require_uint(
            totals[kind], seq="stats", event="stats", field=f"event_totals.{kind}"
        )
    forward_totals = stats["forwarded_reference_totals"]
    if not isinstance(forward_totals, dict):
        _violate(
            "SCHEMA",
            "stats",
            "stats",
            "forwarded_reference_totals object",
            forward_totals,
        )
    _require_keys(
        forward_totals,
        FORWARD_KIND_ORDER,
        seq="stats",
        event="stats",
    )
    for kind in FORWARD_KIND_ORDER:
        _require_uint(
            forward_totals[kind],
            seq="stats",
            event="stats",
            field=f"forwarded_reference_totals.{kind}",
        )


class _Replay:
    def __init__(self, bundle: TraceBundle) -> None:
        self.bundle = bundle
        self.live: dict[int, HeapObject] = {}
        self.seen_ids: set[int] = set()
        self.max_slot_generation: dict[int, int] = {}
        self.forwarding: dict[int, int] = {}
        self.tombstones: dict[int, HeapObject] = {}
        self.interned: set[int] = set()
        self.transactions: list[MovementTransaction] = []
        self.active_collection: tuple[int, str] | None = None
        self.next_collection_id = 0
        self.next_transaction_id = 0
        self.event_totals: Counter[str] = Counter()
        self.pause_slices = 0
        self.forwarded_references = 0
        self.forwarded_reference_totals: Counter[str] = Counter()
        self.collections = 0
        self.collection_ends = 0
        self.relocations = 0
        self.peak_live_bytes = 0
        self.snapshots_verified = 0
        self.last_tick = 0
        self.last_event: dict[str, Any] | None = None
        self.snapshots_by_seq: dict[int, list[dict[str, Any]]] = {}
        self.last_death_base: int | None = None
        self.collection_pending_heap_forwards: Counter[
            tuple[int, int, int]
        ] = Counter()
        self.collection_observed_heap_forwards: Counter[
            tuple[int, int, int]
        ] = Counter()

    def live_bytes(self) -> int:
        return sum(object_.size for object_ in self.live.values()) * 8

    def _sample_peak(self) -> None:
        self.peak_live_bytes = max(self.peak_live_bytes, self.live_bytes())

    def _register_new_id(self, object_id: int, seq: int, event: str) -> None:
        if object_id in self.seen_ids:
            _violate("LIFECYCLE", seq, event, "previously unseen full object id", object_id)
        base = object_id & 0xFFFF_FFFF
        generation = object_id >> 32
        previous = self.max_slot_generation.get(base)
        expected_generation = 1 if previous is None else previous + 1
        if generation != expected_generation:
            _violate(
                "LIFECYCLE",
                seq,
                event,
                {
                    "base": base,
                    "slot_generation": expected_generation,
                },
                generation,
            )
        self.seen_ids.add(object_id)
        self.max_slot_generation[base] = generation

    def _check_interval(
        self, object_id: int, size: int, seq: int, event: str
    ) -> None:
        base = object_id & 0xFFFF_FFFF
        end = base + size
        if end > UINT32_LIMIT:
            _violate("LIFECYCLE", seq, event, "object interval within slot space", [base, end])
        for other in self.live.values():
            other_base = other.id & 0xFFFF_FFFF
            other_end = other_base + other.size
            if base < other_end and other_base < end:
                _violate(
                    "LIFECYCLE",
                    seq,
                    event,
                    "non-overlapping live storage interval",
                    {"id": object_id, "conflicts_with": other.id},
                )

    def resolve(self, object_id: int, seq: int, event: str) -> int:
        current = object_id
        visited: set[int] = set()
        while current in self.forwarding:
            if current in visited:
                _violate("MOVEMENT_CONSERVATION", seq, event, "acyclic forwarding chain", object_id)
            visited.add(current)
            current = self.forwarding[current]
        return current

    def _require_resolved_references(self, seq: int, event: str) -> None:
        for owner in sorted(self.live.values(), key=lambda object_: object_.id & 0xFFFF_FFFF):
            for index, reference in enumerate(owner.refs):
                terminal = self.resolve(reference, seq, event)
                if terminal not in self.live:
                    _violate(
                        "REFERENCE_RESOLUTION",
                        seq,
                        event,
                        "reference forwarding chain terminating at a live object",
                        {"owner": owner.id, "ref_index": index, "ref": reference, "terminal": terminal},
                    )
        for canonical in sorted(self.interned):
            terminal = self.resolve(canonical, seq, event)
            if terminal not in self.live:
                _violate(
                    "REFERENCE_RESOLUTION",
                    seq,
                    event,
                    "intern canonical terminating at a live object",
                    {"canonical": canonical, "terminal": terminal},
                )

    def _rewrite_identity(self, source: int, destination: int) -> None:
        object_ = self.live.pop(source)
        self.live[destination] = HeapObject(
            id=destination,
            kind=object_.kind,
            size=object_.size,
            generation=object_.generation,
            refs=tuple(destination if ref == source else ref for ref in object_.refs),
        )
        for owner_id, owner in tuple(self.live.items()):
            rewritten = tuple(destination if ref == source else ref for ref in owner.refs)
            if rewritten != owner.refs:
                self.live[owner_id] = HeapObject(
                    id=owner.id,
                    kind=owner.kind,
                    size=owner.size,
                    generation=owner.generation,
                    refs=rewritten,
                )
        if source in self.interned:
            self.interned.remove(source)
            self.interned.add(destination)

    def _forward_evidence_counters(
        self,
    ) -> list[
        tuple[
            Counter[tuple[int, int, int]],
            Counter[tuple[int, int, int]],
        ]
    ]:
        if self.active_collection is not None:
            counters = [
                (
                    self.collection_pending_heap_forwards,
                    self.collection_observed_heap_forwards,
                )
            ]
            counters.extend(
                (
                    transaction.pending_heap_forwards,
                    transaction.observed_heap_forwards,
                )
                for transaction in self.transactions
                if transaction.cause in IMMEDIATE_FORWARD_CAUSES
            )
            return counters
        return [
            (
                transaction.pending_heap_forwards,
                transaction.observed_heap_forwards,
            )
            for transaction in self.transactions
        ]

    @staticmethod
    def _rename_forward_owner(
        evidence: Counter[tuple[int, int, int]],
        source: int,
        destination: int,
    ) -> None:
        renamed: Counter[tuple[int, int, int]] = Counter()
        for (owner_id, from_id, to_id), count in evidence.items():
            renamed[
                (
                    destination if owner_id == source else owner_id,
                    from_id,
                    to_id,
                )
            ] += count
        evidence.clear()
        evidence.update(renamed)

    def _record_heap_forward_obligations(
        self, source: int, destination: int
    ) -> None:
        additions: Counter[tuple[int, int, int]] = Counter()
        for owner in self.live.values():
            count = owner.refs.count(source)
            if count:
                additions[(owner.id, source, destination)] += count
        for pending, observed in self._forward_evidence_counters():
            pending.update(additions)
            self._rename_forward_owner(pending, source, destination)
            self._rename_forward_owner(observed, source, destination)

    def _cancel_pending_owner(self, owner_id: int) -> None:
        for pending, observed in self._forward_evidence_counters():
            for key in tuple(pending):
                if key[0] == owner_id:
                    if observed[key] == 0:
                        del pending[key]
                    else:
                        pending[key] = observed[key]

    def _cancel_removed_heap_edges(
        self,
        owner_id: int,
        old_refs: tuple[int, ...],
        new_refs: tuple[int, ...],
        seq: int,
    ) -> None:
        removed = Counter(old_refs) - Counter(new_refs)
        for pending, observed in self._forward_evidence_counters():
            for removed_id, removed_count in sorted(removed.items()):
                terminal = self.resolve(removed_id, seq, "update")
                candidates = [
                    key
                    for key in sorted(pending)
                    if key[0] == owner_id
                    and self.resolve(key[2], seq, "update") == terminal
                ]
                remaining = removed_count
                for key in candidates:
                    if remaining == 0:
                        break
                    unobserved = max(0, pending[key] - observed[key])
                    cancelled = min(remaining, unobserved)
                    pending[key] -= cancelled
                    remaining -= cancelled
                    if pending[key] == 0:
                        del pending[key]

    def _require_heap_forwards_match(
        self,
        pending: Counter[tuple[int, int, int]],
        observed: Counter[tuple[int, int, int]],
        seq: int,
    ) -> None:
        if pending != observed:
            first_edge = min(
                edge
                for edge in set(pending) | set(observed)
                if pending[edge] != observed[edge]
            )
            owner_id, source_id, destination_id = first_edge
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "gc",
                {
                    "heap_forward_edge": {
                        "owner_id": owner_id,
                        "from_id": source_id,
                        "to_id": destination_id,
                        "count": pending[first_edge],
                    }
                },
                {
                    "heap_forward_edge": {
                        "owner_id": owner_id,
                        "from_id": source_id,
                        "to_id": destination_id,
                        "count": observed[first_edge],
                    }
                },
            )

    def _validate_live_metadata(
        self, event: dict[str, Any], seq: int, kind: str
    ) -> HeapObject:
        object_id = event["id"]
        object_ = self.live.get(object_id)
        if object_ is None:
            _violate("LIFECYCLE", seq, kind, "exact live source id", object_id)
        expected = {"size": object_.size, "gen": object_.generation}
        actual = {"size": event["size"], "gen": event["gen"]}
        if expected != actual:
            invariant = "MOVEMENT_CONSERVATION" if kind in {"relocate", "promote"} else "LIFECYCLE"
            _violate(invariant, seq, kind, expected, actual)
        return object_

    def _apply_alloc(self, event: dict[str, Any], seq: int) -> None:
        if self.transactions:
            _violate("MOVEMENT_CONSERVATION", seq, "alloc", "no allocation inside movement transaction", event["id"])
        object_id = event["id"]
        self._register_new_id(object_id, seq, "alloc")
        self._check_interval(object_id, event["size"], seq, "alloc")
        for reference in event["refs"]:
            terminal = self.resolve(reference, seq, "alloc")
            if terminal not in self.live:
                _violate("REFERENCE_RESOLUTION", seq, "alloc", "initial reference to live object", reference)
        self.live[object_id] = HeapObject(
            id=object_id,
            kind=event["object_kind"],
            size=event["size"],
            generation=event["gen"],
            refs=tuple(event["refs"]),
        )
        self._sample_peak()

    def _apply_die(self, event: dict[str, Any], seq: int) -> None:
        if not self.transactions:
            _violate("LIFECYCLE", seq, "die", "death inside movement/death-accounting transaction", "no transaction")
        if self.transactions[-1].cause not in DEATH_CAUSES:
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "die",
                {"death_accounting_cause": sorted(DEATH_CAUSES)},
                {"cause": self.transactions[-1].cause},
            )
        if self.active_collection is None:
            _violate(
                "LIFECYCLE",
                seq,
                "die",
                "death inside an active logical collection",
                None,
            )
        object_ = self._validate_live_metadata(event, seq, "die")
        base = object_.id & 0xFFFF_FFFF
        if self.last_death_base is not None and base <= self.last_death_base:
            _violate(
                "LIFECYCLE",
                seq,
                "die",
                {"source_base_greater_than": self.last_death_base},
                {"source_base": base},
            )
        self.last_death_base = base
        self._cancel_pending_owner(object_.id)
        self.live.pop(object_.id)
        self.tombstones[object_.id] = object_
        for transaction in self.transactions:
            for source, opening in transaction.start_objects.items():
                if self.resolve(source, seq, "die") == object_.id:
                    transaction.deaths[source] = opening
                    break

    def _validate_move_coordinates(self, event: dict[str, Any], seq: int) -> None:
        expected = {
            "from": event["id"] & 0xFFFF_FFFF,
            "to": event["to_id"] & 0xFFFF_FFFF,
        }
        actual = {"from": event["from"], "to": event["to"]}
        if expected != actual:
            _violate("MOVEMENT_CONSERVATION", seq, event["kind"], expected, actual)

    def _move_live_object(
        self, event: dict[str, Any], seq: int, *, promote: bool
    ) -> None:
        kind = event["kind"]
        object_ = self._validate_live_metadata(event, seq, kind)
        source = object_.id
        destination = event["to_id"]
        self._validate_move_coordinates(event, seq)
        if destination == source:
            if not promote:
                _violate("MOVEMENT_CONSERVATION", seq, kind, "changed relocation id", destination)
            self.live[source] = HeapObject(
                id=source,
                kind=object_.kind,
                size=object_.size,
                generation=1,
                refs=object_.refs,
            )
            return
        self._record_heap_forward_obligations(source, destination)
        self.live.pop(source)
        try:
            self._register_new_id(destination, seq, kind)
            self._check_interval(destination, object_.size, seq, kind)
        except TraceViolation:
            self.live[source] = object_
            raise
        self.live[source] = object_
        self._rewrite_identity(source, destination)
        if source in self.forwarding and self.forwarding[source] != destination:
            _violate("MOVEMENT_CONSERVATION", seq, kind, destination, self.forwarding[source])
        self.forwarding[source] = destination
        if promote:
            moved = self.live[destination]
            self.live[destination] = HeapObject(
                id=moved.id,
                kind=moved.kind,
                size=moved.size,
                generation=1,
                refs=moved.refs,
            )

    def _apply_relocate(self, event: dict[str, Any], seq: int) -> None:
        if not self.transactions:
            _violate("MOVEMENT_CONSERVATION", seq, "relocate", "relocation inside a movement transaction", "no transaction")
        transaction = self.transactions[-1]
        cause = transaction.cause
        expected_move_kind = (
            "growth"
            if cause in GROWTH_CAUSES
            else "compaction"
            if cause in COMPACTION_CAUSES
            else None
        )
        if event["move_kind"] != expected_move_kind:
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "relocate",
                {"cause": cause, "move_kind": expected_move_kind},
                {"move_kind": event["move_kind"]},
            )
        if event["from"] == event["to"]:
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "relocate",
                "changed base slot",
                {"from": event["from"], "to": event["to"]},
            )
        source = self.live.get(event["id"])
        if source is not None and cause in GROWTH_CAUSES:
            expected_kind = "map" if cause == "map_growth" else "builder"
            if source.kind != expected_kind:
                _violate(
                    "MOVEMENT_CONSERVATION",
                    seq,
                    "relocate",
                    {"cause": cause, "object_kind": expected_kind},
                    {"object_kind": source.kind},
                )
        self._move_live_object(event, seq, promote=False)
        transaction.relocations += 1
        self.relocations += 1

    def _apply_promote(self, event: dict[str, Any], seq: int) -> None:
        if not self.transactions:
            _violate("MOVEMENT_CONSERVATION", seq, "promote", "promotion inside a movement transaction", "no transaction")
        transaction = self.transactions[-1]
        if transaction.cause not in COMPACTION_CAUSES:
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "promote",
                {"compaction_cause": sorted(COMPACTION_CAUSES)},
                {"cause": transaction.cause},
            )
        source = event["id"]
        if source in self.live:
            if event["to_id"] != source:
                _violate(
                    "MOVEMENT_CONSERVATION",
                    seq,
                    "promote",
                    "changed-ID promotion immediately following its relocation",
                    {"id": source, "to_id": event["to_id"]},
                )
            self._move_live_object(event, seq, promote=True)
            transaction.promotions += 1
            return
        previous = self.last_event
        duplicate = (
            previous is not None
            and previous.get("kind") == "relocate"
            and all(previous.get(field) == event.get(field) for field in ("id", "size", "from", "to", "to_id"))
            and self.forwarding.get(source) == event["to_id"]
        )
        if not duplicate:
            _violate("LIFECYCLE", seq, "promote", "live source or adjacent matching relocation", source)
        destination = event["to_id"]
        object_ = self.live.get(destination)
        if object_ is None:
            _violate("LIFECYCLE", seq, "promote", "relocation destination still live", destination)
        if object_.size != event["size"] or object_.generation != 0:
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "promote",
                {"size": event["size"], "gen": 0},
                {"size": object_.size, "gen": object_.generation},
            )
        self._validate_move_coordinates(event, seq)
        self.live[destination] = HeapObject(
            id=object_.id,
            kind=object_.kind,
            size=object_.size,
            generation=1,
            refs=object_.refs,
        )
        transaction.promotions += 1

    def _apply_update(self, event: dict[str, Any], seq: int) -> None:
        object_id = event["id"]
        object_ = self.live.get(object_id)
        if object_ is None:
            _violate("LIFECYCLE", seq, "update", "exact live object id", object_id)
        expected = {"kind": object_.kind, "gen": object_.generation}
        actual = {"kind": event["object_kind"], "gen": event["gen"]}
        if expected != actual:
            _violate("LIFECYCLE", seq, "update", expected, actual)
        if self.transactions and event["size"] != object_.size:
            _violate(
                "MOVEMENT_CONSERVATION",
                seq,
                "update",
                {"size": object_.size, "reason": "no resize inside movement transaction"},
                event["size"],
            )
        new_refs = tuple(event["refs"])
        if event["size"] == object_.size and new_refs == object_.refs:
            _violate(
                "LIFECYCLE",
                seq,
                "update",
                "changed size or references",
                "no-op update",
            )
        self._cancel_removed_heap_edges(
            object_id, object_.refs, new_refs, seq
        )
        self.live.pop(object_id)
        try:
            self._check_interval(object_id, event["size"], seq, "update")
        except TraceViolation:
            self.live[object_id] = object_
            raise
        self.live[object_id] = HeapObject(
            id=object_id,
            kind=object_.kind,
            size=event["size"],
            generation=object_.generation,
            refs=new_refs,
        )
        if not self.transactions:
            self._require_resolved_references(seq, "update")
        self._sample_peak()

    def _apply_intern(self, event: dict[str, Any], seq: int) -> None:
        object_ = self._validate_live_metadata(event, seq, "intern")
        if object_.kind != "str":
            _violate("LIFECYCLE", seq, "intern", "str canonical", object_.kind)
        if event["hit"] == 1:
            if object_.id not in self.interned:
                _violate("LIFECYCLE", seq, "intern", "hit on registered canonical", object_.id)
        else:
            if object_.id in self.interned:
                _violate("LIFECYCLE", seq, "intern", "miss on unregistered canonical", object_.id)
            self.interned.add(object_.id)

    def _apply_evict(self, event: dict[str, Any], seq: int) -> None:
        object_id = event["id"]
        if object_id not in self.interned:
            _violate("LIFECYCLE", seq, "evict", "registered intern canonical", object_id)
        metadata = self.live.get(object_id, self.tombstones.get(object_id))
        if metadata is None:
            _violate("LIFECYCLE", seq, "evict", "live or same-run dead canonical", object_id)
        if metadata.size != event["size"] or metadata.generation != event["gen"]:
            _violate(
                "LIFECYCLE",
                seq,
                "evict",
                {"size": metadata.size, "gen": metadata.generation},
                {"size": event["size"], "gen": event["gen"]},
            )
        self.interned.remove(object_id)

    def _assert_boundary_counts(self, event: dict[str, Any], seq: int) -> None:
        expected = {"live_bytes": self.live_bytes(), "live_objects": len(self.live)}
        actual = {"live_bytes": event["live_bytes"], "live_objects": event["live_objects"]}
        if expected != actual:
            _violate("MOVEMENT_CONSERVATION", seq, "gc", expected, actual)

    def _apply_gc(self, event: dict[str, Any], seq: int) -> None:
        op = event["op"]
        if op == "pause":
            active_id = (
                self.active_collection[0]
                if self.active_collection is not None
                else None
            )
            if event["collection_id"] != active_id:
                _violate(
                    "LIFECYCLE",
                    seq,
                    "gc",
                    {"active_collection_id": active_id},
                    {"collection_id": event["collection_id"]},
                )
            self.pause_slices += 1
            return
        if op == "forward":
            active_id = (
                self.active_collection[0]
                if self.active_collection is not None
                else None
            )
            if event["collection_id"] != active_id:
                _violate(
                    "LIFECYCLE",
                    seq,
                    "gc",
                    {"active_collection_id": active_id},
                    {"collection_id": event["collection_id"]},
                )
            if self.active_collection is None:
                if not self.transactions:
                    _violate(
                        "LIFECYCLE",
                        seq,
                        "gc",
                        "forward inside a logical collection or growth transaction",
                        "no active context",
                    )
                if self.transactions[-1].cause not in GROWTH_CAUSES:
                    _violate(
                        "LIFECYCLE",
                        seq,
                        "gc",
                        {"growth_cause": sorted(GROWTH_CAUSES)},
                        {"cause": self.transactions[-1].cause},
                    )
            source_id = event["from_id"]
            destination_id = event["to_id"]
            if self.forwarding.get(source_id) != destination_id:
                _violate(
                    "MOVEMENT_CONSERVATION",
                    seq,
                    "gc",
                    {
                        "known_direct_forwarding": {
                            "from_id": source_id,
                            "to_id": self.forwarding.get(source_id),
                        }
                    },
                    {
                        "from_id": source_id,
                        "to_id": destination_id,
                    },
                )
            if event["forward_kind"] == "heap":
                owner_id = event["owner_id"]
                if owner_id not in self.live:
                    _violate(
                        "LIFECYCLE",
                        seq,
                        "gc",
                        "heap forward owner is an exact live object id",
                        owner_id,
                    )
                edge = (owner_id, source_id, destination_id)
                evidence_counters = self._forward_evidence_counters()
                for pending, observed in evidence_counters:
                    if observed[edge] >= pending[edge]:
                        _violate(
                            "MOVEMENT_CONSERVATION",
                            seq,
                            "gc",
                            {
                                "heap_forward_edge": {
                                    "owner_id": owner_id,
                                    "from_id": source_id,
                                    "to_id": destination_id,
                                    "maximum_count": pending[edge],
                                }
                            },
                            {
                                "next_count": observed[edge] + 1,
                            },
                        )
                for _, observed in evidence_counters:
                    observed[edge] += 1
            self.forwarded_references += 1
            self.forwarded_reference_totals[event["forward_kind"]] += 1
            return
        if op == "collection_begin":
            if self.active_collection is not None:
                _violate("LIFECYCLE", seq, "gc", "no active logical collection", self.active_collection)
            if self.transactions:
                _violate(
                    "MOVEMENT_CONSERVATION",
                    seq,
                    "gc",
                    "no movement transaction crossing collection_begin",
                    [transaction.transaction_id for transaction in self.transactions],
                )
            if event["collection_id"] != self.next_collection_id:
                _violate("EVENT_SEQUENCE", seq, "gc", self.next_collection_id, event["collection_id"])
            self._assert_boundary_counts(event, seq)
            self.active_collection = (event["collection_id"], event["collection_kind"])
            self.last_death_base = None
            self.collection_pending_heap_forwards.clear()
            self.collection_observed_heap_forwards.clear()
            self.next_collection_id += 1
            self.collections += 1
            self._require_resolved_references(seq, "collection_begin")
            return
        if op == "collection_end":
            expected_collection = self.active_collection
            actual_collection = (event["collection_id"], event["collection_kind"])
            if expected_collection != actual_collection:
                _violate("LIFECYCLE", seq, "gc", expected_collection, actual_collection)
            if self.transactions:
                _violate("MOVEMENT_CONSERVATION", seq, "gc", "all movement transactions closed", [tx.transaction_id for tx in self.transactions])
            self._assert_boundary_counts(event, seq)
            self._require_resolved_references(seq, "collection_end")
            self._require_heap_forwards_match(
                self.collection_pending_heap_forwards,
                self.collection_observed_heap_forwards,
                seq,
            )
            self.active_collection = None
            self.last_death_base = None
            self.collection_pending_heap_forwards.clear()
            self.collection_observed_heap_forwards.clear()
            self.collection_ends += 1
            return
        if op == "move_begin":
            expected_parent = self.transactions[-1].transaction_id if self.transactions else None
            expected_depth = len(self.transactions) + 1
            if event["transaction_id"] != self.next_transaction_id:
                _violate("EVENT_SEQUENCE", seq, "gc", self.next_transaction_id, event["transaction_id"])
            if event["parent_transaction_id"] != expected_parent or event["depth"] != expected_depth:
                _violate(
                    "MOVEMENT_CONSERVATION",
                    seq,
                    "gc",
                    {"parent_transaction_id": expected_parent, "depth": expected_depth},
                    {"parent_transaction_id": event["parent_transaction_id"], "depth": event["depth"]},
                )
            active_id = self.active_collection[0] if self.active_collection is not None else None
            if event["collection_id"] != active_id:
                _violate("LIFECYCLE", seq, "gc", active_id, event["collection_id"])
            active_kind = (
                self.active_collection[1]
                if self.active_collection is not None
                else None
            )
            cause = event["cause"]
            required_kind = (
                "major"
                if cause == "atomic_major" or cause.startswith("incremental_")
                else "minor"
                if cause == "atomic_minor"
                else None
            )
            if required_kind is not None and active_kind != required_kind:
                _violate(
                    "LIFECYCLE",
                    seq,
                    "gc",
                    {"cause": cause, "collection_kind": required_kind},
                    {"collection_kind": active_kind},
                )
            self._assert_boundary_counts(event, seq)
            self.transactions.append(
                MovementTransaction(
                    transaction_id=event["transaction_id"],
                    cause=cause,
                    collection_id=active_id,
                    start_objects=dict(self.live),
                    deaths={},
                )
            )
            self.next_transaction_id += 1
            return
        if op == "move_end":
            if not self.transactions:
                _violate("MOVEMENT_CONSERVATION", seq, "gc", "open movement transaction", "none")
            transaction = self.transactions[-1]
            expected = {
                "transaction_id": transaction.transaction_id,
                "parent_transaction_id": self.transactions[-2].transaction_id if len(self.transactions) > 1 else None,
                "depth": len(self.transactions),
                "cause": transaction.cause,
                "collection_id": transaction.collection_id,
            }
            actual = {field: event[field] for field in expected}
            if expected != actual:
                _violate("MOVEMENT_CONSERVATION", seq, "gc", expected, actual)
            self._verify_transaction(transaction, event, seq)
            if transaction.cause == "incremental_compaction_finalize":
                self._require_heap_forwards_match(
                    self.collection_pending_heap_forwards,
                    self.collection_observed_heap_forwards,
                    seq,
                )
            self.transactions.pop()
            self._require_resolved_references(seq, "move_end")
            return
        raise AssertionError(f"unhandled gc op {op}")

    def _verify_transaction(
        self, transaction: MovementTransaction, event: dict[str, Any], seq: int
    ) -> None:
        if transaction.cause in GROWTH_CAUSES:
            expected_activity = {"relocations": 1, "promotions": 0}
            actual_activity = {
                "relocations": transaction.relocations,
                "promotions": transaction.promotions,
            }
            if actual_activity != expected_activity:
                _violate(
                    "MOVEMENT_CONSERVATION",
                    seq,
                    "gc",
                    expected_activity,
                    actual_activity,
                )
        self._require_heap_forwards_match(
            transaction.pending_heap_forwards,
            transaction.observed_heap_forwards,
            seq,
        )
        start_bytes = sum(object_.size for object_ in transaction.start_objects.values()) * 8
        dead_bytes = sum(object_.size for object_ in transaction.deaths.values()) * 8
        expected_counts = {
            "live_bytes": start_bytes - dead_bytes,
            "live_objects": len(transaction.start_objects) - len(transaction.deaths),
        }
        actual_counts = {"live_bytes": self.live_bytes(), "live_objects": len(self.live)}
        if expected_counts != actual_counts:
            _violate("MOVEMENT_CONSERVATION", seq, "gc", expected_counts, actual_counts)
        reported = {"live_bytes": event["live_bytes"], "live_objects": event["live_objects"]}
        if actual_counts != reported:
            _violate("MOVEMENT_CONSERVATION", seq, "gc", actual_counts, reported)

        destinations: set[int] = set()
        for source, before in transaction.start_objects.items():
            if source in transaction.deaths:
                continue
            destination = self.resolve(source, seq, "move_end")
            after = self.live.get(destination)
            if after is None:
                _violate("MOVEMENT_CONSERVATION", seq, "gc", "one ending object for each survivor", {"source": source, "terminal": destination})
            if destination in destinations:
                _violate("MOVEMENT_CONSERVATION", seq, "gc", "one-to-one survivor destinations", destination)
            destinations.add(destination)
            expected_metadata = {"kind": before.kind, "size": before.size, "gen": before.generation}
            actual_metadata = {"kind": after.kind, "size": after.size, "gen": after.generation}
            generation_ok = after.generation == before.generation or (
                before.generation == 0 and after.generation == 1
            )
            if before.kind != after.kind or before.size != after.size or not generation_ok:
                _violate("MOVEMENT_CONSERVATION", seq, "gc", expected_metadata, actual_metadata)
        if destinations != set(self.live):
            _violate("MOVEMENT_CONSERVATION", seq, "gc", destinations, set(self.live))

    def apply(self, event: dict[str, Any], seq: int) -> None:
        kind = event["kind"]
        if event["tick"] < self.last_tick:
            _violate("EVENT_SEQUENCE", seq, kind, f"tick >= {self.last_tick}", event["tick"])
        self.last_tick = event["tick"]
        if kind in REQUIRED_EVENT_KINDS:
            self.event_totals[kind] += 1
        if kind == "alloc":
            self._apply_alloc(event, seq)
        elif kind == "die":
            self._apply_die(event, seq)
        elif kind == "relocate":
            self._apply_relocate(event, seq)
        elif kind == "promote":
            self._apply_promote(event, seq)
        elif kind == "update":
            self._apply_update(event, seq)
        elif kind == "intern":
            self._apply_intern(event, seq)
        elif kind == "evict":
            self._apply_evict(event, seq)
        elif kind == "gc":
            self._apply_gc(event, seq)
        elif kind in {"mark_slice", "trap", "verify_step"}:
            pass
        else:
            raise AssertionError(f"unhandled event kind {kind}")
        self.last_event = event

    def prepare_snapshots(self) -> None:
        previous_seq = -1
        for index, snapshot in enumerate(self.bundle.snapshots):
            _validate_snapshot_schema(snapshot, index)
            seq = snapshot["seq"]
            if seq < previous_seq or seq > len(self.bundle.events):
                _violate(
                    "SNAPSHOT_CONSISTENCY",
                    seq,
                    "snapshot",
                    f"nondecreasing seq in [0,{len(self.bundle.events)}]",
                    seq,
                )
            previous_seq = seq
            self.snapshots_by_seq.setdefault(seq, []).append(snapshot)
        event_count = len(self.bundle.events)
        interval = self.bundle.stats["snapshot_interval"]
        expected_sequences = [0, *range(interval, event_count, interval), event_count]
        actual_sequences = [snapshot["seq"] for snapshot in self.bundle.snapshots]
        allowed_sequences = [expected_sequences]
        if event_count > 0 and event_count % interval == 0:
            allowed_sequences.append([*expected_sequences, event_count])
        if actual_sequences not in allowed_sequences:
            _violate(
                "SNAPSHOT_CONSISTENCY",
                event_count,
                "snapshot",
                {"seqs": expected_sequences, "optional_duplicate_exit": len(allowed_sequences) == 2},
                {"seqs": actual_sequences},
            )

    def verify_snapshots_at(self, seq: int) -> None:
        for snapshot in self.snapshots_by_seq.get(seq, []):
            if snapshot is self.bundle.snapshots[0]:
                expected_tick = 0
            elif snapshot is self.bundle.snapshots[-1]:
                expected_tick = self.bundle.stats["ticks"]
            elif seq == len(self.bundle.events):
                expected_tick = (
                    self.bundle.events[-1]["tick"] if self.bundle.events else 0
                )
            else:
                expected_tick = self.bundle.events[seq]["tick"]
            if snapshot["tick"] != expected_tick:
                _violate("SNAPSHOT_CONSISTENCY", seq, "snapshot", expected_tick, snapshot["tick"])
            expected_live = [
                {
                    "id": object_.id,
                    "kind": object_.kind,
                    "size": object_.size,
                    "gen": object_.generation,
                    "refs": list(object_.refs),
                }
                for object_ in sorted(
                    self.live.values(), key=lambda item: item.id & 0xFFFF_FFFF
                )
            ]
            actual_live = snapshot["live"]
            if expected_live != actual_live:
                self._snapshot_difference(seq, expected_live, actual_live)
            self.snapshots_verified += 1

    @staticmethod
    def _snapshot_difference(
        seq: int,
        expected_live: list[dict[str, Any]],
        actual_live: list[dict[str, Any]],
    ) -> NoReturn:
        limit = min(len(expected_live), len(actual_live))
        for index in range(limit):
            expected_object = expected_live[index]
            actual_object = actual_live[index]
            for field in ("id", "kind", "size", "gen", "refs"):
                if expected_object[field] != actual_object[field]:
                    _violate(
                        "SNAPSHOT_CONSISTENCY",
                        seq,
                        "snapshot",
                        {"path": f"live[{index}].{field}", "value": expected_object[field]},
                        {"path": f"live[{index}].{field}", "value": actual_object[field]},
                    )
        _violate(
            "SNAPSHOT_CONSISTENCY",
            seq,
            "snapshot",
            {"live_objects": len(expected_live)},
            {"live_objects": len(actual_live)},
        )

    def verify_stats(self) -> CheckSummary:
        if self.transactions:
            _violate("MOVEMENT_CONSERVATION", len(self.bundle.events), "eof", "all movement transactions closed", [tx.transaction_id for tx in self.transactions])
        if self.active_collection is not None:
            _violate("LIFECYCLE", len(self.bundle.events), "eof", "logical collection closed", self.active_collection)
        if self.collections != self.collection_ends:
            _violate("LIFECYCLE", len(self.bundle.events), "eof", self.collections, self.collection_ends)
        self._require_resolved_references(len(self.bundle.events), "eof")

        if self.last_tick > self.bundle.stats["ticks"]:
            _violate(
                "STATS_CONSISTENCY",
                len(self.bundle.events),
                "stats",
                {"ticks_at_least_last_event": self.last_tick},
                {"ticks": self.bundle.stats["ticks"]},
            )

        actual_values = {
            "live_bytes_final": self.live_bytes(),
            "forwarded_reference_count": self.forwarded_references,
            "forwarded_reference_totals": {
                kind: self.forwarded_reference_totals[kind]
                for kind in FORWARD_KIND_ORDER
            },
            "pause_slices": self.pause_slices,
            "collection_count": self.collections,
            "ticks": self.bundle.snapshots[-1]["tick"],
            "peak_live_bytes": self.peak_live_bytes,
        }
        for field, actual in actual_values.items():
            expected = self.bundle.stats[field]
            if expected != actual:
                _violate("STATS_CONSISTENCY", len(self.bundle.events), "stats", {field: actual}, {field: expected})
        actual_totals = {kind: self.event_totals[kind] for kind in REQUIRED_EVENT_KINDS}
        if self.bundle.stats["event_totals"] != actual_totals:
            _violate(
                "STATS_CONSISTENCY",
                len(self.bundle.events),
                "stats",
                actual_totals,
                self.bundle.stats["event_totals"],
            )
        return CheckSummary(
            events=len(self.bundle.events),
            collections=self.collections,
            relocations=self.relocations,
            peak_live_bytes=self.peak_live_bytes,
            final_live_bytes=self.live_bytes(),
            snapshots_verified=self.snapshots_verified,
        )


def check_trace_bundle(
    bundle: TraceBundle, source: Path | str | None = None
) -> CheckSummary:
    del source
    _validate_stats_schema(bundle.stats)
    replay = _Replay(bundle)
    replay.prepare_snapshots()
    replay.verify_snapshots_at(0)
    for seq, event in enumerate(bundle.events):
        _validate_event_schema(event, seq)
        replay.apply(event, seq)
        replay.verify_snapshots_at(seq + 1)
    return replay.verify_stats()


def check_trace_dir(trace_directory: Path | str) -> CheckSummary:
    directory = Path(trace_directory)
    return check_trace_bundle(load_trace_bundle(directory), directory)


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="replay a lang_trace bundle and verify conservation"
    )
    parser.add_argument(
        "trace_directory",
        type=Path,
        help="directory containing events.jsonl, snapshots.jsonl, and stats.json",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        summary = check_trace_dir(args.trace_directory)
    except TraceViolation as violation:
        print(violation, file=sys.stderr)
        return 1
    print(summary.format_line())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
