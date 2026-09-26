#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Try the standalone DRM/libinput session on real hardware. Run from a text console (not inside
# a graphical session). Logs to ~/.local/state/shaode/tty-test-latest.log. Set SHAODE_TEST_LIMIT
# to a number of seconds to quit on its own in case input stops working.
#
# Usage: tty-session-test.sh [-n | --fresh] [-p | --profile NAME]
# A profile is a directory in ~/.config/shaode/profiles holding an init.lua (and usually the
# theme.lua `shaode import` wrote beside it); one with `extends = "default"` layers over this
# tree's config/init.lua. With neither option, a menu picks a profile or a
# fresh session; Enter takes the one used last. --fresh runs the example configuration from
# this tree alone, as a first install sees it.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
bin=${SHAODE_BIN:-$repo/build/shaode}
limit=${SHAODE_TEST_LIMIT:-}
term=${SHAODE_TEST_TERM:-foot}
logdir=${XDG_STATE_HOME:-$HOME/.local/state}/shaode
profiles=${XDG_CONFIG_HOME:-$HOME/.config}/shaode/profiles

usage() {
    echo "usage: $0 [-n | --fresh] [-p | --profile NAME]" >&2
    exit 2
}

profile=
while [ $# -gt 0 ]; do
    case $1 in
    -n | --n | --fresh) profile=-fresh ;;
    -p | --profile)
        [ $# -ge 2 ] || usage
        profile=$2
        shift
        ;;
    *) usage ;;
    esac
    shift
done
mkdir -p "$logdir"

if [ -z "$profile" ]; then
    last=$(cat "$logdir/last-profile" 2>/dev/null || true)
    names=
    for dir in "$profiles"/*/; do
        [ -f "$dir/init.lua" ] && names="$names $(basename "$dir")"
    done
    set -- $names -fresh
    default=1 i=0
    echo "shaoDe profiles:"
    for name in "$@"; do
        i=$((i + 1))
        [ "$name" = "$last" ] && default=$i
        label=$name
        [ "$name" = -fresh ] && label="fresh (first install, no profile)"
        echo "  $i) $label"
    done
    printf "Choose [%s]: " "$default"
    read -r choice || choice=
    choice=${choice:-$default}
    case $choice in
    *[!0-9]* | '') usage ;;
    esac
    [ "$choice" -ge 1 ] && [ "$choice" -le $# ] || usage
    eval "profile=\${$choice}"
fi

if [ "$profile" = -fresh ]; then
    # A copy, so nothing generated beside the example (a theme.lua) comes along.
    fresh=$(mktemp -d)
    trap 'rm -rf "$fresh"' EXIT
    cp "$repo/config/init.lua" "$fresh/init.lua"
    config=$fresh/init.lua
else
    config=$profiles/$profile/init.lua
    [ -f "$config" ] || { echo "no profile '$profile': $config is missing" >&2; exit 1; }
fi
echo "$profile" >"$logdir/last-profile"

log=$logdir/tty-test-$(date +%Y%m%d-%H%M%S).log
ln -sf "$log" "$logdir/tty-test-latest.log"
{
    echo "tty: $(tty) date: $(date) profile: ${profile#-} config: $config"
    env | grep -E '^(XDG_|WLR_|WAYLAND_DISPLAY|DISPLAY)' || true
} >"$log"
if [ -n "$limit" ]; then
    echo "shaoDe TTY test (${profile#-}): log $log, auto-quit after ${limit}s (Super+M quits sooner)"
    set -- timeout -s TERM -k 10 "$limit"
else
    echo "shaoDe TTY test (${profile#-}): log $log, no time limit (Super+M quits)"
    set --
fi
sleep 2

export SHAODE_DEFAULT_CONFIG="$repo/config/init.lua"
rc=0
dbus-run-session -- "$@" \
    "$bin" --config "$config" --session --exec "$term" >>"$log" 2>&1 || rc=$?
echo "exit status $rc" | tee -a "$log"
