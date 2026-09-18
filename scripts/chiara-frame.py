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
    scripts/chiara-frame.py 15 busiest --window all   # most active, whole run
    scripts/chiara-frame.py 15 median --hot    # + self-time ranking

With no --capture, it asks which build and which capture to read.

Scopes are keyed by their path through the tree, not by name, so the two
`pp-resolve` steps in a post-process chain keep separate statistics. A path that
is not in every frame reports how many frames it appeared in.
"""

import argparse
import array
import bisect
import difflib
import hashlib
import json
import pickle
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

# Every rule, table and wrapped legend line is this wide, so the sections stack
# into one shape instead of a ragged edge.
WIDTH = 104


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

# Bumped whenever the cached shape changes, so a stale entry is a miss rather
# than a crash — or worse, than an entry read as the wrong type.
CACHE_VERSION = 2

# Cached captures kept, newest first. Each is a few MB and a capture is usually
# asked about several times and then never again.
CACHE_KEEP = 8


def cache_path(capture):
    """A cache file keyed by the capture's identity and its mtime and size, so
    a rewritten capture at the same path misses rather than answering stale."""
    root = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    stat = os.stat(capture)
    key = f"{os.path.abspath(capture)}:{stat.st_mtime_ns}:{stat.st_size}:{CACHE_VERSION}"
    digest = hashlib.sha256(key.encode()).hexdigest()[:16]
    return os.path.join(root, "chiara-frame", f"{digest}.pickle")


def cache_read(capture):
    """The parsed capture, or None. Any failure is a miss: a cache that can
    raise is worse than no cache."""
    try:
        with open(cache_path(capture), "rb") as handle:
            names, kinds, ids, stamps, values, dropped = pickle.load(handle)
    except Exception:
        return None

    scopes, counters = [], []
    for kind, name_id, ts, value in zip(kinds, ids, stamps, values):
        if kind:
            counters.append({"name": names[name_id], "ts": ts, "args": {"v": value}})
        else:
            scopes.append({"name": names[name_id], "ts": ts, "dur": value})
    frames = [e for e in scopes if e["name"] == FRAME_SCOPE]
    return scopes, counters, frames, dropped


def cache_write(capture, ordered, counters, dropped):
    """Scopes and counters as parallel typed arrays: 5 MB and a memcpy to read,
    against 25 MB of pickled dictionaries and half a second to rebuild them."""
    names, ids = {}, array.array("I")
    kinds = array.array("B")
    # Both doubles: Chiara timestamps carry fractional microseconds, and
    # rounding them to integers collapses the nesting the whole tree is built
    # from — a parent and its first child can end up starting at the same tick.
    stamps, values = array.array("d"), array.array("d")

    def add(kind, name, ts, value):
        if name not in names:
            names[name] = len(names)
        kinds.append(kind)
        ids.append(names[name])
        stamps.append(float(ts))
        values.append(float(value))

    for event in ordered:
        add(0, event["name"], event["ts"], event["dur"])
    for event in counters:
        value = event["args"].get("v", event["args"])
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            add(1, event["name"], event["ts"], value)

    table = [n for n, _ in sorted(names.items(), key=lambda kv: kv[1])]
    target = cache_path(capture)
    try:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        # Written beside the target and renamed, so an interrupted write cannot
        # leave a half-file that later reads as a hit.
        temporary = target + f".{os.getpid()}.tmp"
        with open(temporary, "wb") as handle:
            pickle.dump((table, kinds, ids, stamps, values, dropped), handle, -1)
        os.replace(temporary, target)
        prune_cache(os.path.dirname(target))
    except Exception:
        pass  # a cache that cannot be written is not an error


def prune_cache(directory):
    entries = [os.path.join(directory, f) for f in os.listdir(directory)
               if f.endswith(".pickle")]
    for stale in sorted(entries, key=os.path.getmtime, reverse=True)[CACHE_KEEP:]:
        try:
            os.remove(stale)
        except OSError:
            pass


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


def active_per_frame(ordered, frames):
    """Frame duration minus its pacing sleep: the frame's active time.

    Active rather than "CPU work": the main thread also blocks on the swapchain
    and on the present queue, and that blocking sits inside this figure because
    it sits inside the scope tree. An app that publishes a frame/cpu-ms counter
    has already subtracted it; the header prints both, and the gap between them
    is the waiting.

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


def analyze(ordered, wants):
    """Walks every frame once: per-path statistics, and the rows of some frames.

    Every wanted frame is collected in the one walk so they share the path ids
    interned here. Two separate walks would number the same path differently,
    and comparing frames across them would compare unrelated scopes.

    Scopes are identified by their position in the tree rather than by name, so
    a chain that resolves twice keeps the two steps apart. Paths are interned to
    integers — building a string key per event is the single largest cost in
    this program, and nothing downstream needs the string.
    """
    path_ids = {}      # (parent id, name, ordinal) -> path id
    path_name = []     # path id -> scope name
    samples = defaultdict(list)

    wants = set(wants)
    index, rows, selected = -1, None, {}
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
            rows = [] if index in wants else None
            if rows is not None:
                selected[index] = rows
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
    return stats, selected


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
        # With no timestamp there is nothing to be near; the middle of the
        # capture is the reading that assumes least.
        if at_s is None:
            at_s = (frames[-1]["ts"] - origin) / 2e6
            best = min(range(len(frames)),
                       key=lambda i: abs((frames[i]["ts"] - origin) / 1e6 - at_s))
            return best, f"nearest the middle of the capture (t={at_s:.3f} s)"
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


def rule(label, style, width=WIDTH):
    """A titled horizontal rule. The eye needs somewhere to stop between
    sections when the screen is otherwise a field of numbers."""
    head = f"── {label} " if label else ""
    return style(head + "─" * max(0, width - len(head)), Style.DIM)


def share_bar(fraction, style, width=8):
    """Filled proportion of a scope's frame, on a dim track so a part-full bar
    is judged against the whole it is part of.

    A scope too small to fill a cell gets blank space instead of an empty
    track. Most scopes in a frame are that small, and eight characters of
    texture on every one of them buries the handful that carry the time.
    """
    filled = int(round(fraction * width))
    if not filled:
        return " " * width
    return style("█" * filled, Style.CYAN) + style("░" * (width - filled), Style.DIM)


def plain_english(percentile_rank, what):
    """The percentile as a sentence. 'p98' is precise and means nothing at a
    glance; 'slower than 98% of frames' means something and costs a line."""
    if percentile_rank >= 50:
        return f"{what} than {percentile_rank:.0f}% of frames"
    return f"{what} than only {percentile_rank:.0f}% of frames"


# What an app may publish about its own frame, in the order a reader wants it.
# The tool derives none of these: the app knows which of its waiting is
# deliberate and which is a stall, and re-deriving that here would be a guess.
FRAME_ACCOUNTING = [
    ("frame/cpu-ms", "cpu", "work on the main thread"),
    ("frame/gpu-ms", "gpu", "the frame's work on the device"),
    ("frame/gpu-wait-ms", "gpu wait", "main thread blocked on the device or the display"),
    ("frame/sleep-ms", "sleep", "deliberate pacing"),
    ("frame/unaccounted-ms", "unaccounted", "main thread descheduled, doing nothing"),
]


def print_accounting(counters, style):
    """The app's own split of a median frame, when it publishes one.

    Worth more than anything derived here: `active` above cannot separate work
    from blocking, and no scope tree can show time the thread was descheduled,
    because a descheduled thread opens no scopes.
    """
    series = defaultdict(list)
    for event in counters:
        value = event["args"].get("v", event["args"])
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            series[event["name"]].append(float(value))

    rows = [(label, sorted(series[key]), gloss)
            for key, label, gloss in FRAME_ACCOUNTING if series.get(key)]
    if not rows:
        return

    print(style("  the app's own accounting of a median frame", Style.BOLD))
    for label, values, gloss in rows:
        print(f"    {label:<12}{statistics.median(values):8.3f} ms   "
              + style(gloss, Style.DIM))
    print()


def print_capture(path, frames, work, dropped, counters, style):
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
    print(row("active", ranked_work, "the frame minus its pacing sleep"))
    if dropped:
        print(style(f"  {dropped} negative-duration event(s) dropped "
                    "(capture stopped mid-scope)", Style.DIM))
    print()
    print_accounting(counters, style)


def print_frame_card(frames, work, index, note, style):
    origin = frames[0]["ts"]
    durations = sorted(f["dur"] for f in frames)
    ranked_work = sorted(work)
    frame = frames[index]
    at = (frame["ts"] - origin) / 1e6
    faster = 100 * bisect.bisect_left(durations, frame["dur"]) / len(durations)
    busier = 100 * bisect.bisect_left(ranked_work, work[index]) / len(ranked_work)

    print(rule(style(f"FRAME #{index}", Style.BOLD) + style(f"  at t = +{at:.3f} s", Style.DIM),
               style))
    print(f"  duration   {style(f'{frame['dur'] / 1000:7.3f} ms', Style.BOLD)}   "
          + style(plain_english(faster, "slower"), Style.DIM))
    print(f"  active     {style(f'{work[index] / 1000:7.3f} ms', Style.BOLD)}   "
          + style(plain_english(busier, "busier"), Style.DIM))
    print(style(f"  chosen as  {note}", Style.DIM))
    # A capped frame's duration is mostly how long it waited, so the two
    # rankings come apart: the shortest frame in this capture is one of the
    # busiest, having slept least. Saying so beats letting it mislead.
    if abs(faster - busier) >= 25:
        print(style(f"  note       this frame's duration and its work disagree "
                    f"(p{faster:.0f} against p{busier:.0f}) — frame length here is "
                    f"mostly pacing.", Style.YELLOW))
        print(style("             `busiest` and `idlest` select on work instead.",
                    Style.YELLOW))
    print()


def print_standout(rows, stats, style, limit=8):
    """The few scopes this frame did something unusual in.

    The tree below has every number in it, which is the problem: a hundred rows
    of ordinary values hide the three that moved. This is the same data filtered
    to what a reader would have gone looking for.
    """
    picks = []
    for depth, pid, event in rows:
        # The frame and its pacing sleep are not findings. A frame selected on
        # duration is trivially unusual in both, and saying so would put "slept
        # 0.09 ms longer than usual" above the work that actually moved.
        if depth == 0 or (depth == 1 and event["name"] == SLEEP_SCOPE):
            continue
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


def print_legend(entries, style, label_w=15):
    """Term-and-gloss lines, wrapped and hung under a common margin. A legend
    that runs past the table it explains reads as a second, wider table."""
    for term, gloss in entries:
        for i, line in enumerate(textwrap.wrap(gloss, WIDTH - label_w - 3)):
            head = f"  {term:<{label_w}}" if i == 0 else " " * (label_w + 2)
            print(style(head + line, Style.DIM))


def rank_cell(rank, style):
    """The percentile, coloured by which tail it is in.

    No bar: a linear fill saturates across p95..p100, which is the whole range
    worth telling apart, and a second bar beside the share bar made a wall of
    blocks that neither could be read through.
    """
    colour = (Style.RED,) if rank >= 95 else (Style.GREEN,) if rank <= 5 else (Style.DIM,)
    return style(f"{'p' + format(rank, '.0f'):>5}", *colour)


def print_tree(rows, stats, frame_count, style, max_depth, min_ms, other=None):
    # Shares are of the frame's work, not of the frame. A pacing-capped frame is
    # mostly sleep, against which every real scope rounds to nothing and a bar
    # of it is a blank column.
    sleep = sum(e["dur"] for d, _, e in rows if d == 1 and e["name"] == SLEEP_SCOPE)
    work = rows[0][2]["dur"] - sleep
    # Wide enough that the deepest scope in a render frame — a submit nested
    # five levels down — still reads as its own name rather than a prefix.
    name_w = 32

    # Against another frame, the two right-hand columns become that frame and
    # the comparison is with it — the median of a scope over the whole run is
    # not what you asked about once you named a frame to compare to.
    if other:
        vs_index, vs_note, vs_rows = other
        baseline = {pid: e["dur"] for _, pid, e in vs_rows}
        head_delta, head_right = f"Δ vs #{vs_index}", f"#{vs_index}"
    else:
        baseline, head_delta, head_right = None, "Δ vs MEDIAN", "MEDIAN"

    print(rule(style("EVERY SCOPE IN THIS FRAME", Style.BOLD)
               + (style(f"  against frame #{other[0]} ({other[1]})", Style.DIM)
                  if other else ""), style))
    print(style(f"    {'SCOPE':<{name_w}} {'ms':>8}  {'% OF ACTIVE':<13}  "
                f"{head_delta:>17}  {'RANK':>5}  "
                f"{head_right:>8}" + ("" if other else f"{'p90':>8}"), Style.BOLD))

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

        # Most of a frame's scopes are both small and behaving. Printed at full
        # weight they are sixty rows of identical texture with the interesting
        # dozen buried in it, so they are dropped to background instead.
        quiet = not unusual and not sleeping and share < 0.02

        # A scope that sits some frames out is marked where it is read rather
        # than given a column that says `·` on nine rows in ten.
        if len(sorted_v) < frame_count:
            partial[event["name"]] = len(sorted_v)
        label = "  " * depth + event["name"] + ("*" if len(sorted_v) < frame_count else "")
        name = f"{label:<{name_w}.{name_w}}"
        name = style(name, Style.BOLD) if unusual else name

        if sleeping:
            pct_text = "    —"
        elif share >= 0.005:
            pct_text = f"{100 * share:4.0f}%"
        else:
            pct_text = "  <1%"

        if baseline is None:
            against = median
            right = f"{median / 1000:8.3f}{percentile(sorted_v, 90) / 1000:8.3f}"
        elif pid in baseline:
            against = baseline[pid]
            right = f"{against / 1000:8.3f}"
        else:
            # The other frame did not run this scope at all, so there is
            # nothing to subtract; the row still belongs in the tree.
            against = None
            right = style(f"{'—':>8}", Style.DIM)

        row = (f"  {mark} {name} "
               f"{event['dur'] / 1000:8.3f}  "
               # The frame and its sleep have no share of the work; an empty
               # track would read as zero rather than as not applicable.
               + (" " * 8 if sleeping else share_bar(share, style)) + pct_text + "  "
               + (delta_cell(event["dur"], against, style) if against is not None
                  else style(f"{'—':>17}", Style.DIM))
               + "  " + rank_cell(rank, style) + "  "
               + style(right, Style.DIM))
        print(style(row, Style.DIM) if quiet else row)

    print()
    entries = [
        ("ms", "this scope in this frame"),
        ("% OF ACTIVE", f"its part of the frame's {work / 1000:.3f} ms of active time "
                        "— the frame minus its pacing sleep, which is why those two "
                        "rows show —. Active time includes blocking on the GPU"),
        ("Δ vs MEDIAN", "how far this frame is from what this scope usually costs; "
                        "— when the median is under a microsecond"),
        ("RANK", "where this frame lands among that scope's own frames: p50 typical, "
                 "p95+ slower than usual, p5- faster"),
        ("MEDIAN  p90", "what this scope normally costs, and its ordinary upper end — a "
                        "p90 near the median is a steady scope, one well above it a "
                        "scope that varies frame to frame"),
        ("▲ ▼", "unusual for this scope — the same rows listed above"),
        ("dimmed", "under 2% of the work and behaving; nothing to look at"),
    ]
    if partial:
        listed = ", ".join(f"{n} {c}" for n, c in sorted(partial.items()))
        entries.append(("*", "did not run in every frame, so its MEDIAN and p90 are over "
                             f"the frames it did — {listed}, of {frame_count}"))
    print_legend(entries, style)
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

    # Ranked against the frame's work, not the frame. Leaving the sleep in puts
    # it first at 96% and scales every real slice to a single block, which is
    # the one thing this ranking exists to avoid.
    ranked = sorted(((e["dur"] - children[p], e) for d, p, e in rows
                     if not (d <= 1 and e["name"] in (SLEEP_SCOPE, FRAME_SCOPE))),
                    key=lambda r: -r[0])
    sleep = sum(e["dur"] for d, _, e in rows if d == 1 and e["name"] == SLEEP_SCOPE)
    total = rows[0][2]["dur"] - sleep

    print(rule(style("SELF TIME", Style.BOLD)
               + style("  (a scope's own time, excluding the scopes nested in it)",
                       Style.DIM), style))
    if not ranked:
        print(style("  nothing but the pacing sleep in this frame", Style.DIM))
        print()
        return
    for self_us, event in ranked[:limit]:
        if self_us / 1000 < NOISE_MS:
            continue
        bar = "█" * max(1, round(30 * self_us / ranked[0][0])) if ranked[0][0] else ""
        print(f"  {event['name']:<30.30} {self_us / 1000:8.3f} ms "
              f"{100 * self_us / total if total else 0:5.1f}%  "
              + style(bar, Style.CYAN))
    print(style(f"  shares are of the frame's {total / 1000:.3f} ms of active time; "
                "the pacing sleep is left out so the scale belongs to the rest",
                Style.DIM))
    print()


def sparkline(values, width=88):
    """The series bucketed to one column each, as eighth-blocks.

    Bucketed by maximum rather than by mean: a spike that lasts one frame is
    the thing worth seeing, and averaging it into its neighbours is exactly
    how it disappears.
    """
    if not values:
        return ""
    blocks = " ▁▂▃▄▅▆▇█"
    step = len(values) / width
    peak = max(values) or 1
    out = []
    for column in range(width):
        lo, hi = int(column * step), max(int((column + 1) * step), int(column * step) + 1)
        chunk = values[lo:hi]
        out.append(blocks[min(int(max(chunk) / peak * 8), 8)] if chunk else " ")
    return "".join(out)


def print_scope(name, ordered, frames, style, limit=8):
    """One scope across the whole capture, rather than one frame across scopes.

    The frame view answers "was this frame unusual"; it cannot answer "is this
    scope always like that", which is the question a suspicious row provokes.
    """
    origin = frames[0]["ts"]
    ends = [f["ts"] + f["dur"] for f in frames]
    starts = [f["ts"] for f in frames]

    # Each occurrence is charged to the frame containing it, so a scope that
    # runs twice in a frame is one sample of their sum, matching the tree.
    per_frame = defaultdict(float)
    for event in ordered:
        if event["name"] != name:
            continue
        at = bisect.bisect_right(starts, event["ts"]) - 1
        if 0 <= at < len(frames) and event["ts"] < ends[at]:
            per_frame[at] += event["dur"]

    if not per_frame:
        known = sorted({e["name"] for e in ordered})
        near = [k for k in known if name.lower() in k.lower()]
        near += [k for k in difflib.get_close_matches(name, known, 5, 0.5) if k not in near]
        print(rule(style(f"NO SCOPE NAMED {name!r}", Style.BOLD, Style.RED), style))
        if near:
            print(style("  did you mean: " + ", ".join(near[:8]), Style.DIM))
        else:
            print(style(f"  the capture has {len(known)} scope names; "
                        "--scope takes one of them exactly", Style.DIM))
        print()
        return

    values = sorted(per_frame.values())
    series = [per_frame.get(i, 0.0) for i in range(len(frames))]

    print(rule(style(f"SCOPE  {name}", Style.BOLD)
               + style(f"  across {len(frames)} frames", Style.DIM), style))
    print(f"  ran in     {len(per_frame)} frames"
          + style(f"  ({100 * len(per_frame) / len(frames):.0f}% of the capture)",
                  Style.DIM))
    print(f"  {'ms':<10}{'p50':>9}{'p90':>9}{'p99':>9}{'min':>9}{'max':>9}")
    print(f"  {'':<10}" + "".join(f"{percentile(values, p) / 1000:9.3f}"
                                  for p in (50, 90, 99))
          + f"{values[0] / 1000:9.3f}{values[-1] / 1000:9.3f}")
    spread = percentile(values, 90) / percentile(values, 50) if percentile(values, 50) else 0
    print(style(f"  p90 is {spread:.1f}x the median — "
                + ("steady" if spread < 1.5 else
                   "varies frame to frame" if spread < 4 else
                   "wildly uneven; the median is not the story"), Style.DIM))
    print()

    print(style(f"  over the capture, peak per column, 0..{values[-1] / 1000:.3f} ms",
                Style.DIM))
    print("  " + style(sparkline(series), Style.CYAN))
    span = (frames[-1]["ts"] - origin) / 1e6
    print(style(f"  {'0 s':<44}{f'{span:.0f} s':>44}", Style.DIM))
    print()

    worst = sorted(per_frame, key=lambda i: -per_frame[i])[:limit]
    print(style("  worst frames", Style.BOLD))
    for i in worst:
        at = (frames[i]["ts"] - origin) / 1e6
        rank = 100 * bisect.bisect_left(values, per_frame[i]) / len(values)
        print(f"    #{i:<7} t=+{at:8.3f} s   {per_frame[i] / 1000:8.3f} ms   "
              + style(f"p{rank:.0f}", Style.RED if rank >= 95 else Style.DIM)
              + style(f"   ({per_frame[i] / percentile(values, 50):.1f}x the median)"
                      if percentile(values, 50) else "", Style.DIM))
    print(style(f"  re-run with `{(frames[worst[0]]['ts'] - origin) / 1e6:.3f} nearest` "
                "to open the worst of them", Style.DIM))
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
    name_w = 30
    print(rule(style("COUNTERS AT THIS FRAME", Style.BOLD), style))
    print(style(f"{'COUNTER':<{name_w}} {'VALUE':>12} {'Δ vs MEDIAN':>16}   {'RANK':>5}  "
                f"{'MEDIAN':>11}{'p10':>11}{'p90':>11}", Style.BOLD))

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
              + style(f"{median:11.4g}{low:11.4g}{high:11.4g}", Style.DIM)
              + stale)

    if constant:
        print()
        body = (f"{len(constant)} counter(s) never changed: "
                + ", ".join(f"{n}={v:g}" for n, v in constant))
        for line in textwrap.wrap(body, WIDTH - 2, subsequent_indent="  "):
            print(style("  " + line, Style.DIM))
    print()


# --- entry point ------------------------------------------------------------

TARGETS = ["nearest", "median", "fastest", "slowest", "p90", "p99", "busiest", "idlest"]


def parse_where(words, fail):
    """Returns (seconds or None, target) from up to two free positionals.

    Two optional positionals of different kinds cannot be told apart by
    argparse, but they can be told apart by looking: one of them is a number.
    That is what lets `slowest` and `15` and `15 slowest` all be typed.
    """
    seconds, target = None, None
    for word in words:
        try:
            if seconds is not None:
                raise ValueError
            seconds = float(word)
            continue
        except ValueError:
            pass
        if target is not None or word not in TARGETS:
            fail(f"unexpected argument {word!r}; expected a time in seconds and one of "
                 + ", ".join(TARGETS))
        target = word
    if target is None:
        # A bare timestamp asks for that moment; a bare target ranks the run,
        # and `nearest` over a whole capture would be the first frame, which
        # nobody means. The middle is the honest reading of "no preference".
        target = "nearest" if seconds is not None else "median"
    return seconds, target

HELP = """
SELECTING A FRAME

  SECONDS is where to look, counted from the capture's first frame. TARGET says
  which frame to take from around there. Either may be given alone, in any
  order: `15` is the frame at 15 s, `slowest` is the slowest in the whole run,
  `15 slowest` the slowest near 15 s, and no argument at all is the median
  frame of the capture.

  With SECONDS, every target except `nearest` searches a window centred on it,
  whose half-width is --window (1 s by default); the window slides to stay
  inside the capture, so asking at 0.2 s reads 0..2 s rather than a second of
  nothing. Without SECONDS there is nothing to centre on, so the search covers
  everything.

    nearest   the frame whose start is closest to SECONDS. Ignores --window.
    median    the middle frame by duration. The one to reach for: a typical
              frame, not the outlier the eye is drawn to.
    fastest   shortest frame in the window.
    slowest   longest frame in the window. Where a hitch shows itself.
    p90       a frame at the 90th duration percentile — a bad frame that is
              still representative, rather than the single worst one.
    p99       the same at the 99th.
    busiest   most active time, which is the frame's duration minus its
              pacing sleep. When frames are capped at a refresh rate, ranking
              by duration ranks them by how long they waited; this does not.
              Active time still includes blocking on the GPU, so it is not the
              same as CPU work — see WHERE A FRAME GOES below.
    idlest    least active time.

WHAT YOU GET

  Four sections, in the order you would want them:

    the capture      how long it ran and what an ordinary frame costs in it
    the frame        which frame this is, and how it ranks
    what stands out  the handful of scopes that did something unusual here.
                     Read this first — it is the whole point of the tool. When
                     a parent's excursion is entirely one child's, only the
                     child is listed, because only the child names the code.
    every scope      the full tree, for when the summary is not enough

WHERE A FRAME GOES

  Two accountings sit at the top of the report, and they do not agree.

  `active` is the tool's own: the frame minus its pacing sleep, which is
  everything the scope tree can see. It is what the % OF ACTIVE column is a
  share of, and what busiest and idlest rank on. It includes the main thread
  blocking on the swapchain and the present queue, because those waits are
  scopes like any other.

  The block beneath it is the app's, printed only when the capture carries the
  counters for it. That one separates work from blocking, which the tree
  cannot, and reports time the thread was descheduled, which no tree can — a
  descheduled thread opens no scopes, so the gap is invisible from the inside.
  Trust it over `active` where they differ, and read the difference as waiting.

  So a scope high in the tree is not automatically expensive: check whether it
  is a wait before treating it as work. A `present` or an `acquire` at the top
  of SELF TIME usually means the frame was ahead of the display, not that
  anything needs fixing.

READING THE TABLE

  Every row compares one scope in the chosen frame against that same scope
  across the whole recording:

    ms              its duration in this frame
    % OF ACTIVE     its part of the frame's active time — the frame minus its
                    pacing sleep. Against the whole frame, a capped frame is
                    95% sleep and every real scope rounds to nothing; those
                    two rows show `—` for that reason.
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

ONE SCOPE INSTEAD OF ONE FRAME

  --scope NAME turns the question around: instead of one frame across every
  scope, it shows one scope across every frame — how often it ran, its spread,
  its shape over the capture, and the frames it was worst in. This is what
  answers "is that row always like this, or was that one frame", which the
  frame view provokes and cannot settle.

COMPARING TWO FRAMES

  --vs takes another frame, chosen exactly the way the first one is
  (`--vs median`, `--vs "3 slowest"`), and puts it where the median columns
  were, so Δ is against that frame rather than against each scope's own
  history. A scope the other frame never ran shows —.

CHOOSING A CAPTURE

  With no --capture, the script lists the builds under out/build that hold
  captures, then the captures in the one you pick. A single candidate is taken
  without asking, and so is the newest when stdin is not a terminal.

  A parsed copy of each capture is kept under ~/.cache/chiara-frame, keyed by
  the capture's path, size and mtime, which takes a second run from about a
  second to about four tenths. The eight most recent are kept. --no-cache
  parses afresh.

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
    parser.add_argument("where", nargs="*", metavar="[SECONDS] [TARGET]",
                        help="where to look, in seconds from the capture's first frame, "
                             "and which frame to take from around there. Either may be "
                             "given alone: with no SECONDS the search covers the whole "
                             "capture, with no TARGET it is the nearest frame")
    parser.add_argument("--scope", metavar="NAME",
                        help="instead of a frame, show one scope across every frame — "
                             "its spread, its shape over the capture, and the frames it "
                             "was worst in")
    parser.add_argument("--vs", metavar="[SECONDS] TARGET",
                        help="compare against another frame, chosen the same way "
                             "(--vs median, --vs '3 slowest'), in place of the median "
                             "columns")
    parser.add_argument("--window", default=None, metavar="SEC",
                        help=f"half-width of the search window, in seconds "
                             f"(default {DEFAULT_WINDOW_S} when SECONDS is given, the "
                             f"whole capture when it is not); 'all' searches everything")
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
    parser.add_argument("--no-cache", action="store_true",
                        help="re-parse the capture instead of reading the parsed copy "
                             "kept under ~/.cache/chiara-frame")
    parser.add_argument("--no-color", action="store_true",
                        help="plain text; also the default when stdout is not a terminal")
    args = parser.parse_args()

    style = Style(not args.no_color and sys.stdout.isatty())
    seconds, target = parse_where(args.where, parser.error)
    # No timestamp means no place to centre a window on, so the search is the
    # whole capture: `slowest` on its own is the slowest frame in the run.
    if args.window is None:
        half = None if seconds is None else DEFAULT_WINDOW_S
    else:
        half = None if args.window == "all" else float(args.window)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = resolve_capture(args, root, style)

    size_mb = os.path.getsize(path) / (1 << 20)
    cached = None if args.no_cache else cache_read(path)
    if cached:
        ordered, counters, frames, dropped = cached
    else:
        done = progress(f"reading {os.path.basename(path)} ({size_mb:.0f} MB)…", style)
        ordered, counters, frames, dropped = load(path)
        done()
        if not args.no_cache:
            cache_write(path, ordered, counters, dropped)

    done = progress(f"walking {len(frames)} frames…", style)
    work = active_per_frame(ordered, frames)
    index, note = select(frames, work, seconds, target, half)
    wants = {index}
    other = None
    vs_index = vs_note = None
    if args.vs:
        vs_seconds, vs_target = parse_where(args.vs.split(), parser.error)
        vs_index, vs_note = select(frames, work, vs_seconds, vs_target,
                                   None if vs_seconds is None else half)
        wants.add(vs_index)
    stats, collected = analyze(ordered, wants)
    rows = collected[index]
    if vs_index is not None:
        other = (vs_index, vs_note, collected[vs_index])
    done()

    print()
    if args.scope:
        print_capture(path, frames, work, dropped, counters, style)
        print_scope(args.scope, ordered, frames, style)
        return

    print_capture(path, frames, work, dropped, counters, style)
    print_frame_card(frames, work, index, note, style)
    print_standout(rows, stats, style)
    print_tree(rows, stats, len(frames), style, args.depth, args.min_ms, other)
    if args.hot:
        print_hot(rows, style, limit=20)
    if args.counters:
        print_counters(counters, frames, index, style, min_frames=2)


if __name__ == "__main__":
    main()
