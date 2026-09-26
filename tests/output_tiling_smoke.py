# SPDX-License-Identifier: GPL-3.0-or-later
"""Tiled windows follow their output across two outputs of different sizes and scales: when
their output is disabled they join the other one's tiling, fullscreen fits it, and turning
tiling off leaves them floating inside it."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

import harness

compositor, probe = (str(Path(p).resolve()) for p in sys.argv[1:3])

# HEADLESS-1 is 2048x1152 logical, like a 1440p monitor at 1.25; HEADLESS-2 is smaller and
# sits to its right unless placed. The pointer starts at 0, 0.
def config(primary=None, first="position = { x = 0, y = 0 }", second=""):
    return f"""return {{
    xwayland = false,
    layout = {{ tiling = true }},
    outputs = {{{f' primary = "{primary}",' if primary else ""}
        monitors = {{
            ["HEADLESS-1"] = {{ mode = "2560x1440", scale = 1.25, {first} }},
            ["HEADLESS-2"] = {{ mode = "1600x900", {second} }},
        }},
    }},
}}"""


with tempfile.TemporaryDirectory(prefix="shaode-output-tiling-test-") as directory:
    root = Path(directory)
    init = root / "init.lua"
    init.write_text(config())
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
        """(focused, tiled, x, y, width, height) per window, oldest first."""
        rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
        return [(r[1] == "1", r[3] == "1", *map(int, r[4:8])) for r in rows]

    def outputs():
        rows = [line.split("\t") for line in msg("get", "outputs").splitlines()]
        return {r[0]: (r[1] == "1", *map(int, r[2:6])) for r in rows}

    def wait_for(predicate, message):
        harness.wait_for(predicate, processes, message,
                         detail=lambda: f"windows: {windows()}, outputs: {outputs()}")

    def inside(box, area):
        x, y, width, height = box
        return (x >= area[0] and y >= area[1] and x + width <= area[0] + area[2] and
                y + height <= area[1] + area[3])

    def disjoint(rects):
        return all(a[0] + a[2] <= b[0] or b[0] + b[2] <= a[0] or
                   a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1]
                   for i, a in enumerate(rects) for b in rects[i + 1:])

    reloads = 0

    def reload(text):
        global reloads
        reloads += 1
        init.write_text(text)
        server.send_signal(signal.SIGHUP)
        wait_for(lambda: log.read_text().count("Configuration reloaded") == reloads, "reload")

    def launch():
        processes.append(subprocess.Popen([probe, "--external-control"], env=env,
                                          stdout=subprocess.DEVNULL))

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(init)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            wait_for(lambda: "Running Wayland compositor" in log.read_text(), "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            assert msg("get", "tiling") == "on\n"
            assert outputs()["HEADLESS-1"] == (True, 0, 0, 2048, 1152), outputs()

            # Two tiles on the large output, where the pointer starts.
            for count in (1, 2):
                launch()
                wait_for(lambda: len(windows()) == count and all(w[1] for w in windows()),
                         f"window {count} tiled")
            large = outputs()["HEADLESS-1"][1:]
            wait_for(lambda: all(inside(w[2:], large) for w in windows()) and
                     disjoint([w[2:] for w in windows()]), "two tiles on the large output")

            # Moving the large output far below takes the pointer, now off every output, to
            # the nearest point of the small one: the next window tiles there.
            reload(config(first="position = { x = 0, y = 3000 }"))
            small = outputs()["HEADLESS-2"][1:]
            assert small == (2048, 0, 1600, 900), outputs()
            launch()
            wait_for(lambda: len(windows()) == 3 and windows()[2][1] and
                     inside(windows()[2][2:], small), "third window tiled on the small output")

            # Disabling the large output moves its tiles into the small output's tiling. The
            # only output left starts at the origin, far from the tiles at y = 3000, so no
            # leftover coordinates happen to fit.
            reload(config(first="enabled = false", second="position = { x = 3000, y = 200 }"))
            state = outputs()
            assert state["HEADLESS-1"][0] is False and \
                state["HEADLESS-2"] == (True, 0, 0, 1600, 900), state
            small = state["HEADLESS-2"][1:]
            wait_for(lambda: all(w[1] and inside(w[2:], small) for w in windows()) and
                     disjoint([w[2:] for w in windows()]),
                     "tiles of the disabled output rejoined the small output's tiling")

            # Fullscreen covers exactly the output the window is on now.
            msg("fullscreen")
            wait_for(lambda: [w[2:] for w in windows() if w[0]] == [small],
                     "fullscreen fits the small output")
            msg("fullscreen")
            wait_for(lambda: all(w[1] and inside(w[2:], small) for w in windows()) and
                     disjoint([w[2:] for w in windows()]), "left fullscreen back into its tile")

            # Leaving the tiling keeps every window floating inside the output it is on now,
            # at its floating size.
            msg("toggle_tiling")
            wait_for(lambda: not any(w[1] for w in windows()) and
                     all(inside(w[2:], small) and w[4:] == (320, 240) for w in windows()),
                     "floating windows stayed on the small output")

            for window in processes[1:]:
                window.kill()
                window.wait(timeout=5)
            del processes[1:]
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Tiles moved between outputs of different sizes, fullscreen, and restore "
                  "passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
