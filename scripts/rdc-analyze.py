"""Per-pass GPU timings from a RenderDoc capture, on the terminal.

RenderDoc's GUI answers this too, but only by hand. This prints the same numbers
as text so they can be diffed, pasted, and kept next to a Chiara capture — the
CPU half of the same frame.

Usage (see docs/gpu-profiling-guide.md for the whole workflow):

    RDC_CAPTURE=capture.rdc qrenderdoc --python scripts/rdc-analyze.py

Inputs come through the environment, not argv, because qrenderdoc hands its
command line to Qt first and Qt silently swallows anything it does not
recognise -- including a trailing `-- file.rdc`. RDC_REPORT optionally sets the
output path; it defaults to <capture>-gpu.txt.

Two things that are not obvious and cost an afternoon to rediscover:

  * `--python` runs BEFORE the main window opens, and qrenderdoc opens it
    afterwards regardless. Anything driving this unattended must kill the
    process itself, which is why this ends in os._exit() rather than falling off
    the end of main(). Without that you get a GUI window that waits forever for
    someone to close it.

  * Arch's `renderdoc` package ships no importable `renderdoc` Python module, so
    `python3 thisfile.py` cannot work. qrenderdoc's embedded interpreter is the
    only way in on that distro.
"""

import os
import sys

import renderdoc as rd


def walk(actions, depth=0):
    """Flatten RenderDoc's action tree, keeping depth for indentation."""
    out = []
    for action in actions:
        out.append((depth, action))
        out.extend(walk(action.children, depth + 1))
    return out


def marker_path(depth_actions, index, sfile):
    """The debug-utils marker stack enclosing entry `index`.

    Markers are the parent nodes in the action tree (ASSISI_PROFILE_GPU_SCOPE
    emits them), so an action's pass name is just its ancestors' names. Without
    markers every action is a root and this returns empty — which is exactly the
    "350 anonymous draws" problem the markers exist to fix.
    """
    depth, _ = depth_actions[index]
    path = []
    for prev_depth, prev in reversed(depth_actions[:index]):
        if prev_depth < depth:
            path.append(prev.customName or prev.GetName(sfile) or "?")
            depth = prev_depth
            if depth == 0:
                break
    return list(reversed(path))


def main(capture_path, out_path):
    lines = []

    def emit(text=""):
        lines.append(str(text))

    def finish():
        with open(out_path, "w") as handle:
            handle.write("\n".join(lines) + "\n")
        print("\n".join(lines))

    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture_path, "", None) != rd.ResultCode.Succeeded:
        emit("could not open capture: %s" % capture_path)
        finish()
        return
    if cap.LocalReplaySupport() != rd.ReplaySupport.Supported:
        emit("this capture cannot be replayed locally (different GPU/driver?)")
        finish()
        return

    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        emit("replay failed to start: %s" % str(status))
        finish()
        return

    # EventGPUDuration is the per-event GPU time. It is measured by replaying
    # each event in isolation, so these DO NOT sum to the frame's wall-clock GPU
    # time -- barriers, layout transitions and pipeline drain between events are
    # attributed to nobody. Use them to rank passes; use Chiara's frame/gpu-ms
    # for the absolute number.
    counters = controller.EnumerateCounters()
    if rd.GPUCounter.EventGPUDuration not in counters:
        emit("this driver does not expose EventGPUDuration")
        controller.Shutdown()
        finish()
        return

    desc = controller.DescribeCounter(rd.GPUCounter.EventGPUDuration)
    times = {}
    for result in controller.FetchCounters([rd.GPUCounter.EventGPUDuration]):
        times[result.eventId] = result.value.d if desc.resultByteWidth == 8 else result.value.f

    actions = walk(controller.GetRootActions())
    sfile = controller.GetStructuredFile()

    # Roll every leaf action up into the marker range that encloses it, so the
    # report is in the engine's vocabulary (scene, light-cull, imgui) rather
    # than Vulkan's (vkCmdDispatch x350).
    passes = {}
    leaf_total = 0.0
    for i, (_, action) in enumerate(actions):
        if action.children:
            continue
        gpu = times.get(action.eventId)
        if gpu is None:
            continue
        leaf_total += gpu
        key = " / ".join(marker_path(actions, i, sfile)) or "(unmarked)"
        entry = passes.setdefault(key, {"time": 0.0, "count": 0, "cmds": {}})
        entry["time"] += gpu
        entry["count"] += 1
        cmd = action.GetName(sfile).split("(")[0]
        entry["cmds"][cmd] = entry["cmds"].get(cmd, 0) + 1

    emit("capture: %s" % capture_path)
    emit("%d actions, %d timed, %d marker groups" % (len(actions), len(times), len(passes)))
    emit("total per-event GPU: %.4f ms" % (leaf_total * 1000.0))
    emit("  (per-event times are measured in isolation and under-report the frame;")
    emit("   compare against Chiara's frame/gpu-ms for the real number)")
    emit()
    emit("%10s %7s %7s   %s" % ("GPU ms", "share", "events", "pass"))
    emit("-" * 78)
    for name, entry in sorted(passes.items(), key=lambda kv: -kv[1]["time"]):
        share = 100.0 * entry["time"] / leaf_total if leaf_total else 0.0
        cmds = ", ".join("%s x%d" % (c, n) for c, n in
                         sorted(entry["cmds"].items(), key=lambda kv: -kv[1]))
        emit("%10.4f %6.1f%% %7d   %s" % (entry["time"] * 1000.0, share, entry["count"], name))
        emit("%27s%s" % ("", cmds))

    controller.Shutdown()
    finish()


if __name__ == "__main__":
    capture = os.environ.get("RDC_CAPTURE", "")
    report = os.environ.get("RDC_REPORT", "")
    if not capture:
        report = report or "/tmp/rdc-analyze-error.txt"
        with open(report, "w") as handle:
            handle.write("set RDC_CAPTURE=<capture.rdc>\n")
    else:
        report = report or os.path.splitext(capture)[0] + "-gpu.txt"
        try:
            main(capture, report)
        except Exception:  # noqa: BLE001 - never leave the GUI up on a crash
            import traceback
            with open(report, "w") as handle:
                handle.write(traceback.format_exc())

    # See the module docstring: qrenderdoc opens its main window after this
    # script returns. Exiting here is what keeps the run unattended.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0)
