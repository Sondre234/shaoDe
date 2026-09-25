# SPDX-License-Identifier: GPL-3.0-or-later
"""Bars stay hidden over a fullscreen window, even after another window takes focus."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

import harness

compositor, probe = (str(Path(p).resolve()) for p in sys.argv[1:3])

with tempfile.TemporaryDirectory(prefix="shaode-fullscreen-panel-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text("return { xwayland = false }")
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET"):
        env.pop(name, None)

    def msg(*words):
        result = subprocess.run([compositor, "msg", *words], env=env, capture_output=True,
                                text=True, timeout=5, check=True)
        return result.stdout

    def windows():
        return [line.split("\t") for line in msg("get", "windows").splitlines()]

    def panels():
        rows = [line.split("\t") for line in msg("get", "layers").splitlines()]
        return [row[3] == "1" for row in rows if row[0] == "shaode-test-panel"]

    def wait_for(predicate, message):
        harness.wait_for(predicate, processes, message,
                         detail=lambda: f"windows: {windows()}, panels: {panels()}")

    def stays(predicate, message):
        # Bars are updated as each frame is drawn; give a few frames the chance to undo it.
        deadline = time.monotonic() + .3
        while time.monotonic() < deadline:
            assert predicate(), f"{message}; windows: {windows()}, panels: {panels()}"
            time.sleep(.02)

    def shown():
        found = panels()
        return len(found) == len(processes) - 1 and all(found)

    def hidden():
        found = panels()
        return len(found) == len(processes) - 1 and not any(found)

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            harness.wait_for(lambda: "Control socket" in log.read_text(), processes, "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            _, _, _, _, width, height, *_ = msg("get", "outputs").split("\t")
            size = [width, height]

            # Each probe reserves a test panel along the bottom and opens a window.
            processes.append(subprocess.Popen([probe, "--external-control"], env=env,
                                              stdout=subprocess.DEVNULL))
            wait_for(lambda: len(windows()) == 1 and shown(), "first window and its panel")
            msg("fullscreen")
            wait_for(lambda: windows()[0][6:8] == size and hidden(), "fullscreen hid the panel")

            processes.append(subprocess.Popen([probe, "--external-control"], env=env,
                                              stdout=subprocess.DEVNULL))
            wait_for(lambda: len(windows()) == 2 and windows()[0][1] == "0", "second window")
            stays(hidden, "the panels came back over an unfocused fullscreen window")

            msg("workspace", "2")
            wait_for(shown, "panels on a workspace without the fullscreen window")
            msg("workspace", "1")
            wait_for(hidden, "panels hidden again on the fullscreen window's workspace")

            processes.pop().kill()
            wait_for(lambda: len(windows()) == 1 and windows()[0][1] == "1" and hidden(),
                     "focus back on the fullscreen window")
            msg("fullscreen")
            wait_for(lambda: windows()[0][6:8] != size and shown(),
                     "panel back after leaving fullscreen")

            server.send_signal(signal.SIGTERM)
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Bars hidden over fullscreen windows, focused or not")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
