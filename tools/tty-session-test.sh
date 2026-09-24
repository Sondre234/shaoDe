#!/bin/sh
# Try the standalone DRM/libinput session on real hardware. Run from a text console (not inside
# a graphical session). Logs to ~/.local/state/shaode/tty-test-latest.log and exits on its own
# after SHAODE_TEST_LIMIT seconds (default 300) in case input stops working.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
bin=${SHAODE_BIN:-$repo/build/shaode}
limit=${SHAODE_TEST_LIMIT:-300}
term=${SHAODE_TEST_TERM:-foot}
logdir=${XDG_STATE_HOME:-$HOME/.local/state}/shaode
mkdir -p "$logdir"
log=$logdir/tty-test-$(date +%Y%m%d-%H%M%S).log
ln -sf "$log" "$logdir/tty-test-latest.log"

{
    echo "tty: $(tty) date: $(date)"
    env | grep -E '^(XDG_|WLR_|WAYLAND_DISPLAY|DISPLAY)' || true
} >"$log"
echo "shaoDe TTY test: log $log, auto-quit after ${limit}s (Alt+Shift+Escape quits sooner)"
sleep 2

rc=0
dbus-run-session -- timeout -s TERM -k 10 "$limit" \
    "$bin" --config "$repo/config/init.lua" --session --exec "$term" >>"$log" 2>&1 || rc=$?
echo "exit status $rc" | tee -a "$log"
