#!/usr/bin/env python3
"""Negative and helper-level regression tests for build_showcase.py.

The watchability cases consume only real checked-in lang_trace output. Scratch files in
this self-test are ordinary marker files for path/publication tests, never trace files.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
from typing import Callable


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


def verify_event_policy_is_explicit_and_manifested() -> None:
    expected_modes = {
        "tree_churn": "sampled",
        "intern_pressure": "sampled",
        "ephemeron_lifecycle": "full",
    }
    require(
        {demo.id: demo.verify_events for demo in showcase.DEMOS} == expected_modes,
        "showcase verify-event policy drifted",
    )

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "SCHEMA.md").write_bytes(b"schema marker\n")
        for demo in showcase.DEMOS:
            trace_directory = root / "traces" / demo.id
            trace_directory.mkdir(parents=True)
            for name in showcase.BUNDLE_FILES:
                (trace_directory / name).write_bytes(f"{demo.id}:{name}\n".encode())

            command = showcase._lang_trace_command(
                Path("/lang_trace"),
                trace_directory / "program.lang",
                trace_directory,
                demo,
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

        showcase._write_manifest(root)
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
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
