# SPDX-License-Identifier: GPL-3.0-or-later
"""Tiled windows keep gap_outer at the edges, gap_inner between them, and their border inside."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

import harness

compositor, probe = (str(Path(p).resolve()) for p in sys.argv[1:3])


def settings(inner, outer, border):
    return f"""return {{
    xwayland = false,
    layout = {{ tiling = true, gap_inner = {inner}, gap_outer = {outer} }},
    windows = {{ border_width = {border}, border_color = "#ca9ee6ff",
                 border_inactive_color = "#6c7086cc", inactive_opacity = 0.85,
                 rules = {{ {{ app_id = "^shaode-probe$", opacity = 0.9 }} }} }},
}}"""


with tempfile.TemporaryDirectory(prefix="shaode-style-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text(settings(4, 20, 3))
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET"):
        env.pop(name, None)

    def msg(*words):
        result = subprocess.run([compositor, "msg", *words], env=env, capture_output=True,
                                text=True, timeout=5, check=True)
        return result.stdout

    def boxes():
        rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
        return sorted(tuple(map(int, r[4:8])) for r in rows)

    def wait_for(predicate, message):
        harness.wait_for(predicate, processes, message, detail=lambda: f"windows: {boxes()}")

    def laid_out(inner, outer, border):
        found = boxes()
        if len(found) != 2:
            return False
        (lx, ly, lw, lh), (rx, ry, rw, rh) = found
        edge = outer + border
        # Each probe reserves a test panel along the bottom, so the height is not checked.
        return (lx == edge and ly == edge and ry == edge and lh == rh and
                rx + rw == width - edge and rx - (lx + lw) == inner + 2 * border)

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            harness.wait_for(lambda: "Control socket" in log.read_text(), processes, "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            _, _, x, y, width, height, *_ = msg("get", "outputs").split("\t")
            assert (int(x), int(y)) == (0, 0)
            width = int(width)
            for _ in range(2):
                processes.append(subprocess.Popen([probe, "--external-control"], env=env,
                                                  stdout=subprocess.DEVNULL))
            wait_for(lambda: laid_out(4, 20, 3), "gaps and borders around two tiles")

            config.write_text(settings(10, 0, 0))
            server.send_signal(signal.SIGHUP)
            wait_for(lambda: laid_out(10, 0, 0), "reloaded gaps without borders")

            config.write_text(settings(0, 12, 5))
            server.send_signal(signal.SIGHUP)
            wait_for(lambda: laid_out(0, 12, 5), "reloaded outer gap and thicker border")

            for process in processes[1:]:
                process.terminate()
                process.wait(timeout=5)
            server.send_signal(signal.SIGTERM)
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Inner and outer gaps, borders, and their reload passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
