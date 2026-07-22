#!/usr/bin/env bash
# Launch the sandbox under a sanitizer build with diagnostics captured to a log
# file rather than scrolling past in the terminal — a report survives even if
# the window dies.
#
# Usage:  ./scripts/run-sanitized.sh [sandbox args...]
#         ASSISI_SAN_PRESET=gcc-tsan ./scripts/run-sanitized.sh
#
# Build the preset first (e.g. `make gcc-asan`). Logs land in
# out/sanitizer-logs/, which is gitignored.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${ASSISI_SAN_PRESET:-gcc-asan}"
EXE="$ROOT/out/build/$PRESET/apps/sandbox/Assisi-Sandbox"
LOGDIR="$ROOT/out/sanitizer-logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$LOGDIR/run-$STAMP"

mkdir -p "$LOGDIR"
[ -x "$EXE" ] || { echo "Not built. Run: make $PRESET" >&2; exit 1; }

# log_path makes the sanitizers write to $LOG.<pid> instead of interleaving with
# the app's own stdout, so a report survives even if the window dies.
# halt_on_error=0 keeps UBSan reporting every distinct site in one session
# rather than aborting at the first; ASan still halts (its errors are fatal).
# NOTE: no `suppressions=` here. ASan's suppression file takes a different set
# of types (interceptor_name, odr_violation, ...) and hard-fails at startup on
# `leak:` entries — those belong to LSAN_OPTIONS below.
export ASAN_OPTIONS="log_path=$LOG:detect_leaks=1:print_stacktrace=1:strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:abort_on_error=0:disable_coredump=1"
export UBSAN_OPTIONS="log_path=$LOG:print_stacktrace=1:halt_on_error=0:report_error_type=1"
export LSAN_OPTIONS="suppressions=$ROOT/scripts/lsan-suppressions.txt:print_suppressions=0"

echo "exe : $EXE"
echo "log : $LOG.<pid>   (plus $LOG.app.log for stdout/stderr)"
echo "---- running; close the window or Ctrl-C to stop ----"

# The app is launched from its own directory so the staged asset root next to
# the executable is discovered exactly as in a normal run.
cd "$(dirname "$EXE")" || exit 1
"$EXE" "$@" 2>&1 | tee "$LOG.app.log"
STATUS=${PIPESTATUS[0]}

echo "---- exited with status $STATUS ----"
shopt -s nullglob
REPORTS=("$LOG".*)
if [ ${#REPORTS[@]} -eq 0 ]; then
    echo "No sanitizer reports were written. Clean run."
else
    for f in "${REPORTS[@]}"; do
        case "$f" in *.app.log) continue;; esac
        echo "SANITIZER REPORT: $f"
        head -40 "$f"
    done
fi
