#!/usr/bin/env python3
"""Deterministic reader for a generated showcase trace bundle."""

import json
import sys
from pathlib import Path


def _jsonl(path):
    with path.open(encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def load_bundle(directory):
    root = Path(directory)
    events = _jsonl(root / "events.jsonl")
    snapshots = _jsonl(root / "snapshots.jsonl")
    return events, snapshots


def _resolved(object_id, forwarding):
    seen = set()
    while object_id in forwarding:
        if object_id in seen:
            raise ValueError("forwarding cycle")
        seen.add(object_id)
        object_id = forwarding[object_id]
    return object_id


def _object(record, forwarding):
    return {
        "id": record["id"],
        "kind": record.get("object_kind", record.get("kind")),
        "size": record["size"],
        "gen": record["gen"],
        "refs": [_resolved(ref, forwarding) for ref in record.get("refs", [])],
    }


def replay(events, seek, tick, end_seq=None):
    forwarding = {}

    # Earlier forwarding aliases may still be named by later reference fields.
    for event in events:
        if event["seq"] >= seek["seq"]:
            break
        if event["kind"] in ("relocate", "promote"):
            source = event["id"]
            destination = event.get("to_id", source)
            if source != destination:
                forwarding[source] = destination

    heap = {}
    for record in seek.get("live", []):
        value = _object(record, forwarding)
        heap[value["id"]] = value

    def resolve_heap_refs():
        for value in heap.values():
            value["refs"] = [_resolved(ref, forwarding) for ref in value["refs"]]

    for event in events:
        seq = event["seq"]
        if seq < seek["seq"]:
            continue
        if end_seq is not None:
            if seq >= end_seq:
                break
        elif event["tick"] > tick:
            break

        kind = event["kind"]
        if kind == "alloc":
            value = _object(event, forwarding)
            heap[value["id"]] = value
        elif kind == "die":
            heap.pop(event["id"], None)
        elif kind == "relocate":
            source = event["id"]
            destination = event["to_id"]
            value = heap.pop(source, None)
            forwarding[source] = destination
            if value is not None:
                value["id"] = destination
                heap[destination] = value
            resolve_heap_refs()
        elif kind == "promote":
            source = event["id"]
            destination = event.get("to_id", source)
            value = heap.pop(source, None)
            if value is None and source != destination:
                value = heap.pop(destination, None)
            if source != destination:
                forwarding[source] = destination
            if value is not None:
                value["id"] = destination
                value["gen"] = 1
                heap[destination] = value
            resolve_heap_refs()
        elif kind == "update":
            object_id = event["id"]
            if object_id in heap:
                value = heap[object_id]
                value["size"] = event["size"]
                value["refs"] = [_resolved(ref, forwarding) for ref in event["refs"]]
        # verify_step sampling does not affect materialized heap state.
    return heap


def materialize(directory, tick):
    events, snapshots = load_bundle(directory)
    eligible = [snapshot for snapshot in snapshots if snapshot["tick"] <= tick]
    if eligible:
        seek = eligible[0]
        for candidate in eligible[1:]:
            if (candidate["tick"], candidate["seq"]) >= (seek["tick"], seek["seq"]):
                seek = candidate
    else:
        seek = {"tick": 0, "seq": 0, "live": []}

    # Snapshot seq names the next event; exact ticks preserve that seq boundary.
    end_seq = seek["seq"] if eligible and seek["tick"] == tick else None
    return replay(events, seek, tick, end_seq=end_seq)


def render(tick, heap):
    objects = sorted(heap.values(), key=lambda item: (item["id"] & 0xFFFFFFFF, item["id"]))
    return json.dumps({"tick": tick, "live": objects}, separators=(",", ":"))


def main(argv):
    if len(argv) != 3:
        raise SystemExit("usage: reader.py <trace-dir> <tick>")
    try:
        tick = int(argv[2])
    except ValueError:
        raise SystemExit("tick must be a nonnegative integer") from None
    if tick < 0:
        raise SystemExit("tick must be a nonnegative integer")
    print(render(tick, materialize(argv[1], tick)))


if __name__ == "__main__":
    main(sys.argv)
