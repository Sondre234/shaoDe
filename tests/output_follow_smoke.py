# SPDX-License-Identifier: GPL-3.0-or-later
"""The layout starts at 0, 0, and windows move with their output when the layout changes."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

from harness import wait_for

compositor, probe = (str(Path(p).resolve()) for p in sys.argv[1:3])

# The pointer starts at the origin, on the first output in `order`, so the window opens there.
CONFIG = """return {
    xwayland = false,
    layout = { tiling = false },
    outputs = {
        order = { %s },
        monitors = { ["HEADLESS-1"] = { mode = "1280x720" },
                     ["HEADLESS-2"] = { mode = "1280x720", %s } },
    },
}"""

with tempfile.TemporaryDirectory(prefix="shaode-output-follow-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text(CONFIG % ('"HEADLESS-1", "HEADLESS-2"', ""))
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman",
               WLR_HEADLESS_OUTPUTS="2")
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET"):
        env.pop(name, None)

    def msg(*words):
        return subprocess.run([compositor, "msg", *words], env=env, capture_output=True,
                              text=True, timeout=5, check=True).stdout

    def outputs():
        rows = [line.split("\t") for line in msg("get", "outputs").splitlines()]
        return {row[0]: tuple(int(value) for value in row[2:6]) for row in rows}

    def window():
        """x, y, and output of the only window."""
        rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
        return (int(rows[0][4]), int(rows[0][5]), rows[0][10]) if len(rows) == 1 else None

    reloads = 0

    def reload(text):
        global reloads
        reloads += 1
        config.write_text(text)
        server.send_signal(signal.SIGHUP)
        wait_for(lambda: log.read_text().count("Configuration reloaded") == reloads, processes,
                 "reload")

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            wait_for(lambda: "Control socket" in log.read_text(), processes, "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            processes.append(subprocess.Popen([probe, "--external-control"], env=env,
                                              stdout=subprocess.DEVNULL))
            wait_for(lambda: window() is not None and window()[2] == "HEADLESS-1", processes,
                     "window opened on HEADLESS-1")
            x, y, _ = window()
            assert x < 1280, window()

            # Swapping the order moves HEADLESS-1 right by 1280; its window goes along.
            reload(CONFIG % ('"HEADLESS-2", "HEADLESS-1"', ""))
            assert outputs()["HEADLESS-1"] == (1280, 0, 1280, 720), outputs()
            assert window() == (x + 1280, y, "HEADLESS-1"), window()

            # An output placed left of the origin shifts the whole layout right, since X11
            # windows get no input at negative coordinates.
            reload(CONFIG % ('"HEADLESS-1"', "position = { x = -1280, y = -100 }"))
            assert outputs() == {"HEADLESS-2": (0, 0, 1280, 720),
                                 "HEADLESS-1": (1280, 100, 1280, 720)}, outputs()
            assert window() == (x + 1280, y + 100, "HEADLESS-1"), window()

            server.send_signal(signal.SIGTERM)
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Layout origin and windows following their output passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
