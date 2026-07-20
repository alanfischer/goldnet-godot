#!/usr/bin/env bash
set -uo pipefail
# Run goldnet's in-engine integration cases. Each case is a server+client process pair
# talking over a real ENetMultiplayerPeer with a real GoldNetMultiplayer installed; the
# client holds the assertions and its exit code is the verdict.
#
# Usage:
#   ./run.sh                    # all cases in cases/
#   ./run.sh ring_expiry        # just the named case(s)
#
# Override the engine with GODOT=/path/to/Godot. Requires ../../build.sh to have been run
# — without the built extension every case fails at "GoldNetMultiplayer missing".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="$SCRIPT_DIR/.logs"

# Watchdog ceiling per case, comfortably above the longest case's own timeout_s (the case
# should report its own failure first; this only catches a genuinely wedged process).
CASE_TIMEOUT_S="${CASE_TIMEOUT_S:-45}"

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

# Named cases, or every cases/case_*.gd.
if [ $# -gt 0 ]; then
    CASES=("$@")
else
    CASES=()
    for f in "$SCRIPT_DIR"/cases/case_*.gd; do
        name="$(basename "$f" .gd)"
        CASES+=("${name#case_}")
    done
fi

# A bare `--headless --path` run doesn't scan for .gdextension files — it reads this list,
# which the editor would normally write during an import pass. Generate it so the suite
# works from a clean checkout with no editor step. (.godot/ is gitignored on purpose:
# it's derived state, so regenerating beats committing it. Note example/ takes the other
# route and commits its copy — same gotcha, two answers; see README.)
#
# Append rather than overwrite: if someone has opened this project in the editor, Godot
# writes a richer list here and clobbering it would silently drop other extensions.
EXT_LIST="$SCRIPT_DIR/.godot/extension_list.cfg"
GOLDNET_EXT="res://addons/goldnet/goldnet.gdextension"
mkdir -p "$SCRIPT_DIR/.godot"
grep -qxF "$GOLDNET_EXT" "$EXT_LIST" 2>/dev/null || echo "$GOLDNET_EXT" >> "$EXT_LIST"

mkdir -p "$LOG_DIR"
PASS=0
FAIL=0
FAILED_CASES=()

for case_name in "${CASES[@]}"; do
    echo ""
    echo "--- $case_name ---"
    server_log="$LOG_DIR/$case_name.server.log"
    case_file="$SCRIPT_DIR/cases/case_$case_name.gd"

    # Cases needing more than one client declare it with a `## @clients N` line; the
    # default is 1. Both halves get the count — each cross-checks it against what the case
    # says it needs, so a missed grep fails loudly instead of silently degrading.
    n_clients="$(grep -oE '^## @clients[[:space:]]+[0-9]+' "$case_file" | grep -oE '[0-9]+' | head -1)"
    [ -n "$n_clients" ] || n_clients=1

    "$GODOT" --headless --path "$SCRIPT_DIR" -- --server --case "$case_name" \
        --clients "$n_clients" > "$server_log" 2>&1 &
    server_pid=$!

    # The client retries its connect, so it can start immediately; but if the server died
    # on startup (missing extension, port in use) there's nothing to connect to and we'd
    # burn the client's full connect timeout. Wait for its ready line instead of a fixed
    # sleep — usually well under a second, and 8 cases of `sleep 1` was a seventh of the
    # suite's wall time.
    ready_wait=0
    while ! grep -q "ready on udp:" "$server_log" 2>/dev/null; do
        kill -0 "$server_pid" 2>/dev/null || break
        [ "$ready_wait" -ge 100 ] && break   # ~10s, then fall through to the liveness check
        sleep 0.1
        ready_wait=$((ready_wait + 1))
    done
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "FAIL $case_name: server exited during startup"
        sed 's/^/  | /' "$server_log" | tail -20
        FAIL=$((FAIL + 1)); FAILED_CASES+=("$case_name")
        continue
    fi

    # Every client must pass for the case to pass.
    client_pids=()
    client_logs=()
    for i in $(seq 0 $((n_clients - 1))); do
        log="$LOG_DIR/$case_name.client$i.log"
        "$GODOT" --headless --path "$SCRIPT_DIR" -- --client --case "$case_name" \
            --client-index "$i" --clients "$n_clients" > "$log" 2>&1 &
        client_pids+=($!)
        client_logs+=("$log")
    done

    # Hard watchdog. A case that hangs — or a script that fails to parse, leaving Godot
    # alive with no main scene — must not wedge the run forever. Rolled by hand because
    # macOS has no coreutils `timeout`.
    waited=0
    while :; do
        alive=0
        for p in "${client_pids[@]}"; do
            kill -0 "$p" 2>/dev/null && alive=1
        done
        [ "$alive" -eq 0 ] && break
        if [ "$waited" -ge "$CASE_TIMEOUT_S" ]; then
            echo "  (watchdog: killing client(s) after ${CASE_TIMEOUT_S}s)"
            for p in "${client_pids[@]}"; do kill -9 "$p" 2>/dev/null; done
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done

    client_rc=0
    for p in "${client_pids[@]}"; do
        wait "$p" 2>/dev/null || client_rc=1
    done

    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null

    if [ "$client_rc" -eq 0 ]; then
        for log in "${client_logs[@]}"; do
            grep -E "^PASS" "$log" || echo "PASS $case_name"
        done
        PASS=$((PASS + 1))
    else
        # On failure the server's side of the story is usually what explains it.
        for log in "${client_logs[@]}"; do
            grep -E "^(FAIL|  FAIL)" "$log" || true
        done
        echo "  --- server log ---"
        sed 's/^/  | /' "$server_log" | tail -15
        for log in "${client_logs[@]}"; do
            echo "  --- $(basename "$log") ---"
            sed 's/^/  | /' "$log" | tail -15
        done
        FAIL=$((FAIL + 1)); FAILED_CASES+=("$case_name")
    fi
done

echo ""
echo "======================================"
echo "integration: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "failed: ${FAILED_CASES[*]}"
    echo "logs in $LOG_DIR"
    exit 1
fi
exit 0
