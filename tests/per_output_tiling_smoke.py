# SPDX-License-Identifier: GPL-3.0-or-later
"""Tiling is a setting of each output: outputs.monitors overrides layout.tiling, toggling one
output leaves the others alone, and a reload applies only tiling settings that changed."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

import harness

compositor, probe = (str(Path(p).resolve()) for p in sys.argv[1:3])


# The pointer starts at the origin, on HEADLESS-1 until HEADLESS-2 is made primary, which moves
# HEADLESS-2 under it, so the next window opens there.
def config(primary="HEADLESS-1", first="tiling = true", second=""):
    return f"""return {{
    xwayland = false,
    layout = {{ tiling = false }},
    outputs = {{
        primary = "{primary}",
        monitors = {{
            ["HEADLESS-1"] = {{ mode = "1280x720", {first} }},
            ["HEADLESS-2"] = {{ mode = "1280x720", {second} }},
        }},
    }},
}}"""


with tempfile.TemporaryDirectory(prefix="shaode-per-output-tiling-test-") as directory:
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

    def tiling():
        """Whether each output tiles, from `get workspaces`."""
        rows = [line.split("\t") for line in msg("get", "workspaces").splitlines()]
        return {row[0]: row[4] == "on" for row in rows}

    def windows():
        """(tiled, output) per window, oldest first."""
        rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
        return [(row[3] == "1", row[10]) for row in rows]

    def wait_for(predicate, message):
        harness.wait_for(predicate, processes, message,
                         detail=lambda: f"windows: {windows()}, tiling: {tiling()}")

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
            wait_for(lambda: "Control socket" in log.read_text(), "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]

            # HEADLESS-1 has its own setting; HEADLESS-2 follows layout.tiling.
            assert tiling() == {"HEADLESS-1": True, "HEADLESS-2": False}, tiling()
            assert msg("get", "tiling") == "on\n"
            for count in (1, 2):
                launch()
                wait_for(lambda: len(windows()) == count, f"window {count} mapped")
            wait_for(lambda: windows() == [(True, "HEADLESS-1")] * 2, "tiled on HEADLESS-1")

            # A window opening on HEADLESS-2 floats there. The reload changes no tiling
            # setting, so neither output changes.
            reload(config(primary="HEADLESS-2"))
            launch()
            wait_for(lambda: len(windows()) == 3, "window 3 mapped")
            wait_for(lambda: windows()[2] == (False, "HEADLESS-2"), "floating on HEADLESS-2")
            assert windows()[:2] == [(True, "HEADLESS-1")] * 2, windows()
            assert msg("get", "tiling") == "off\n", "get tiling should follow the focused output"

            # Toggling one output leaves the other as it was.
            msg("output", "HEADLESS-2", "toggle_tiling")
            wait_for(lambda: windows()[2] == (True, "HEADLESS-2"), "tiled on HEADLESS-2")
            assert tiling() == {"HEADLESS-1": True, "HEADLESS-2": True}, tiling()
            assert windows()[:2] == [(True, "HEADLESS-1")] * 2, windows()
            msg("output", "HEADLESS-1", "toggle_tiling")
            wait_for(lambda: windows() == [(False, "HEADLESS-1")] * 2 + [(True, "HEADLESS-2")],
                     "HEADLESS-1 floating, HEADLESS-2 still tiled")

            # Toggled outputs keep their state while their setting stays the same...
            reload(config(primary="HEADLESS-2", second="tiling = false"))
            assert tiling() == {"HEADLESS-1": False, "HEADLESS-2": True}, tiling()
            # ...and follow it when it changes.
            reload(config(primary="HEADLESS-2", first="tiling = false", second="tiling = false"))
            reload(config(primary="HEADLESS-2", first="tiling = true", second="tiling = false"))
            wait_for(lambda: windows() == [(True, "HEADLESS-1")] * 2 + [(True, "HEADLESS-2")],
                     "HEADLESS-1 tiled again by its changed setting")
            reload(config(primary="HEADLESS-2", first="tiling = true", second="tiling = true"))
            assert tiling() == {"HEADLESS-1": True, "HEADLESS-2": True}, tiling()
            reload(config(primary="HEADLESS-2", first="tiling = true", second="tiling = false"))
            wait_for(lambda: windows()[2] == (False, "HEADLESS-2"),
                     "HEADLESS-2 floating by its changed setting")
            assert windows()[:2] == [(True, "HEADLESS-1")] * 2, windows()

            for window in processes[1:]:
                window.kill()
                window.wait(timeout=5)
            del processes[1:]
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Per-output tiling settings, toggles, and reloads passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
