#!/usr/bin/env python3
"""Non-vacuity proof for the trace conservation checker."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import sys
import tempfile
from typing import Callable

sys.dont_write_bytecode = True
import trace_conservation_check as checker


class SelfTestFailure(RuntimeError):
    """Raised when the negative-test matrix is incomplete or ineffective."""


Mutation = Callable[[checker.TraceBundle], None]


def _json_line(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n"


def _write_bundle(bundle: checker.TraceBundle, directory: Path) -> None:
    directory.mkdir()
    with (directory / "events.jsonl").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        stream.write("".join(_json_line(event) for event in bundle.events))
    with (directory / "snapshots.jsonl").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        stream.write("".join(_json_line(snapshot) for snapshot in bundle.snapshots))
    with (directory / "stats.json").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        stream.write(_json_line(bundle.stats))


def _use_endpoint_snapshots(bundle: checker.TraceBundle) -> None:
    start = bundle.snapshots[0]
    exit_snapshot = bundle.snapshots[-1]
    start["seq"] = 0
    exit_snapshot["seq"] = len(bundle.events)
    bundle.snapshots = [start, exit_snapshot]
    bundle.stats["snapshot_interval"] = max(len(bundle.events), 1)


def _delete_event_and_repair_surface(
    bundle: checker.TraceBundle, index: int
) -> None:
    removed = bundle.events.pop(index)
    for seq, event in enumerate(bundle.events):
        event["seq"] = seq
    for snapshot in bundle.snapshots:
        if snapshot["seq"] > index:
            snapshot["seq"] -= 1
    totals = bundle.stats["event_totals"]
    if removed["kind"] in totals:
        totals[removed["kind"]] -= 1
    _use_endpoint_snapshots(bundle)


def _same_move(left: dict[str, object], right: dict[str, object]) -> bool:
    return all(
        left.get(field) == right.get(field)
        for field in ("id", "to_id", "size", "from", "to")
    )


def _drop_relocation(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events):
        if event.get("kind") != "relocate" or event.get("move_kind") != "compaction":
            continue
        if index + 1 < len(bundle.events):
            following = bundle.events[index + 1]
            if following.get("kind") == "promote" and _same_move(event, following):
                continue
        _delete_event_and_repair_surface(bundle, index)
        return
    raise SelfTestFailure("real trace has no standalone compaction relocation to drop")


def _drop_paired_relocation(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events[:-1]):
        following = bundle.events[index + 1]
        if (
            event.get("kind") == "relocate"
            and following.get("kind") == "promote"
            and _same_move(event, following)
        ):
            _delete_event_and_repair_surface(bundle, index)
            return
    raise SelfTestFailure("real trace has no promotion-paired relocation to drop")


def _tamper_relocation_size(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if event.get("kind") == "relocate":
            event["size"] += 1
            return
    raise SelfTestFailure("real trace has no relocation whose size can be corrupted")


def _retarget_reference_to_dead_id(bundle: checker.TraceBundle) -> None:
    dead_ids: list[int] = []
    for event in bundle.events:
        if event.get("kind") == "die":
            dead_ids.append(event["id"])
            continue
        refs = event.get("refs")
        if dead_ids and event.get("kind") in {"alloc", "update"} and refs:
            refs[0] = dead_ids[-1]
            return
    raise SelfTestFailure(
        "real trace has no post-death object state with a reference to retarget"
    )


def _delete_death(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events):
        if event.get("kind") == "die":
            _delete_event_and_repair_surface(bundle, index)
            return
    raise SelfTestFailure("real trace has no death event to delete")


def _tamper_snapshot(bundle: checker.TraceBundle) -> None:
    for snapshot in bundle.snapshots:
        if snapshot["live"]:
            snapshot["live"][0]["size"] += 1
            return
    raise SelfTestFailure("real trace has no non-empty snapshot to corrupt")


def _delete_periodic_snapshot(bundle: checker.TraceBundle) -> None:
    final_seq = len(bundle.events)
    for index, snapshot in enumerate(bundle.snapshots):
        if snapshot["seq"] not in {0, final_seq}:
            bundle.snapshots.pop(index)
            return
    raise SelfTestFailure("real trace has no periodic snapshot to delete")


def _inflate_stats(bundle: checker.TraceBundle) -> None:
    bundle.stats["pause_slices"] += 1


def _break_event_sequence(bundle: checker.TraceBundle) -> None:
    if len(bundle.events) < 2:
        raise SelfTestFailure("real trace is too short for sequence corruption")
    index = len(bundle.events) // 2
    bundle.events[index]["seq"] += 1


def _put_event_after_exit_tick(bundle: checker.TraceBundle) -> None:
    if not bundle.events:
        raise SelfTestFailure("real trace has no event tick to corrupt")
    bundle.events[-1]["tick"] = bundle.stats["ticks"] + 1


def _delete_all_forward_events(bundle: checker.TraceBundle) -> None:
    indices = [
        index
        for index, event in enumerate(bundle.events)
        if event.get("kind") == "gc" and event.get("op") == "forward"
    ]
    if not indices:
        raise SelfTestFailure("real trace has no forward evidence to delete")
    for index in reversed(indices):
        bundle.events.pop(index)
    for seq, event in enumerate(bundle.events):
        event["seq"] = seq
    bundle.stats["forwarded_reference_count"] = 0
    _use_endpoint_snapshots(bundle)


def _delete_one_heap_forward(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events):
        if (
            event.get("kind") == "gc"
            and event.get("op") == "forward"
            and event.get("forward_kind") == "heap"
        ):
            _delete_event_and_repair_surface(bundle, index)
            bundle.stats["forwarded_reference_count"] -= 1
            return
    raise SelfTestFailure("real trace has no heap-forward evidence to delete")


def _tamper_heap_forward_mapping(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if (
            event.get("kind") == "gc"
            and event.get("op") == "forward"
            and event.get("forward_kind") == "heap"
        ):
            event["to_id"] += 1
            return
    raise SelfTestFailure("real trace has no heap-forward mapping to corrupt")


def _tamper_heap_forward_owner(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if (
            event.get("kind") == "gc"
            and event.get("op") == "forward"
            and event.get("forward_kind") == "heap"
        ):
            event["owner_id"] = event["from_id"]
            return
    raise SelfTestFailure("real trace has no heap-forward owner to corrupt")


def _duplicate_heap_forward(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events):
        if (
            event.get("kind") == "gc"
            and event.get("op") == "forward"
            and event.get("forward_kind") == "heap"
        ):
            bundle.events.insert(index + 1, copy.deepcopy(event))
            for seq, item in enumerate(bundle.events):
                item["seq"] = seq
            bundle.stats["forwarded_reference_count"] += 1
            _use_endpoint_snapshots(bundle)
            return
    raise SelfTestFailure("real trace has no heap forward to duplicate")


def _delete_forward_before_later_update(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events):
        if (
            event.get("kind") != "gc"
            or event.get("op") != "forward"
            or event.get("forward_kind") != "heap"
        ):
            continue
        if any(
            later.get("kind") == "update"
            and later.get("id") == event["owner_id"]
            for later in bundle.events[index + 1 :]
        ):
            _delete_event_and_repair_surface(bundle, index)
            bundle.stats["forwarded_reference_count"] -= 1
            return
    raise SelfTestFailure(
        "real trace has no heap forward followed by an owner update"
    )


def _substitute_root_for_heap(bundle: checker.TraceBundle) -> None:
    for heap_index, heap_event in enumerate(bundle.events):
        if (
            heap_event.get("kind") != "gc"
            or heap_event.get("op") != "forward"
            or heap_event.get("forward_kind") != "heap"
        ):
            continue
        for root_event in bundle.events:
            if (
                root_event.get("kind") == "gc"
                and root_event.get("op") == "forward"
                and root_event.get("forward_kind") == "root"
                and root_event.get("from_id") == heap_event["from_id"]
                and root_event.get("to_id") == heap_event["to_id"]
            ):
                _delete_event_and_repair_surface(bundle, heap_index)
                root_event["owner_id"] = heap_event["owner_id"]
                root_event["forward_kind"] = "heap"
                bundle.stats["forwarded_reference_count"] -= 1
                return
    raise SelfTestFailure(
        "real trace has no root/heap forwarding pair for one mapping"
    )


def _mislabel_gc_collection(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if (
            event.get("kind") == "gc"
            and event.get("op") in {"pause", "forward"}
            and event.get("collection_id") is not None
        ):
            event["collection_id"] += 1_000_000
            return
    raise SelfTestFailure("real trace has no in-collection GC counter event to mislabel")


def _move_death_into_non_death_phase(bundle: checker.TraceBundle) -> None:
    for begin_index, begin in enumerate(bundle.events):
        if begin.get("kind") != "gc" or begin.get("op") != "move_begin":
            continue
        transaction_id = begin["transaction_id"]
        end_index = None
        saw_death = False
        for index in range(begin_index + 1, len(bundle.events)):
            event = bundle.events[index]
            if event.get("kind") == "die":
                saw_death = True
            if (
                event.get("kind") == "gc"
                and event.get("op") == "move_end"
                and event.get("transaction_id") == transaction_id
            ):
                end_index = index
                break
        if saw_death and end_index is not None:
            begin["cause"] = "incremental_compaction_step"
            bundle.events[end_index]["cause"] = "incremental_compaction_step"
            return
    raise SelfTestFailure("real trace has no death-accounting transaction to relabel")


def _move_compaction_into_finalize_phase(bundle: checker.TraceBundle) -> None:
    for begin_index, begin in enumerate(bundle.events):
        if (
            begin.get("kind") != "gc"
            or begin.get("op") != "move_begin"
            or begin.get("cause") != "incremental_compaction_step"
        ):
            continue
        transaction_id = begin["transaction_id"]
        saw_move = False
        for event in bundle.events[begin_index + 1 :]:
            if event.get("kind") in {"relocate", "promote"}:
                saw_move = True
            if (
                event.get("kind") == "gc"
                and event.get("op") == "move_end"
                and event.get("transaction_id") == transaction_id
            ):
                if saw_move:
                    begin["cause"] = "incremental_compaction_finalize"
                    event["cause"] = "incremental_compaction_finalize"
                    return
                break
    raise SelfTestFailure("real trace has no incremental compaction move to relabel")


def _mislabel_growth_object_kind(bundle: checker.TraceBundle) -> None:
    for begin_index, begin in enumerate(bundle.events):
        if (
            begin.get("kind") != "gc"
            or begin.get("op") != "move_begin"
            or begin.get("cause") not in {"map_growth", "builder_growth"}
        ):
            continue
        transaction_id = begin["transaction_id"]
        replacement = (
            "map_growth" if begin["cause"] == "builder_growth" else "builder_growth"
        )
        for event in bundle.events[begin_index + 1 :]:
            if (
                event.get("kind") == "gc"
                and event.get("op") == "move_end"
                and event.get("transaction_id") == transaction_id
            ):
                begin["cause"] = replacement
                event["cause"] = replacement
                return
    raise SelfTestFailure("real trace has no growth transaction to mislabel")


def _flip_collection_kind(bundle: checker.TraceBundle) -> None:
    for begin in bundle.events:
        if begin.get("kind") != "gc" or begin.get("op") != "collection_begin":
            continue
        collection_id = begin["collection_id"]
        replacement = "minor" if begin["collection_kind"] == "major" else "major"
        for event in bundle.events:
            if (
                event.get("kind") == "gc"
                and event.get("op") == "collection_end"
                and event.get("collection_id") == collection_id
            ):
                begin["collection_kind"] = replacement
                event["collection_kind"] = replacement
                return
    raise SelfTestFailure("real trace has no complete collection to relabel")


def _swap_adjacent_deaths(bundle: checker.TraceBundle) -> None:
    for left, right in zip(bundle.events, bundle.events[1:]):
        if left.get("kind") != "die" or right.get("kind") != "die":
            continue
        left_base = left["id"] & 0xFFFF_FFFF
        right_base = right["id"] & 0xFFFF_FFFF
        if left_base >= right_base:
            continue
        for field in ("id", "size", "gen"):
            left[field], right[field] = right[field], left[field]
        return
    raise SelfTestFailure("real trace has no adjacent ascending deaths to swap")


def _replace_scalar(value: object, old: int, new: int) -> object:
    if type(value) is int and value == old:
        return new
    if isinstance(value, list):
        return [_replace_scalar(item, old, new) for item in value]
    if isinstance(value, dict):
        return {key: _replace_scalar(item, old, new) for key, item in value.items()}
    return value


def _jump_first_slot_generation(bundle: checker.TraceBundle) -> None:
    ids_by_base: dict[int, set[int]] = {}
    for event in bundle.events:
        candidates = [event.get("id"), event.get("to_id")]
        refs = event.get("refs")
        if isinstance(refs, list):
            candidates.extend(refs)
        for object_id in candidates:
            if type(object_id) is int and object_id >= (1 << 32):
                ids_by_base.setdefault(object_id & 0xFFFF_FFFF, set()).add(object_id)
    for event in bundle.events:
        if event.get("kind") != "alloc":
            continue
        object_id = event["id"]
        if len(ids_by_base.get(object_id & 0xFFFF_FFFF, set())) != 1:
            continue
        replacement = object_id + (5 << 32)
        bundle.events = _replace_scalar(bundle.events, object_id, replacement)
        bundle.snapshots = _replace_scalar(bundle.snapshots, object_id, replacement)
        return
    raise SelfTestFailure("real trace has no single-generation slot to jump")


def _unknown_verify_step(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if event.get("kind") == "verify_step":
            event["check"] = "unknown_validator"
            return
    raise SelfTestFailure("real trace has no verify_step label to corrupt")


def _duplicate_update(bundle: checker.TraceBundle) -> None:
    for index, event in enumerate(bundle.events):
        if event.get("kind") != "update":
            continue
        bundle.events.insert(index + 1, copy.deepcopy(event))
        for seq, item in enumerate(bundle.events):
            item["seq"] = seq
        _use_endpoint_snapshots(bundle)
        return
    raise SelfTestFailure("real trace has no update event to duplicate")


def _malform_object_kind(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if event.get("kind") == "alloc":
            event["object_kind"] = []
            return
    raise SelfTestFailure("real trace has no allocation kind to malform")


def _impossible_fixed_width(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if event.get("kind") == "alloc" and event.get("object_kind") == "pair":
            event["size"] = 2
            return
    raise SelfTestFailure("real trace has no fixed-width pair allocation to corrupt")


def _inject_diagnostic_label(bundle: checker.TraceBundle) -> None:
    if not bundle.events:
        raise SelfTestFailure("real trace has no event diagnostic to corrupt")
    bundle.events[0]["kind"] = "forged\nOK events=999"
    bundle.events[0].pop("src_pos")


def _insert_contextless_forward(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if event.get("kind") == "gc" and event.get("op") == "forward":
            forged = copy.deepcopy(event)
            forged["tick"] = 0
            forged["collection_id"] = None
            bundle.events.insert(0, forged)
            for seq, item in enumerate(bundle.events):
                item["seq"] = seq
            bundle.stats["forwarded_reference_count"] += 1
            _use_endpoint_snapshots(bundle)
            return
    raise SelfTestFailure("real trace has no forward event to move out of context")


def _impossible_builder_width(bundle: checker.TraceBundle) -> None:
    for event in bundle.events:
        if event.get("kind") == "alloc" and event.get("object_kind") == "builder":
            event["size"] = 4
            return
    raise SelfTestFailure("real trace has no Builder allocation to corrupt")


def _sampled_verify_stats(bundle: checker.TraceBundle) -> dict[str, object]:
    verify_events = bundle.stats.get("verify_events")
    if (
        not isinstance(verify_events, dict)
        or verify_events.get("mode") != "sampled"
    ):
        raise SelfTestFailure("real trace does not use sampled verify events")
    return verify_events


def _delete_sampled_retained_head(bundle: checker.TraceBundle) -> None:
    verify_events = _sampled_verify_stats(bundle)
    for index, event in enumerate(bundle.events):
        if event.get("kind") != "verify_step" or event.get("verify_index") != 0:
            continue

        scope_ledger = verify_events
        scope_field = "unscoped_emitted_count"
        collection_id = event.get("collection_id")
        if collection_id is not None:
            for collection_end in bundle.events:
                if (
                    collection_end.get("kind") == "gc"
                    and collection_end.get("op") == "collection_end"
                    and collection_end.get("collection_id") == collection_id
                ):
                    scope_ledger = collection_end
                    scope_field = "verify_emitted_count"
                    break
            else:
                raise SelfTestFailure(
                    "sampled retained head has no matching collection_end ledger"
                )

        ledgers = (
            (verify_events, "emitted_count"),
            (scope_ledger, scope_field),
        )
        for ledger, field in ledgers:
            count = ledger.get(field)
            if type(count) is not int or count <= 0:
                raise SelfTestFailure(
                    f"sampled retained head has no positive {field} ledger"
                )

        _delete_event_and_repair_surface(bundle, index)
        for ledger, field in ledgers:
            ledger[field] -= 1
        return
    raise SelfTestFailure("real sampled trace has no retained verify head to delete")


def _declare_sampled_stream_full(bundle: checker.TraceBundle) -> None:
    verify_events = _sampled_verify_stats(bundle)
    verify_events["mode"] = "full"
    verify_events["retention_rule"] = "all"


def _tamper_sampled_true_total(bundle: checker.TraceBundle) -> None:
    verify_events = _sampled_verify_stats(bundle)
    true_count = verify_events.get("true_count")
    if type(true_count) is not int:
        raise SelfTestFailure("real sampled trace has no true verify total to corrupt")
    verify_events["true_count"] = true_count + 1


def _tamper_sampled_head_check_index(bundle: checker.TraceBundle) -> None:
    _sampled_verify_stats(bundle)
    for event in bundle.events:
        if event.get("kind") != "verify_step" or event.get("verify_index") != 0:
            continue
        check_index = event.get("check_index")
        if type(check_index) is not int:
            raise SelfTestFailure("sampled retained head has no check index")
        event["check_index"] = check_index + 1
        return
    raise SelfTestFailure("real sampled trace has no first retained verify event")


def _tamper_sampled_unscoped_total_to_uint64_max(
    bundle: checker.TraceBundle,
) -> None:
    verify_events = _sampled_verify_stats(bundle)
    verify_events["true_count"] = checker.UINT64_MAX
    verify_events["unscoped_true_count"] = checker.UINT64_MAX


CORRUPTIONS: tuple[tuple[str, str, Mutation], ...] = (
    ("drop_relocation", "MOVEMENT_CONSERVATION", _drop_relocation),
    ("drop_paired_relocation", "MOVEMENT_CONSERVATION", _drop_paired_relocation),
    ("relocation_size", "MOVEMENT_CONSERVATION", _tamper_relocation_size),
    ("reference_to_dead", "REFERENCE_RESOLUTION", _retarget_reference_to_dead_id),
    ("delete_death", "MOVEMENT_CONSERVATION", _delete_death),
    ("snapshot_entry", "SNAPSHOT_CONSISTENCY", _tamper_snapshot),
    ("delete_periodic_snapshot", "SNAPSHOT_CONSISTENCY", _delete_periodic_snapshot),
    ("stats_counter", "STATS_CONSISTENCY", _inflate_stats),
    ("event_sequence", "EVENT_SEQUENCE", _break_event_sequence),
    ("event_after_exit_tick", "STATS_CONSISTENCY", _put_event_after_exit_tick),
    ("delete_forward_evidence", "MOVEMENT_CONSERVATION", _delete_all_forward_events),
    ("delete_heap_forward", "MOVEMENT_CONSERVATION", _delete_one_heap_forward),
    ("heap_forward_mapping", "MOVEMENT_CONSERVATION", _tamper_heap_forward_mapping),
    ("heap_forward_owner", "LIFECYCLE", _tamper_heap_forward_owner),
    ("duplicate_heap_forward", "MOVEMENT_CONSERVATION", _duplicate_heap_forward),
    (
        "delete_forward_before_later_update",
        "MOVEMENT_CONSERVATION",
        _delete_forward_before_later_update,
    ),
    (
        "substitute_root_for_heap",
        "STATS_CONSISTENCY",
        _substitute_root_for_heap,
    ),
    ("gc_collection_id", "LIFECYCLE", _mislabel_gc_collection),
    ("death_in_move_phase", "MOVEMENT_CONSERVATION", _move_death_into_non_death_phase),
    ("move_in_finalize_phase", "MOVEMENT_CONSERVATION", _move_compaction_into_finalize_phase),
    ("growth_kind_label", "MOVEMENT_CONSERVATION", _mislabel_growth_object_kind),
    ("collection_kind", "LIFECYCLE", _flip_collection_kind),
    ("death_order", "LIFECYCLE", _swap_adjacent_deaths),
    ("slot_generation", "LIFECYCLE", _jump_first_slot_generation),
    ("verify_step_label", "SCHEMA", _unknown_verify_step),
    ("duplicate_update", "LIFECYCLE", _duplicate_update),
    ("malformed_object_kind", "SCHEMA", _malform_object_kind),
    ("impossible_fixed_width", "SCHEMA", _impossible_fixed_width),
    ("diagnostic_injection", "SCHEMA", _inject_diagnostic_label),
    ("contextless_forward", "LIFECYCLE", _insert_contextless_forward),
    ("impossible_builder_width", "SCHEMA", _impossible_builder_width),
    (
        "sampled_retained_head_deletion",
        "VERIFY_SAMPLING",
        _delete_sampled_retained_head,
    ),
    ("sampled_stream_declared_full", "VERIFY_SAMPLING", _declare_sampled_stream_full),
    ("sampled_true_total", "VERIFY_SAMPLING", _tamper_sampled_true_total),
    (
        "sampled_head_check_index",
        "VERIFY_SAMPLING",
        _tamper_sampled_head_check_index,
    ),
    (
        "sampled_uint64_max_unscoped_total",
        "VERIFY_SAMPLING",
        _tamper_sampled_unscoped_total_to_uint64_max,
    ),
)


def run_selftest(trace_directories: list[Path]) -> None:
    pristine_bundles: list[checker.TraceBundle] = []
    for trace_directory in trace_directories:
        pristine = checker.load_trace_bundle(trace_directory)
        summary = checker.check_trace_bundle(pristine, trace_directory)
        pristine_bundles.append(pristine)
        print(f"PASS pristine bundle={trace_directory.name} {summary.format_line()}")

    duplicate_exit = copy.deepcopy(pristine_bundles[0])
    periodic_final = copy.deepcopy(duplicate_exit.snapshots[-1])
    periodic_final["tick"] = duplicate_exit.events[-1]["tick"]
    duplicate_exit.stats["snapshot_interval"] = len(duplicate_exit.events)
    duplicate_exit.snapshots = [
        duplicate_exit.snapshots[0],
        periodic_final,
        duplicate_exit.snapshots[-1],
    ]
    with tempfile.TemporaryDirectory(
        prefix="trace-conservation-duplicate-exit-"
    ) as temporary:
        duplicate_directory = Path(temporary) / "bundle"
        _write_bundle(duplicate_exit, duplicate_directory)
        duplicate_summary = checker.check_trace_dir(duplicate_directory)
    print(
        "PASS duplicate_final_snapshot "
        f"snapshots_verified={duplicate_summary.snapshots_verified}"
    )

    unicode_string = copy.deepcopy(pristine_bundles[0])
    for event in unicode_string.events:
        if isinstance(event.get("src_pos"), dict):
            event["src_pos"]["fn"] = "json\u2028label"
            break
    else:
        raise SelfTestFailure("real trace has no source position for JSON string proof")
    with tempfile.TemporaryDirectory(
        prefix="trace-conservation-unicode-json-"
    ) as temporary:
        unicode_directory = Path(temporary) / "bundle"
        _write_bundle(unicode_string, unicode_directory)
        checker.check_trace_dir(unicode_directory)
    print("PASS unicode_json_string")

    for name, expected_invariant, mutate in CORRUPTIONS:
        corrupted = None
        unavailable: list[str] = []
        for trace_directory, pristine in zip(
            trace_directories, pristine_bundles
        ):
            candidate = copy.deepcopy(pristine)
            try:
                mutate(candidate)
            except SelfTestFailure as error:
                unavailable.append(f"{trace_directory.name}: {error}")
                continue
            corrupted = candidate
            break
        if corrupted is None:
            raise SelfTestFailure(f"{name}: " + "; ".join(unavailable))
        with tempfile.TemporaryDirectory(
            prefix=f"trace-conservation-{name}-"
        ) as temporary:
            corrupted_directory = Path(temporary) / "bundle"
            _write_bundle(corrupted, corrupted_directory)
            try:
                checker.check_trace_dir(corrupted_directory)
            except checker.TraceViolation as violation:
                diagnostic = str(violation)
                if (
                    not diagnostic.startswith("VIOLATION seq=")
                    or "\n" in diagnostic
                    or "\r" in diagnostic
                ):
                    raise SelfTestFailure(
                        f"{name}: unsafe multi-line diagnostic: {diagnostic!r}"
                    ) from violation
                if violation.invariant != expected_invariant:
                    raise SelfTestFailure(
                        f"{name}: expected {expected_invariant}, "
                        f"got {violation.invariant}: {violation}"
                    ) from violation
                print(f"PASS {name} rejected={violation.invariant}")
            else:
                raise SelfTestFailure(f"{name}: corrupted trace was accepted")

    print(f"SELFTEST OK corruptions={len(CORRUPTIONS)}")


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="prove trace_conservation_check rejects corrupted real traces"
    )
    parser.add_argument(
        "trace_directories",
        type=Path,
        nargs="+",
        help="real lang_trace bundles used as pristine baselines",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        run_selftest(args.trace_directories)
    except (SelfTestFailure, checker.TraceViolation, OSError) as error:
        print(f"SELFTEST FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
