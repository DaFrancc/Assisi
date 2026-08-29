#!/usr/bin/env python3
"""Show one frame of a Chiara capture against the distribution it came from.

A duration on its own says nothing. 0.079 ms in `submit-present` is either the
steady state or a spike, and which one it is decides whether there is anything
to do. So every row here carries the same scope's median, min and max over the
whole recording, the signed distance from that median, and where this frame's
value falls between the extremes.

    scripts/chiara-frame.py 15                 # frame nearest t=15 s
    scripts/chiara-frame.py 15 median          # median frame in t=14..16 s
    scripts/chiara-frame.py 15 slowest         # slowest frame in that window
    scripts/chiara-frame.py 15 busiest --window all   # most CPU work, whole run
    scripts/chiara-frame.py 15 median --hot    # + self-time ranking

With no --capture, it asks which build and which capture to read.

Scopes are keyed by their path through the tree, not by name, so the two
`pp-resolve` steps in a post-process chain keep separate statistics. A path that
is not in every frame reports how many frames it appeared in.
"""

import argparse
import bisect
import json
import os
import statistics
import sys
import textwrap
from collections import defaultdict

# The scope that delimits a frame. Chiara opens exactly one per iteration of the
# application loop, on the main thread.
FRAME_SCOPE = "Frame"

# Subtracted from a frame's duration to get the work it actually did. A capped
# frame is mostly this, so ranking frames by duration ranks them by how long
# they waited — the opposite of what a profile is for.
SLEEP_SCOPE = "pacing-sleep"

# Windowed selectors span this many seconds either side of the timestamp.
DEFAULT_WINDOW_S = 1.0

# Rows whose value and median are both under this are dropped: at a tenth of a
# microsecond the number is the timer, not the code.
NOISE_MS = 0.0005


# --- terminal ---------------------------------------------------------------

class Style:
    """ANSI escapes, or empty strings when the output is not a terminal."""

    def __init__(self, enabled):
        self.enabled = enabled

    def __call__(self, text, *codes):
        if not self.enabled or not codes:
            return text
        return f"\033[{';'.join(codes)}m{text}\033[0m"

    DIM = "2"
    BOLD = "1"
    RED = "31"
    GREEN = "32"
    YELLOW = "33"
    BLUE = "34"
    CYAN = "36"


def percentile(ordered, p):
    return ordered[min(int(p / 100 * len(ordered)), len(ordered) - 1)]


def progress(message, style):
    """Says what is happening during the second or so a capture takes to parse.

    On stderr, so redirecting the report to a file still shows it and still gets
    a clean file. Rewritten in place on a terminal and left behind otherwise.
    """
    if not sys.stderr.isatty():
        return lambda: None
    sys.stderr.write(style(message, Style.DIM))
    sys.stderr.flush()
    return lambda: (sys.stderr.write("\r" + " " * len(message) + "\r"), sys.stderr.flush())


# --- capture discovery ------------------------------------------------------

def find_captures(root):
    """Returns {build name: [capture paths, newest first]} under out/build."""
    builds = {}
    build_root = os.path.join(root, "out", "build")
    if not os.path.isdir(build_root):
        return builds
    for build in sorted(os.listdir(build_root)):
        found = []
        for base, _, files in os.walk(os.path.join(build_root, build)):
            if os.path.basename(base) != "captures":
                continue
            found += [os.path.join(base, f) for f in files
                      if f.startswith("chiara-") and f.endswith(".json")]
        if found:
            builds[build] = sorted(found, key=os.path.getmtime, reverse=True)
    return builds


def choose(prompt, options, describe, style):
    """One-of menu. Returns the chosen option; Enter takes the first."""
    if len(options) == 1:
        return options[0]
    if not sys.stdin.isatty():
        print(style(f"{prompt}: {len(options)} candidates, taking the newest "
                    f"({describe(options[0])}). Pass --capture to choose.", Style.DIM))
        return options[0]

    print(style(prompt, Style.BOLD))
    for i, option in enumerate(options, 1):
        print(f"  {i}) {describe(option)}")
    while True:
        reply = input(f"Choose [1-{len(options)}, default 1]: ").strip()
        if not reply:
            return options[0]
        if reply.isdigit() and 1 <= int(reply) <= len(options):
            return options[int(reply) - 1]
        print(style("  not one of those", Style.RED))


def resolve_capture(args, root, style):
    if args.capture:
        return args.capture

    builds = find_captures(root)
    if not builds:
        sys.exit(f"no captures found under {os.path.join(root, 'out', 'build')} "
                 "— run a -c build and take one, or pass --capture")

    if args.build:
        if args.build not in builds:
            sys.exit(f"build '{args.build}' has no captures; found: {', '.join(builds)}")
        name = args.build
    else:
        names = sorted(builds, key=lambda b: os.path.getmtime(builds[b][0]), reverse=True)
        name = choose(
            "Which build?", names,
            lambda b: f"{b:<24} {len(builds[b])} capture(s)", style)

    def describe(path):
        size_mb = os.path.getsize(path) / (1 << 20)
        stamp = os.path.getmtime(path)
        from datetime import datetime
        return (f"{os.path.basename(path):<32} {size_mb:6.1f} MB   "
                f"{datetime.fromtimestamp(stamp):%Y-%m-%d %H:%M}")

    return choose(f"Which capture in {name}?", builds[name], describe, style)


# --- loading ----------------------------------------------------------------

def load(path):
    """One pass over the event array; the file is tens of MB and every extra
    sweep of it costs more than anything downstream."""
    with open(path, "rb") as handle:
        events = json.loads(handle.read())["traceEvents"]

    main_tid = None
    scopes, counters, dropped = [], [], 0
    for event in events:
        phase = event["ph"]
        if phase == "X":
            # 'X' is a complete (begin+duration) event — every
            # ASSISI_PROFILE_SCOPE. A capture stopped mid-frame leaves its
            # outermost scopes open and writes them with a negative duration;
            # those are not data.
            if event.get("dur", 0) < 0:
                dropped += 1
                continue
            scopes.append(event)
            if main_tid is None and event["name"] == FRAME_SCOPE:
                main_tid = event["tid"]
        elif phase == "C":
            counters.append(event)

    if main_tid is None:
        sys.exit(f"{path}: no complete '{FRAME_SCOPE}' scopes — not a frame capture")

    # Sorting by (start, -duration) puts a parent immediately before its
    # children, so a stack of end times reconstructs the nesting that produced
    # them — no interval tree, and it matches how they actually nested.
    ordered = sorted((e for e in scopes if e["tid"] == main_tid),
                     key=lambda e: (e["ts"], -e["dur"]))
    frames = [e for e in ordered if e["name"] == FRAME_SCOPE]
    return ordered, counters, frames, dropped


def cpu_work_per_frame(ordered, frames):
    """Frame duration minus its pacing sleep: what each frame actually did.

    Ranking pacing-capped frames by duration ranks them by how long they waited,
    which is the reverse of interesting. Both scopes are one per frame, so this
    merges two short lists rather than walking the whole capture.
    """
    sleeps = [e for e in ordered if e["name"] == SLEEP_SCOPE]
    work, at = [], 0
    for frame in frames:
        end = frame["ts"] + frame["dur"]
        while at < len(sleeps) and sleeps[at]["ts"] < frame["ts"]:
            at += 1
        slept = 0
        while at < len(sleeps) and sleeps[at]["ts"] < end:
            slept += sleeps[at]["dur"]
            at += 1
        work.append(frame["dur"] - slept)
    return work


def analyze(ordered, want):
    """Walks every frame once: per-path statistics, and the rows of one frame.

    Scopes are identified by their position in the tree rather than by name, so
    a chain that resolves twice keeps the two steps apart. Paths are interned to
    integers — building a string key per event is the single largest cost in
    this program, and nothing downstream needs the string.
    """
    path_ids = {}      # (parent id, name, ordinal) -> path id
    path_name = []     # path id -> scope name
    samples = defaultdict(list)

    index, rows, selected = -1, None, None
    per_frame = None
    sibling = {}       # (parent id, name) -> count, within the current frame
    stack_end, stack_id = [], []

    for event in ordered:
        ts = event["ts"]
        name = event["name"]
        while stack_end and ts >= stack_end[-1]:
            stack_end.pop()
            stack_id.pop()

        if not stack_end and name == FRAME_SCOPE:
            if per_frame is not None:
                for pid, total in per_frame.items():
                    samples[pid].append(total)
            index += 1
            per_frame = {}
            sibling.clear()
            rows = [] if index == want else None
            if rows is not None:
                selected = rows
        if per_frame is None:
            continue  # a capture can open mid-frame; that fragment is not one

        parent = stack_id[-1] if stack_id else -1
        key = (parent, name)
        ordinal = sibling.get(key, 0)
        sibling[key] = ordinal + 1

        pid = path_ids.get((parent, name, ordinal))
        if pid is None:
            pid = len(path_name)
            path_ids[(parent, name, ordinal)] = pid
            path_name.append(name)

        # The ordinal makes a path unique within a frame, so this is an
        # assignment rather than an accumulation.
        per_frame[pid] = event["dur"]
        if rows is not None:
            rows.append((len(stack_end), pid, event))
        stack_end.append(ts + event["dur"])
        stack_id.append(pid)

    if per_frame:
        for pid, total in per_frame.items():
            samples[pid].append(total)

    stats = {}
    for pid, values in samples.items():
        values.sort()
        middle = len(values) // 2
        median = values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2
        stats[pid] = (median, values[0], values[-1], values)
    return stats, selected or []


# --- frame selection --------------------------------------------------------

def window_bounds(frames, at_s, half_s):
    """[lo, hi] in capture-relative seconds, shifted to stay inside the capture.

    A window centred at 0.5 s with a 1 s half-width would read a second of
    nothing; sliding it to 0..2 s answers the question that was asked with data
    that exists, which is better than answering it with half as much.
    """
    origin = frames[0]["ts"]
    span = (frames[-1]["ts"] - origin) / 1e6
    if half_s is None:
        return 0.0, span

    width = min(2 * half_s, span)
    lo = at_s - width / 2
    lo = max(0.0, min(lo, span - width))
    return lo, lo + width


def select(frames, work, at_s, target, half_s):
    """Returns (index, note) for the chosen frame."""
    origin = frames[0]["ts"]

    if target == "nearest":
        best = min(range(len(frames)), key=lambda i: abs((frames[i]["ts"] - origin) / 1e6 - at_s))
        return best, f"nearest to t={at_s:g} s"

    lo, hi = window_bounds(frames, at_s, half_s)
    pool = [i for i in range(len(frames)) if lo <= (frames[i]["ts"] - origin) / 1e6 <= hi]
    if not pool:
        sys.exit(f"no frames in t={lo:.3f}..{hi:.3f} s")
    scope = "whole capture" if half_s is None else f"t={lo:.3f}..{hi:.3f} s"
    note = f"{target} of {len(pool)} frames in {scope}"

    if target == "busiest":
        return max(pool, key=lambda i: work[i]), note
    if target == "idlest":
        return min(pool, key=lambda i: work[i]), note

    order = sorted(pool, key=lambda i: frames[i]["dur"])
    picks = {
        "fastest": 0.0,
        "median": 50.0,
        "p90": 90.0,
        "p99": 99.0,
        "slowest": 100.0,
    }
    rank = picks[target] / 100 * (len(order) - 1)
    return order[round(rank)], note


# --- rendering --------------------------------------------------------------

def ratio_text(delta, median, floor):
    """The delta as a percentage of the median, in a fixed six columns.

    A scope that idles in most frames has a median at or near zero, against
    which any real value is some enormous multiple. That number is arithmetic
    rather than information, so below `floor` the ratio is withheld and the
    absolute delta beside it carries the row.
    """
    if abs(median) < floor:
        return "     —"
    pct = delta / median * 100
    if pct > 999:
        return " >999%"
    if pct < -999:
        return "<-999%"
    return f"{pct:+5.0f}%"


def delta_cell(value, median, style, width=17):
    """Signed distance from the median, coloured by direction and size."""
    delta = value - median
    # One microsecond: below this a scope's median is the timer's resolution.
    text = f"{delta / 1000:+7.3f} {ratio_text(delta, median, 1.0)}"
    pct = (delta / median * 100) if median else 0.0
    if abs(median) < 1.0:
        colour = (Style.DIM,)
    elif abs(pct) < 10:
        colour = (Style.DIM,)
    elif delta > 0:
        colour = (Style.RED,) if pct > 50 else (Style.YELLOW,)
    else:
        colour = (Style.GREEN,)
    return style(f"{text:>{width}}", *colour)


def rule(label, style, width=112):
    """A titled horizontal rule. The eye needs somewhere to stop between
    sections when the screen is otherwise a field of numbers."""
    head = f"── {label} " if label else ""
    return style(head + "─" * max(0, width - len(head)), Style.DIM)


def share_bar(fraction, width=8):
    """Filled proportion of a scope's frame. A number says how much; a bar says
    how much compared to the row above it, without being read."""
    filled = int(round(fraction * width))
    if filled == 0:
        return ("▏" if fraction > 0 else " ") + " " * (width - 1)
    return "█" * filled + " " * (width - filled)


def plain_english(percentile_rank, what):
    """The percentile as a sentence. 'p98' is precise and means nothing at a
    glance; 'slower than 98% of frames' means something and costs a line."""
    if percentile_rank >= 50:
        return f"{what} than {percentile_rank:.0f}% of frames"
    return f"{what} than only {percentile_rank:.0f}% of frames"


def print_header(path, frames, work, index, note, dropped, style):
    origin = frames[0]["ts"]
    span = (frames[-1]["ts"] + frames[-1]["dur"] - origin) / 1e6
    durations = sorted(f["dur"] for f in frames)
    ranked_work = sorted(work)

    def row(label, values, note_text=""):
        cells = "".join(f"{percentile(values, p) / 1000:9.3f}" for p in (50, 90, 99))
        line = f"  {label:<10}{cells}{values[0] / 1000:9.3f}{values[-1] / 1000:9.3f}"
        return line + style(f"   {note_text}", Style.DIM) if note_text else line

    print(rule(style(os.path.basename(path), Style.BOLD, Style.CYAN), style))
    print(style(f"  {len(frames)} frames · {span:.1f} s · {len(frames) / span:.1f} fps",
                Style.DIM))
    print()
    print(style(f"  {'ms':<10}{'p50':>9}{'p90':>9}{'p99':>9}{'min':>9}{'max':>9}", Style.BOLD))
    print(row("frame", durations))
    print(row("cpu work", ranked_work, "frame minus pacing-sleep"))
    if dropped:
        print(style(f"  {dropped} negative-duration event(s) dropped "
                    "(capture stopped mid-scope)", Style.DIM))
    print()

    frame = frames[index]
    at = (frame["ts"] - origin) / 1e6
    faster = 100 * bisect.bisect_left(durations, frame["dur"]) / len(durations)
    busier = 100 * bisect.bisect_left(ranked_work, work[index]) / len(ranked_work)

    print(rule(style(f"FRAME #{index}", Style.BOLD) + style(f"  at t = +{at:.3f} s", Style.DIM),
               style))
    print(f"  duration   {style(f'{frame['dur'] / 1000:7.3f} ms', Style.BOLD)}   "
          + style(plain_english(faster, "slower"), Style.DIM))
    print(f"  cpu work   {style(f'{work[index] / 1000:7.3f} ms', Style.BOLD)}   "
          + style(plain_english(busier, "busier"), Style.DIM))
    print(style(f"  chosen as  {note}", Style.DIM))
    print()


def print_standout(rows, stats, style, limit=8):
    """The few scopes this frame did something unusual in.

    The tree below has every number in it, which is the problem: a hundred rows
    of ordinary values hide the three that moved. This is the same data filtered
    to what a reader would have gone looking for.
    """
    picks = []
    for depth, pid, event in rows:
        if depth == 0:
            continue  # the frame itself; the card above already reports it
        median, _, _, sorted_v = stats[pid]
        delta = event["dur"] - median
        # 10 µs: under it a scope can be at p100 and still not matter.
        if abs(delta) < 10:
            continue
        rank = 100 * bisect.bisect_left(sorted_v, event["dur"]) / len(sorted_v)
        if 5 < rank < 95:
            continue
        picks.append([abs(delta), delta, rank, event, median, depth])

    # A parent whose whole excursion is one child's says nothing the child does
    # not: listing `fixed-update` above `physics-step` costs a line and adds
    # nothing. The child is kept because it names the code.
    explained = set()
    for i, (_, delta, _, _, _, depth) in enumerate(picks):
        for _, child_delta, _, _, _, child_depth in picks[i + 1:]:
            if child_depth <= depth:
                break
            if delta and abs(child_delta / delta) >= 0.8:
                explained.add(i)
                break
    picks = [p for i, p in enumerate(picks) if i not in explained]

    print(rule(style("WHAT STANDS OUT", Style.BOLD)
               + style("  (scopes furthest from their own median)", Style.DIM), style))
    if not picks:
        print(style("  nothing in this frame is unusual for it — every scope is "
                    "within its normal range", Style.DIM))
        print()
        return

    picks.sort(key=lambda p: -p[0])
    for _, delta, rank, event, median, _ in picks[:limit]:
        up = delta > 0
        arrow = style("▲", Style.RED) if up else style("▼", Style.GREEN)
        times = f"{event['dur'] / median:.1f}×" if median >= 1 else "  — "
        print(f"  {arrow} {event['name']:<28.28} "
              + style(f"{event['dur'] / 1000:8.3f} ms", Style.BOLD)
              + style(f"{delta / 1000:+8.3f}", Style.RED if up else Style.GREEN)
              + style(f" vs its median of {median / 1000:6.3f}   {times:>5}   "
                      f"p{rank:.0f}", Style.DIM))
    if len(picks) > limit:
        print(style(f"  … and {len(picks) - limit} more, in the tree below", Style.DIM))
    print()


def rank_cell(rank, style, width=6):
    """The percentile as a number and a fill. Reading a row is then one glance:
    a nearly-full bar is a scope having a worse frame than it usually does."""
    filled = int(round(rank / 100 * width))
    bar = "█" * filled + style("░" * (width - filled), Style.DIM)
    colour = (Style.RED,) if rank >= 95 else (Style.GREEN,) if rank <= 5 else (Style.DIM,)
    return style(f"p{rank:<3.0f}", *colour) + " " + bar


def print_tree(rows, stats, frame_count, style, max_depth, min_ms):
    # Shares are of the frame's work, not of the frame. A pacing-capped frame is
    # mostly sleep, against which every real scope rounds to nothing and a bar
    # of it is a blank column.
    sleep = sum(e["dur"] for d, _, e in rows if d == 1 and e["name"] == SLEEP_SCOPE)
    work = rows[0][2]["dur"] - sleep
    # Wide enough that the deepest scope in a render frame — a submit nested
    # five levels down — still reads as its own name rather than a prefix.
    name_w = 32

    print(rule(style("EVERY SCOPE IN THIS FRAME", Style.BOLD), style))
    print(style(f"    {'SCOPE':<{name_w}} {'ms':>8}  {'SHARE OF WORK':<14}"
                f"{'Δ vs MEDIAN':>17}   {'RANK':<11}  "
                f"{'MEDIAN':>8}{'p90':>8}", Style.BOLD))

    partial = {}
    for depth, pid, event in rows:
        if max_depth is not None and depth > max_depth:
            continue
        median, low, high, sorted_v = stats[pid]
        if event["dur"] / 1000 < min_ms and median / 1000 < min_ms:
            continue

        sleeping = depth <= 1 and event["name"] in (SLEEP_SCOPE, FRAME_SCOPE)
        share = 0.0 if sleeping else (event["dur"] / work if work else 0.0)
        rank = 100 * bisect.bisect_left(sorted_v, event["dur"]) / len(sorted_v)
        unusual = (rank >= 95 or rank <= 5) and abs(event["dur"] - median) >= 10

        mark = ("▲" if event["dur"] > median else "▼") if unusual else " "
        mark = style(mark, Style.RED if event["dur"] > median else Style.GREEN) if unusual else " "

        # A scope that sits some frames out is marked where it is read rather
        # than given a column that says `·` on nine rows in ten.
        if len(sorted_v) < frame_count:
            partial[event["name"]] = len(sorted_v)
        label = "  " * depth + event["name"] + ("*" if len(sorted_v) < frame_count else "")
        name = style(f"{label:<{name_w}.{name_w}}", Style.BOLD) if unusual \
            else f"{label:<{name_w}.{name_w}}"

        if sleeping:
            pct_text = style("    —", Style.DIM)
        elif share >= 0.005:
            pct_text = f"{100 * share:4.0f}%"
        else:
            pct_text = style("  <1%", Style.DIM)

        print(f"  {mark} {name} "
              f"{event['dur'] / 1000:8.3f}  "
              + style(share_bar(share), Style.CYAN) + pct_text + " "
              + delta_cell(event["dur"], median, style)
              + "   " + rank_cell(rank, style) + "  "
              + style(f"{median / 1000:8.3f}{percentile(sorted_v, 90) / 1000:8.3f}",
                      Style.DIM))

    print()
    print(style("  ms             this scope in this frame", Style.DIM))
    print(style(f"  SHARE OF WORK  its part of the frame's {work / 1000:.3f} ms of work "
                "(the frame minus pacing-sleep, which is why the two of those show —)",
                Style.DIM))
    print(style("  Δ vs MEDIAN    how far this frame is from what this scope usually "
                "costs; — when the median is under a microsecond", Style.DIM))
    print(style("  RANK           where this frame lands among that scope's own frames: "
                "p50 typical, p95+ slower than usual, p5- faster", Style.DIM))
    print(style("  MEDIAN  p90    what this scope normally costs, and its ordinary upper "
                "end — a p90 near the median is a steady", Style.DIM))
    print(style("                 scope, a p90 well above it one that varies frame to "
                "frame", Style.DIM))
    print(style("  ▲ ▼            unusual for this scope — the same rows listed above",
                Style.DIM))
    if partial:
        listed = ", ".join(f"{n} {c}" for n, c in sorted(partial.items()))
        body = (f"did not run in every frame, so its MEDIAN and p90 are over the frames "
                f"it did — {listed}, of {frame_count}")
        for i, line in enumerate(textwrap.wrap(body, 96)):
            label = f"  {'*':<15}" if i == 0 else " " * 17
            print(style(label + line, Style.DIM))
    print()


def print_hot(rows, style, limit):
    """Self time — a scope's duration minus its children's. What the tree cannot
    say on its own is which slice *holds* the cost rather than contains it."""
    children = defaultdict(float)
    stack = []
    for depth, pid, event in rows:
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if stack:
            children[stack[-1][1]] += event["dur"]
        stack.append((depth, pid, event))

    ranked = sorted(((e["dur"] - children[p], e) for _, p, e in rows), key=lambda r: -r[0])
    print(style("SELF TIME (excludes nested scopes)", Style.BOLD))
    total = rows[0][2]["dur"]
    for self_us, event in ranked[:limit]:
        if self_us / 1000 < NOISE_MS:
            continue
        bar = "█" * max(1, round(30 * self_us / ranked[0][0])) if ranked[0][0] else ""
        print(f"  {event['name']:<30.30} {self_us / 1000:8.3f} ms "
              f"{100 * self_us / total if total else 0:5.1f}%  "
              + style(bar, Style.CYAN))
    print()


def print_counters(counters, frames, index, style, min_frames):
    """Counters are point samples: the newest at or before this frame's end is
    what was true during it. The same median/min/max treatment applies, over
    every sample in the recording."""
    series = defaultdict(list)
    for event in sorted(counters, key=lambda e: e["ts"]):
        value = event["args"].get("v", event["args"])
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            series[event["name"]].append((event["ts"], float(value)))
    if not series:
        return

    end = frames[index]["ts"] + frames[index]["dur"]
    name_w = 34
    print(rule(style("COUNTERS AT THIS FRAME", Style.BOLD), style))
    print(style(f"{'COUNTER':<{name_w}} {'VALUE':>12} {'Δ vs MEDIAN':>17}   {'RANK':<11}  "
                f"{'MEDIAN':>12}{'p10':>12}{'p90':>12}", Style.BOLD))

    constant = []
    for name in sorted(series):
        stamps, values = zip(*series[name])
        if len(values) < min_frames:
            continue
        # A counter that never moved has nothing to compare against; the whole
        # table would otherwise be entity counts that were fixed at load.
        if values[0] == min(values) == max(values):
            constant.append((name, values[0]))
            continue
        at = bisect.bisect_right(stamps, end) - 1
        if at < 0:
            continue
        value = values[at]
        ordered = sorted(values)
        median = statistics.median(ordered)
        # p10/p90 rather than the extremes: a counter's min and max are one
        # startup frame apiece and read the same on every row.
        low, high = percentile(ordered, 10), percentile(ordered, 90)
        rank = 100 * bisect.bisect_left(ordered, value) / len(ordered)

        delta = value - median
        # Subtracting equal floats leaves a residue like -2.8e-17, which is not
        # a change and is wide enough to shift the column.
        if abs(delta) < abs(median) * 1e-9:
            delta = 0.0
        pct = (delta / median * 100) if median else 0.0
        # Counters carry their own units, so the only meaningful floor is zero.
        ratio = ratio_text(delta, median, sys.float_info.min)
        colour = (Style.DIM,) if abs(pct) < 10 else (
            (Style.YELLOW,) if delta > 0 else (Style.GREEN,))
        age_s = (end - stamps[at]) / 1e6
        stale = style("  [stale]", Style.RED) if age_s > 0.1 else ""

        print(f"{name:<{name_w}.{name_w}} {value:12.4g} "
              + style(f"{delta:+9.4g} {ratio}", *colour)
              + "   " + rank_cell(rank, style) + "  "
              + style(f"{median:12.4g}{low:12.4g}{high:12.4g}", Style.DIM)
              + stale)

    if constant:
        print()
        body = (f"{len(constant)} counter(s) never changed: "
                + ", ".join(f"{n}={v:g}" for n, v in constant))
        for line in textwrap.wrap(body, 110, subsequent_indent="  "):
            print(style("  " + line, Style.DIM))
    print()


# --- entry point ------------------------------------------------------------

TARGETS = ["nearest", "median", "fastest", "slowest", "p90", "p99", "busiest", "idlest"]

HELP = """
SELECTING A FRAME

  SECONDS is where to look, counted from the capture's first frame. TARGET says
  which frame to take from around there. Every target except `nearest` searches
  a window centred on SECONDS, whose half-width is --window (1 s by default);
  the window slides to stay inside the capture, so asking at 0.2 s reads
  0..2 s rather than a second of nothing.

    nearest   the frame whose start is closest to SECONDS. Ignores --window.
    median    the middle frame by duration. The one to reach for: a typical
              frame, not the outlier the eye is drawn to.
    fastest   shortest frame in the window.
    slowest   longest frame in the window. Where a hitch shows itself.
    p90       a frame at the 90th duration percentile — a bad frame that is
              still representative, rather than the single worst one.
    p99       the same at the 99th.
    busiest   most CPU work, where work is the frame's duration minus its
              pacing sleep. When frames are capped at a refresh rate, ranking
              by duration ranks them by how long they waited; this does not.
    idlest    least CPU work.

WHAT YOU GET

  Four sections, in the order you would want them:

    the capture      how long it ran and what an ordinary frame costs in it
    the frame        which frame this is, and how it ranks
    what stands out  the handful of scopes that did something unusual here.
                     Read this first — it is the whole point of the tool. When
                     a parent's excursion is entirely one child's, only the
                     child is listed, because only the child names the code.
    every scope      the full tree, for when the summary is not enough

READING THE TABLE

  Every row compares one scope in the chosen frame against that same scope
  across the whole recording:

    ms              its duration in this frame
    SHARE OF WORK   its part of the frame's work — the frame minus its pacing
                    sleep. Against the whole frame, a capped frame is 95%
                    sleep and every real scope rounds to nothing; those two
                    rows show `—` for that reason.
    Δ vs MEDIAN     distance from its own median, absolute and as a
                    percentage. `—` means that median is under a microsecond,
                    where the ratio would be arithmetic rather than
                    information.
    RANK            where this frame lands among that scope's own frames:
                    p50 is typical, p95+ slower than it usually is, p5- faster.
    MEDIAN          what this scope normally costs
    p90             its ordinary upper end. Together with MEDIAN this is the
                    scope's spread: a p90 near the median is a steady scope,
                    a p90 well above it one that varies frame to frame. The
                    true max is deliberately not shown — nearly every scope's
                    max is some startup hitch, so the column read "spiky" for
                    93% of rows and separated nothing.
    *               after a name: it did not run in every frame, so its
                    MEDIAN and p90 are over the frames it did run in. A scope
                    on fixed-update ticks — `physics-step` — is then compared
                    against its own real cost rather than diluted by the
                    frames it sat out. The legend names the counts.
    ▲ ▼             unusual for that scope — the rows listed in the summary

  Scopes are keyed by their position in the tree rather than by name, so the
  two `pp-resolve` steps of a post-process chain keep separate statistics.

CHOOSING A CAPTURE

  With no --capture, the script lists the builds under out/build that hold
  captures, then the captures in the one you pick. A single candidate is taken
  without asking, and so is the newest when stdin is not a terminal.

EXAMPLES

  chiara-frame.py 15                        the frame nearest t=15 s
  chiara-frame.py 15 median                 a typical frame from t=14..16 s
  chiara-frame.py 15 slowest --hot          the worst frame there, by self time
  chiara-frame.py 15 busiest --window all   the most expensive frame in the run
  chiara-frame.py 5 median --depth 2        the top two levels only
  chiara-frame.py 5 median --counters       + every counter track
  chiara-frame.py 5 median --build gcc-dev-chiara
"""


def main():
    parser = argparse.ArgumentParser(
        description="Show one frame of a Chiara capture against the distribution "
                    "it came from.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=HELP,
    )
    parser.add_argument("seconds", type=float, metavar="SECONDS",
                        help="where to look, in seconds from the capture's first frame")
    parser.add_argument("target", nargs="?", default="nearest", choices=TARGETS,
                        help="which frame to take from around there (default: nearest); "
                             "see SELECTING A FRAME below")
    parser.add_argument("--window", default=str(DEFAULT_WINDOW_S), metavar="SEC",
                        help=f"half-width of the search window, in seconds "
                             f"(default {DEFAULT_WINDOW_S}); 'all' searches the whole "
                             f"capture. Ignored by the nearest target")
    parser.add_argument("--capture", metavar="PATH",
                        help="read this capture instead of asking")
    parser.add_argument("--build", metavar="NAME",
                        help="take captures from this build, and ask only which one")
    parser.add_argument("--hot", action="store_true",
                        help="also rank scopes by self time — which slice holds the "
                             "cost rather than contains it")
    parser.add_argument("--counters", action="store_true",
                        help="also show every counter track, with the same comparison")
    parser.add_argument("--depth", type=int, metavar="N",
                        help="hide scopes nested deeper than N (the frame itself is 0)")
    parser.add_argument("--min-ms", type=float, default=NOISE_MS, metavar="MS",
                        help=f"hide scopes under this in both value and median "
                             f"(default {NOISE_MS}) — below it the number is the timer, "
                             f"not the code")
    parser.add_argument("--no-color", action="store_true",
                        help="plain text; also the default when stdout is not a terminal")
    args = parser.parse_args()

    style = Style(not args.no_color and sys.stdout.isatty())
    half = None if args.window == "all" else float(args.window)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = resolve_capture(args, root, style)

    size_mb = os.path.getsize(path) / (1 << 20)
    done = progress(f"reading {os.path.basename(path)} ({size_mb:.0f} MB)…", style)
    ordered, counters, frames, dropped = load(path)
    done()

    done = progress(f"walking {len(frames)} frames…", style)
    work = cpu_work_per_frame(ordered, frames)
    index, note = select(frames, work, args.seconds, args.target, half)
    stats, rows = analyze(ordered, index)
    done()

    print()
    print_header(path, frames, work, index, note, dropped, style)
    print_standout(rows, stats, style)
    print_tree(rows, stats, len(frames), style, args.depth, args.min_ms)
    if args.hot:
        print_hot(rows, style, limit=20)
    if args.counters:
        print_counters(counters, frames, index, style, min_frames=2)


if __name__ == "__main__":
    main()
