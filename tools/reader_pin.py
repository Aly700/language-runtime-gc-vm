#!/usr/bin/env python3
"""Pin reference-reader replay against every recorded showcase snapshot."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
from types import ModuleType
from typing import Any


sys.dont_write_bytecode = True


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def import_reader(path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location("showcase_reader_pin_target", path)
    require(spec is not None and spec.loader is not None, f"cannot import reader {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def expected_output(raw_snapshot: bytes, tick: int) -> bytes:
    marker = b',"live":'
    require(raw_snapshot.endswith(b"}\n"), "snapshot line is not LF-terminated")
    marker_offset = raw_snapshot.find(marker)
    require(marker_offset >= 0, "snapshot line has no raw live field")
    raw_live = raw_snapshot[marker_offset + len(marker) : -2]
    return b'{"tick":' + str(tick).encode("ascii") + marker + raw_live + b"}\n"


def rendered_bytes(reader: ModuleType, tick: int, heap: Any) -> bytes:
    rendered = reader.render(tick, heap)
    require(isinstance(rendered, str), "reader.render did not return str")
    return rendered.encode("utf-8") + b"\n"


def pin_synthetic_edges(reader_path: Path, reader: ModuleType) -> None:
    forwarding_event = {
        "seq": 0,
        "tick": 0,
        "kind": "relocate",
        "id": 10,
        "to_id": 20,
    }
    live_20 = {
        "id": 20,
        "kind": "record",
        "size": 1,
        "gen": 0,
        "refs": [],
    }
    heap = reader.replay(
        [
            forwarding_event,
            {"seq": 1, "tick": 1, "kind": "update", "id": 10, "size": 2},
        ],
        {"tick": 0, "seq": 1, "live": [live_20]},
        1,
        2,
    )
    require(
        20 in heap and heap[20]["size"] == 1,
        "stale update ID followed a pre-seek forwarding alias",
    )

    live_10 = {**live_20, "id": 10}
    heap = reader.replay(
        [forwarding_event],
        {"tick": 0, "seq": 1, "live": [live_10]},
        0,
        1,
    )
    require(
        10 in heap and heap[10]["id"] == 10 and 20 not in heap,
        "snapshot object ID followed a pre-seek forwarding alias",
    )

    try:
        reader._resolved(1, {1: 2, 2: 1})
    except ValueError:
        pass
    else:
        raise RuntimeError("forwarding cycle did not raise ValueError")

    first = {**live_20, "id": 30}
    last = {**live_20, "id": 40}
    original_load_bundle = reader.load_bundle
    reader.load_bundle = lambda _directory: (
        [],
        [
            {"tick": 7, "seq": 0, "live": [first]},
            {"tick": 7, "seq": 0, "live": [last]},
        ],
    )
    try:
        heap = reader.materialize(Path("unused"), 7)
    finally:
        reader.load_bundle = original_load_bundle
    require(
        40 in heap and heap[40]["id"] == 40 and 30 not in heap,
        "materialize did not choose the last equal tick/seq snapshot",
    )

    completed = subprocess.run(
        [str(reader_path)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=5,
    )
    require(completed.returncode != 0, "reader CLI without arguments succeeded")
    require(completed.stdout == b"", "reader CLI usage wrote stdout")
    require(
        completed.stderr == b"usage: reader.py <trace-dir> <tick>\n",
        "reader CLI usage stderr drifted",
    )


def pin_bundle(
    reader_path: Path, reader: ModuleType, directory: Path
) -> tuple[int, int, int]:
    events, snapshots = reader.load_bundle(directory)
    require(isinstance(events, list), f"{directory.name}: events is not a list")
    require(isinstance(snapshots, list), f"{directory.name}: snapshots is not a list")
    raw_snapshots = (directory / "snapshots.jsonl").read_bytes().splitlines(
        keepends=True
    )
    require(
        len(raw_snapshots) == len(snapshots),
        f"{directory.name}: parsed/raw snapshot counts differ",
    )
    require(bool(snapshots), f"{directory.name}: no snapshots")
    initial = {"tick": 0, "seq": 0, "live": []}
    require(snapshots[0] == initial, f"{directory.name}: initial snapshot is not empty")

    replay_count = 0
    event_span = 0
    for index, (target, raw_snapshot) in enumerate(zip(snapshots, raw_snapshots)):
        tick = target["tick"]
        target_seq = target["seq"]
        seek = initial if index == 0 else snapshots[index - 1]
        heap = reader.replay(events, seek, tick, target_seq)
        expected = expected_output(raw_snapshot, tick)
        require(
            rendered_bytes(reader, tick, heap) == expected,
            f"{directory.name}: replay mismatch at snapshot {index}",
        )
        materialized = reader.materialize(directory, tick)
        require(
            rendered_bytes(reader, tick, materialized) == expected,
            f"{directory.name}: exact-tick materialize mismatch at snapshot {index}",
        )
        completed = subprocess.run(
            [str(reader_path), str(directory), str(tick)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=5,
        )
        require(
            completed.returncode == 0,
            f"{directory.name}: reader CLI exited {completed.returncode} "
            f"at snapshot {index}",
        )
        require(
            completed.stderr == b"",
            f"{directory.name}: reader CLI wrote stderr at snapshot {index}",
        )
        require(
            completed.stdout == expected,
            f"{directory.name}: reader CLI mismatch at snapshot {index}",
        )
        if index:
            replay_count += 1
            event_span += target_seq - seek["seq"]

    require(
        snapshots[-1]["seq"] == len(events),
        f"{directory.name}: replay segments do not cover every event",
    )
    require(
        event_span == len(events),
        f"{directory.name}: covered event span differs from event count",
    )
    return len(snapshots), replay_count, event_span


def main(argv: list[str]) -> int:
    require(len(argv) == 3, "usage: reader_pin.py READER TRACE_ROOT")
    reader_path = Path(argv[1])
    require(
        reader_path.is_file() and not reader_path.is_symlink(),
        f"reader is not a regular file: {reader_path}",
    )
    require(os.access(reader_path, os.X_OK), f"reader is not executable: {reader_path}")
    reader = import_reader(reader_path)
    pin_synthetic_edges(reader_path, reader)
    trace_root = Path(argv[2])
    require(trace_root.is_dir(), f"trace root is not a directory: {trace_root}")
    directories = sorted(path for path in trace_root.iterdir() if path.is_dir())
    require(bool(directories), f"trace root has no bundle directories: {trace_root}")

    snapshot_count = 0
    replay_count = 0
    event_span = 0
    for directory in directories:
        snapshots, replays, events = pin_bundle(reader_path, reader, directory)
        snapshot_count += snapshots
        replay_count += replays
        event_span += events
    print(
        f"READER PIN OK bundles={len(directories)} snapshots={snapshot_count} "
        f"replays={replay_count} events={event_span}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except Exception as error:
        print(f"READER PIN FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
