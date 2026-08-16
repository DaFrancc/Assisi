#!/usr/bin/env python3
"""Read a Chiara capture and explain one frame, or the whole run.

Chiara writes Chrome Trace Event JSON, which
Perfetto renders beautifully and answers nothing specific about. This does the
other half: pick a frame, print its scope tree with a duration against every
slice, and show what the counters said at that instant.

    scripts/chiara-analyze.py capture.json --at 15          # frame nearest t=15 s
    scripts/chiara-analyze.py capture.json --frame 2153     # by index
    scripts/chiara-analyze.py capture.json --worst          # the slowest frame
    scripts/chiara-analyze.py capture.json --percentile 99  # a representative p99
    scripts/chiara-analyze.py capture.json --summary        # no frame; aggregates only
    scripts/chiara-analyze.py capture.json --at 15 --hot    # + self-time ranking

`--summary` is also printed as a header for every frame view, because a frame is
only interesting relative to the distribution it came from: 7 ms means one thing
at p50 and another at p99.

Note on the trailing frame: a capture stopped mid-frame emits its last, still-open
`Frame` scope with a garbage timestamp and a negative duration. Every mode here
drops negative-duration events, so a naive mean is not poisoned by it — but the
count of what was dropped is reported rather than hidden.
"""

import argparse
import json
import statistics
import sys
from collections import Counter, defaultdict

# The scope that delimits a frame. Chiara opens exactly one per iteration of the
# application loop, on the main thread.
FRAME_SCOPE = "Frame"


def load(path):
    """Returns (events, thread-names, complete-events, frames)."""
    with open(path) as handle:
        events = json.load(handle)["traceEvents"]

    thread_names = {
        e["tid"]: e["args"]["name"]
        for e in events
        if e["ph"] == "M" and e["name"] == "thread_name"
    }

    # 'X' is a complete (begin+duration) event — every ASSISI_PROFILE_SCOPE.
    complete = [e for e in events if e["ph"] == "X"]
    dropped = [e for e in complete if e.get("dur", 0) < 0]
    complete = [e for e in complete if e.get("dur", 0) >= 0]

    frames = sorted((e for e in complete if e["name"] == FRAME_SCOPE), key=lambda e: e["ts"])
    return events, thread_names, complete, frames, dropped


def summarize(frames, complete, thread_names, dropped):
    if not frames:
        print("no complete Frame scopes in this capture")
        return

    durations = sorted(f["dur"] for f in frames)

    def pct(p):
        return durations[min(int(p / 100 * len(durations)), len(durations) - 1)] / 1000

    span = (frames[-1]["ts"] + frames[-1]["dur"] - frames[0]["ts"]) / 1e6
    print(f"{len(frames)} frames over {span:.1f} s  ({len(frames) / span:.1f} fps average)")
    print(
        f"  frame ms   p50 {pct(50):.3f}   p90 {pct(90):.3f}   p99 {pct(99):.3f}   "
        f"max {durations[-1] / 1000:.3f}   mean {statistics.mean(durations) / 1000:.3f}"
    )

    per_thread = Counter(thread_names.get(e["tid"], f"tid-{e['tid']}") for e in complete)
    active = {t: c for t, c in per_thread.items() if c}
    print(f"  {len(complete)} scopes across {len(active)} active thread(s): "
          + ", ".join(f"{t} ({c})" for t, c in sorted(active.items(), key=lambda kv: -kv[1])))

    if dropped:
        print(f"  dropped {len(dropped)} negative-duration event(s) "
              "(capture stopped mid-scope): "
              + ", ".join(sorted({e['name'] for e in dropped})))
    print()


def pick_frame(frames, args):
    """Returns (index, frame) for whichever selector was given."""
    if args.frame is not None:
        if not 0 <= args.frame < len(frames):
            sys.exit(f"--frame {args.frame} out of range (0..{len(frames) - 1})")
        return args.frame, frames[args.frame]

    if args.worst:
        best = max(range(len(frames)), key=lambda i: frames[i]["dur"])
        return best, frames[best]

    if args.percentile is not None:
        # The frame whose duration sits at that percentile — a representative
        # slow frame rather than the single worst outlier.
        order = sorted(range(len(frames)), key=lambda i: frames[i]["dur"])
        return (lambda i: (i, frames[i]))(
            order[min(int(args.percentile / 100 * len(order)), len(order) - 1)]
        )

    # Default/--at: the frame whose start is nearest the requested wall time,
    # measured from the first frame in the capture rather than from ts=0 (the
    # clock has an arbitrary origin).
    target = frames[0]["ts"] + (args.at or 0) * 1e6
    best = min(range(len(frames)), key=lambda i: abs(frames[i]["ts"] - target))
    return best, frames[best]


def nest(events):
    """Assigns a depth to each event by containment. Input need not be sorted.

    Sorting by (start, -duration) puts a parent immediately before its children,
    so a stack of end-times is enough — no interval tree, and it matches how the
    scopes were actually nested at runtime.
    """
    ordered = sorted(events, key=lambda e: (e["ts"], -e["dur"]))
    stack, out = [], []
    for event in ordered:
        while stack and event["ts"] >= stack[-1]:
            stack.pop()
        out.append((len(stack), event))
        stack.append(event["ts"] + event["dur"])
    return out


def print_tree(depth_events, frame, thread_label):
    total = frame["dur"]
    print(f"== {thread_label} ==")
    for depth, event in depth_events:
        share = 100 * event["dur"] / total if total else 0
        print(
            f"  {'  ' * depth}{event['name']:<{30 - 2 * depth}} "
            f"{event['dur'] / 1000:8.3f} ms  {share:5.1f}%  @+{(event['ts'] - frame['ts']) / 1000:7.3f} ms"
        )
    print()


def print_hot(depth_events):
    """Self time = a scope's duration minus its direct children's.

    This is what says *which* slice actually holds the cost, as opposed to which
    slice contains it — `render` being 1.5 ms tells you nothing on its own.
    """
    children = defaultdict(float)
    stack = []  # (depth, event)
    for depth, event in depth_events:
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if stack:
            children[id(stack[-1][1])] += event["dur"]
        stack.append((depth, event))

    rows = sorted(
        ((e["dur"] - children[id(e)], e) for _, e in depth_events),
        key=lambda row: -row[0],
    )
    print("== self time (excludes nested scopes) ==")
    for self_us, event in rows:
        if self_us / 1000 < 0.001:
            continue
        print(f"  {event['name']:<30} {self_us / 1000:8.3f} ms")
    print()


def print_counters(events, frame, only_stale):
    """Counters are point samples; report the newest at or before the frame end.

    A counter last written long before this frame is not "flat", it is stale —
    a series sampled on an event rather than per frame. Flagging that is the
    difference between reading a real plateau and reading a leftover.
    """
    end = frame["ts"] + frame["dur"]
    latest = {}
    for event in sorted((e for e in events if e["ph"] == "C"), key=lambda e: e["ts"]):
        if event["ts"] <= end:
            latest[event["name"]] = event

    if not latest:
        return
    print("== counters (newest sample at or before frame end) ==")
    for name in sorted(latest):
        event = latest[name]
        age_ms = (end - event["ts"]) / 1000
        stale = age_ms > 100  # older than ~a dozen frames: not a per-frame series
        if only_stale and not stale:
            continue
        value = event["args"].get("v", event["args"])
        marker = f"   [stale: {age_ms / 1000:.1f} s old]" if stale else ""
        shown = f"{value:.6g}" if isinstance(value, (int, float)) else str(value)
        print(f"  {name:<32} {shown}{marker}")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Analyze one frame of a Chiara capture.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        # Docstrings are stripped under `python -OO`, so this must not assume one.
        epilog=(__doc__ or "").split("\n", 2)[-1],
    )
    parser.add_argument("capture", help="path to a chiara-*.json capture")
    selector = parser.add_mutually_exclusive_group()
    selector.add_argument("--at", type=float, metavar="SEC",
                          help="frame nearest this many seconds into the capture (default 0)")
    selector.add_argument("--frame", type=int, metavar="N", help="frame by index")
    selector.add_argument("--worst", action="store_true", help="the single slowest frame")
    selector.add_argument("--percentile", type=float, metavar="P",
                          help="a frame at this duration percentile, e.g. 99")
    parser.add_argument("--summary", action="store_true",
                        help="capture-wide statistics only; do not dump a frame")
    parser.add_argument("--hot", action="store_true",
                        help="also rank scopes by self time")
    parser.add_argument("--all-threads", action="store_true",
                        help="include scopes from threads other than the frame's")
    parser.add_argument("--stale-counters", action="store_true",
                        help="show only counters that are not being sampled per frame")
    args = parser.parse_args()

    events, thread_names, complete, frames, dropped = load(args.capture)
    summarize(frames, complete, thread_names, dropped)
    if args.summary:
        return

    index, frame = pick_frame(frames, args)
    start_s = (frame["ts"] - frames[0]["ts"]) / 1e6
    faster = sum(1 for f in frames if f["dur"] < frame["dur"])
    print(
        f"FRAME #{index}  t=+{start_s:.3f} s  dur={frame['dur'] / 1000:.3f} ms  "
        f"(p{100 * faster / len(frames):.0f} of this capture)\n"
    )

    own_thread = [e for e in complete
                  if e["tid"] == frame["tid"] and frame["ts"] <= e["ts"] < frame["ts"] + frame["dur"]]
    depth_events = nest(own_thread)
    print_tree(depth_events, frame, thread_names.get(frame["tid"], f"tid-{frame['tid']}"))

    if args.hot:
        print_hot(depth_events)

    if args.all_threads:
        # Any thread that had a scope open during this frame's window — worker
        # and physics activity that the main-thread tree cannot show.
        by_thread = defaultdict(list)
        for event in complete:
            if event["tid"] != frame["tid"] and event["ts"] < frame["ts"] + frame["dur"] \
                    and event["ts"] + event["dur"] > frame["ts"]:
                by_thread[event["tid"]].append(event)
        for tid, tid_events in sorted(by_thread.items()):
            print_tree(nest(tid_events), frame, thread_names.get(tid, f"tid-{tid}"))
        if not by_thread:
            print("== no other thread had a scope open during this frame ==\n")

    print_counters(events, frame, args.stale_counters)


if __name__ == "__main__":
    main()
