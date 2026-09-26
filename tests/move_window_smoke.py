# SPDX-License-Identifier: GPL-3.0-or-later
"""move_left/right, as Hyprland's movewindow: tiles trade places without covering each other,
floating windows keep their size and stop at the edge, and both move on to the next output."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import harness

compositor, probe = (str(Path(p).resolve()) for p in sys.argv[1:3])

# HEADLESS-1 tiles and sits left of HEADLESS-2, which does not.
CONFIG = """return {
    xwayland = false,
    layout = { tiling = false },
    outputs = {
        order = { "HEADLESS-1", "HEADLESS-2" },
        monitors = {
            ["HEADLESS-1"] = { mode = "1280x720", tiling = true },
            ["HEADLESS-2"] = { mode = "1280x720" },
        },
    },
}"""

with tempfile.TemporaryDirectory(prefix="shaode-move-window-test-") as directory:
    root = Path(directory)
    init = root / "init.lua"
    init.write_text(CONFIG)
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="2")
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET"):
        env.pop(name, None)

    def msg(*words):
        result = subprocess.run([compositor, "msg", *words], env=env, capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == 0, (words, result.stdout, result.stderr)
        return result.stdout

    def windows():
        """(focused, tiled, x, y, width, height, output) per window, oldest first."""
        rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
        return [(r[1] == "1", r[3] == "1", *map(int, r[4:8]), r[10]) for r in rows]

    def wait_for(predicate, message):
        harness.wait_for(predicate, processes, message, detail=lambda: f"windows: {windows()}")

    def overlap(a, b):
        return not (a[2] + a[4] <= b[2] or b[2] + b[4] <= a[2] or
                    a[3] + a[5] <= b[3] or b[3] + b[5] <= a[3])

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(init)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            wait_for(lambda: "Control socket" in log.read_text(), "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            for count in (1, 2):
                processes.append(subprocess.Popen([probe, "--external-control"], env=env,
                                                  stdout=subprocess.DEVNULL))
                wait_for(lambda: len(windows()) == count, f"window {count} mapped")

            def tiled_pair():
                a, b = windows()
                return a[1] and b[1] and a[3] == b[3] and a[5] == b[5] and not overlap(a, b)

            # B opens in the half under the pointer, at the origin.
            wait_for(lambda: tiled_pair() and windows()[1][2] < windows()[0][2],
                     "B tiled left of A")
            assert windows()[1][0], "the newest window should have focus"

            # At the edge, with no output to the left, nothing happens.
            msg("move_left")
            assert windows()[1][2] < windows()[0][2] and tiled_pair(), windows()
            # B trades places with A, and back; neither ever covers the other.
            msg("move_right")
            wait_for(lambda: tiled_pair() and windows()[1][2] > windows()[0][2], "B right of A")
            msg("move_left")
            wait_for(lambda: tiled_pair() and windows()[1][2] < windows()[0][2], "B left again")
            msg("move_right")
            wait_for(lambda: tiled_pair() and windows()[1][2] > windows()[0][2], "B right again")

            # From the right edge B moves onto HEADLESS-2, which does not tile: it floats at its
            # left edge, and A takes all of HEADLESS-1.
            msg("move_right")
            wait_for(lambda: windows()[1][6] == "HEADLESS-2" and not windows()[1][1],
                     "B floating on HEADLESS-2")
            a, b = windows()
            assert b[0], "B lost focus"
            assert 1280 <= b[2] < 1280 + 40, f"B not at the left edge of HEADLESS-2: {b}"
            wait_for(lambda: windows()[0][4] > 1100, "A fills HEADLESS-1")

            # A floating window moves to the edge, keeps its size, then stops.
            size = windows()[1][4:6]
            msg("move_right")
            wait_for(lambda: windows()[1][2] + windows()[1][4] > 2560 - 40, "B at the right edge")
            assert windows()[1][4:6] == size, f"B changed size: {windows()[1]} vs {size}"
            before = windows()[1]
            msg("move_right")
            assert windows()[1] == before, "B moved past the last output"

            # Back to the left edge, then into HEADLESS-1's tiling on its right side.
            msg("move_left")
            wait_for(lambda: windows()[1][2] < 1280 + 40 and windows()[1][6] == "HEADLESS-2",
                     "B at the left edge of HEADLESS-2")
            msg("move_left")
            wait_for(lambda: windows()[1][6] == "HEADLESS-1" and tiled_pair() and
                     windows()[1][2] > windows()[0][2], "B tiled right of A on HEADLESS-1")

            # With tiling off the windows float: moving one never resizes it to half the output.
            msg("toggle_tiling")
            wait_for(lambda: not any(w[1] for w in windows()), "floating")
            size = windows()[1][4:6]
            msg("move_left")
            wait_for(lambda: windows()[1][2] < 40, "B at the left edge")
            assert windows()[1][4:6] == size, f"B resized while floating: {windows()[1]}"
            assert windows()[1][6] == "HEADLESS-1", windows()

            for window in processes[1:]:
                window.kill()
                window.wait(timeout=5)
            del processes[1:]
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Tiles and floating windows moved within and across outputs")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
