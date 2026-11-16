#!/usr/bin/env python3
"""Run a capture, then fail unless the passes its report timed are exactly the
ones expected.

With every rendering feature off and nothing in the scene, the frame should
record only the passes it cannot do without. A feature whose pass still runs
when it is switched off costs every player who turned it off, and nothing else
shows it: the picture looks the same.

Usage: check-passes.py --report <json> [--expect <pass,pass,...>] [--expect-file <path>]... -- <capture command...>

--expect-file adds the comma-separated names a file holds, such as the base
passes the build generates from a graph's pass declarations; give one per graph.

The capture command must write its report, with per-pass timing on, to the
path given as --report.

Exit codes: 0 the passes matched, 1 they did not or the capture failed.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--report", required=True, type=Path, help="where the capture writes its JSON report")
    parser.add_argument("--expect", default="", help="the passes the frame should record, comma separated")
    parser.add_argument("--expect-file", type=Path, action="append", default=[],
                        help="a file holding more passes the frame should record, comma separated; repeatable")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="the capture to run, after --")
    arguments = parser.parse_args(argv)
    if arguments.command and arguments.command[0] == "--":
        arguments.command = arguments.command[1:]
    if not arguments.command:
        parser.error("give the capture command after --")
    if not arguments.expect and not arguments.expect_file:
        parser.error("give the expected passes with --expect or --expect-file")
    return arguments


def recorded_passes(report: Path):
    """The pass names in the capture report at @p report."""
    with report.open() as handle:
        return set(json.load(handle).get("passes", {}))


def main(argv) -> int:
    arguments = parse_arguments(argv)
    arguments.report.unlink(missing_ok=True)
    if subprocess.call(arguments.command) != 0:
        print("check-passes: the capture failed.", file=sys.stderr)
        return 1
    listed = arguments.expect
    for expect_file in arguments.expect_file:
        listed += "," + expect_file.read_text().strip()
    expected = {name for name in listed.split(",") if name}
    recorded = recorded_passes(arguments.report)
    if recorded == expected:
        print(f"check-passes: the frame recorded exactly {', '.join(sorted(expected))}.")
        return 0
    extra = sorted(recorded - expected)
    missing = sorted(expected - recorded)
    if extra:
        print(f"check-passes: passes ran that nothing switched on: {', '.join(extra)}.", file=sys.stderr)
    if missing:
        print(f"check-passes: passes the frame cannot do without did not run: {', '.join(missing)}.",
              file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
