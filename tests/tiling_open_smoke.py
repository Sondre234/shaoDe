# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows opening into the tiling get their tile size before their first buffer, and each
reflow sends one configure to the tiles that change and none to the rest. Also: a taskbar
maximize request leaves a fullscreen tile alone."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import harness

compositor, probe, example = (str(Path(p).resolve()) for p in sys.argv[1:4])
CONFIGURE = re.compile(r"xdg_toplevel#\d+\.configure\((-?\d+), (-?\d+),")

with tempfile.TemporaryDirectory(prefix="shaode-tiling-open-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text(Path(example).read_text().replace("xwayland = true", "xwayland = false")
                      .replace("tiling = false", "tiling = true"))
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET", "WAYLAND_DEBUG"):
        env.pop(name, None)

    def windows():
        """(x, y, width, height) per window, oldest first."""
        result = subprocess.run([compositor, "msg", "get", "windows"], env=env,
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, result.stderr
        return [tuple(map(int, line.split("\t")[4:8])) for line in result.stdout.splitlines()]

    def configures(index):
        """The sizes of every xdg_toplevel.configure the `index`th window received."""
        text = (root / f"probe{index}.log").read_text()
        return [(int(m[1]), int(m[2])) for m in CONFIGURE.finditer(text)]

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]

        def wait_for(predicate, message):
            harness.wait_for(predicate, processes, message,
                             detail=lambda: f"windows: {windows()}, configures: "
                             f"{[configures(i) for i in range(len(processes) - 1)]}")

        def launch():
            index = len(processes) - 1
            window = subprocess.Popen([probe, "--window-only"], env=dict(env, WAYLAND_DEBUG="client"),
                                      stdout=subprocess.DEVNULL,
                                      stderr=(root / f"probe{index}.log").open("w"))
            processes.append(window)
            wait_for(lambda: len(windows()) == index + 1 and
                     all(configures(i) and configures(i)[-1] == windows()[i][2:]
                         for i in range(index + 1)), f"window {index} tiled")
            # Let any late configure arrive before counting.
            subprocess.run([probe, "--globals"], env=env, capture_output=True, timeout=5)
            return [configures(i) for i in range(index + 1)]

        try:
            wait_for(lambda: "Control socket" in log.read_text(), "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]

            counts = []
            for opened in range(3):
                seen = launch()
                boxes = windows()
                # The first configure already carries the tile, so the first buffer fits it;
                # the second only activates the window.
                assert seen[opened][0] == boxes[opened][2:], (opened, seen, boxes)
                assert len(seen[opened]) <= 2, (opened, seen)
                if counts:
                    added = [len(sizes) - count for sizes, count in zip(seen, counts)]
                    # Each tile that changed hears about it once; unchanged tiles not at all.
                    for index, (sizes, new) in enumerate(zip(seen, added)):
                        changed = previous[index] != boxes[index]
                        assert new == (1 if changed or index == opened - 1 else 0), \
                            (opened, index, seen, previous, boxes)
                counts = [len(sizes) for sizes in seen]
                previous = boxes
            # The third window split the second (focused) one and left the first alone.
            assert previous[0][2:] == windows()[0][2:]

            # A taskbar maximize request leaves a fullscreen window alone, as a client's does.
            output = subprocess.run([compositor, "msg", "get", "outputs"], env=env,
                                    capture_output=True, text=True, timeout=5).stdout
            subprocess.run([compositor, "msg", "fullscreen"], env=env, check=True, timeout=5)
            full = [(0, 0, *map(int, re.search(r"(\d+)x(\d+)", output).groups()))]
            wait_for(lambda: windows()[2] == full[0], "focused window fullscreen")
            subprocess.run([probe, "--maximize", "shaode-probe"], env=env, check=True,
                           capture_output=True, timeout=5)
            subprocess.run([probe, "--globals"], env=env, capture_output=True, timeout=5)
            assert windows()[2] == full[0], windows()
            subprocess.run([compositor, "msg", "fullscreen"], env=env, check=True, timeout=5)
            wait_for(lambda: windows() == previous, "tile restored after fullscreen")

            for window in processes[1:]:
                window.kill()
                window.wait(timeout=5)
            del processes[1:]
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("New tiles configured once at their size, unchanged tiles left alone")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
