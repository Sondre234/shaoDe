#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Try the standalone DRM/libinput session on real hardware. Run from a text console (not inside
# a graphical session). Logs to ~/.local/state/shaode/tty-test-latest.log. Set SHAODE_TEST_LIMIT
# to a number of seconds to quit on its own in case input stops working.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
bin=${SHAODE_BIN:-$repo/build/shaode}
limit=${SHAODE_TEST_LIMIT:-}
term=${SHAODE_TEST_TERM:-foot}
logdir=${XDG_STATE_HOME:-$HOME/.local/state}/shaode
mkdir -p "$logdir"
log=$logdir/tty-test-$(date +%Y%m%d-%H%M%S).log
ln -sf "$log" "$logdir/tty-test-latest.log"

{
    echo "tty: $(tty) date: $(date)"
    env | grep -E '^(XDG_|WLR_|WAYLAND_DISPLAY|DISPLAY)' || true
} >"$log"
if [ -n "$limit" ]; then
    echo "shaoDe TTY test: log $log, auto-quit after ${limit}s (Super+M quits sooner)"
    set -- timeout -s TERM -k 10 "$limit"
else
    echo "shaoDe TTY test: log $log, no time limit (Super+M quits)"
    set --
fi
sleep 2

rc=0
dbus-run-session -- "$@" \
    "$bin" --config "$repo/config/init.lua" --session --exec "$term" >>"$log" 2>&1 || rc=$?
echo "exit status $rc" | tee -a "$log"
