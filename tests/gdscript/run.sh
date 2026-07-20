#!/usr/bin/env bash
set -uo pipefail
# Run goldnet's GDScript helper suites (InterpolationBuffer, PredictedBody, ServerClock).
#
# Usage:
#   ./run.sh                        # all suites
#   ./run.sh interpolation_buffer   # just one
#
# Override the engine with GODOT=/path/to/Godot. Unlike tests/integration this needs no
# built extension and no second process — the helpers are pure GDScript.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# A wedged run must not hang CI. macOS has no coreutils `timeout`, so this is rolled by hand.
TIMEOUT_S="${TIMEOUT_S:-60}"

GODOT="${GODOT:-}"
if [ -z "$GODOT" ] || [ ! -x "$GODOT" ]; then
    for candidate in \
        "/Applications/Godot.app/Contents/MacOS/Godot" \
        "$HOME/Applications/Godot.app/Contents/MacOS/Godot" \
        "$(command -v godot 2>/dev/null || true)"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then GODOT="$candidate"; break; fi
    done
fi
if [ ! -x "${GODOT:-}" ]; then
    echo "Godot binary not found. Set GODOT=/path/to/Godot" >&2
    exit 127
fi

ARGS=(--headless --path "$SCRIPT_DIR")
if [ $# -gt 0 ]; then
    ARGS+=(-- --suite "$1")
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

"$GODOT" "${ARGS[@]}" > "$log" 2>&1 &
pid=$!

waited=0
while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$TIMEOUT_S" ]; then
        echo "  (watchdog: killing the run after ${TIMEOUT_S}s)"
        kill -9 "$pid" 2>/dev/null
        break
    fi
    sleep 1
    waited=$((waited + 1))
done
wait "$pid" 2>/dev/null
rc=$?

# Godot writes its banner and any engine errors here too; show only the suite report on
# success, and everything on failure.
if [ "$rc" -eq 0 ]; then
    grep -E "^(PASS|gdscript:)" "$log"
else
    sed 's/^/  | /' "$log"
    echo ""
    echo "gdscript suites FAILED (exit $rc)"
fi
exit "$rc"
