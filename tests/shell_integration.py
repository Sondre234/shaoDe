# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify real Qt layer surfaces, reserved space, and reload on a private compositor."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

from harness import wait_for

compositor, shell, probe, example = (str(Path(p).resolve()) for p in sys.argv[1:])

with tempfile.TemporaryDirectory(prefix="shaode-shell-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    source = Path(example).read_text()
    config.write_text(source)
    compositor_log, shell_log = root / "compositor.log", root / "shell.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman",
               QT_QPA_PLATFORM="wayland", QT_QUICK_BACKEND="software",
               XDG_DATA_HOME=directory, XDG_DATA_DIRS=directory)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    processes = []
    with compositor_log.open("w") as output, shell_log.open("w") as shell_output:
        try:
            server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                      env=env, stdout=output, stderr=output)
            processes.append(server)
            wait_for(lambda: "Running Wayland compositor" in compositor_log.read_text(),
                     processes, "compositor startup")
            env["WAYLAND_DISPLAY"] = re.search(
                r"WAYLAND_DISPLAY=(\S+)", compositor_log.read_text())[1]
            env["SHAODE_SOCKET"] = re.search(
                r"Control socket: (\S+)", compositor_log.read_text())[1]
            desktop = subprocess.Popen([shell, "--config", str(config)], env=env,
                                       stdout=shell_output, stderr=shell_output)
            processes.append(desktop)
            marker = "shaoDe surface rendered: shaoDe taskbar"
            wait_for(lambda: marker in shell_log.read_text(), processes, "panel rendering")
            assert "shaoDe surface rendered: shaoDe desktop" in shell_log.read_text()

            def check_panel(height):
                # A render marker can precede the compositor applying the new layer state.
                last = []

                def reserved():
                    last[:] = [subprocess.run([probe, "--external-panel", str(height)], env=env,
                                              capture_output=True, text=True, timeout=5)]
                    return last[0].returncode == 0

                wait_for(reserved, processes, f"panel reservation of {height}",
                         detail=lambda: last[0].stderr)

            check_panel(52)
            # The launcher action reaches the panel on the output under the pointer.
            def launcher():
                subprocess.run([compositor, "msg", "launcher"], env=env, check=True,
                               capture_output=True, timeout=5)

            def opened():
                return "shaoDe launcher opened on" in shell_log.read_text()

            # The shell subscribes asynchronously: resend until the first request arrives.
            for _ in range(10):
                launcher()
                try:
                    wait_for(opened, processes, "launcher opened", timeout=.5)
                    break
                except AssertionError:
                    pass
            assert opened() and "launcher closed" not in shell_log.read_text()
            launcher()
            wait_for(lambda: "shaoDe launcher closed on" in shell_log.read_text(),
                     processes, "launcher closed")
            # A live panel-height change must alter maximized client geometry.
            config.write_text(source.replace("panel_height = 52", "panel_height = 72"))
            desktop.send_signal(signal.SIGHUP)
            wait_for(lambda: shell_log.read_text().count(marker) >= 2,
                     processes, "panel resize after reload")
            check_panel(72)
            # A floating bar on top reserves its height plus the margins above and below it.
            floating = (source.replace('panel_position = "bottom"', 'panel_position = "top"')
                        .replace("panel_margin = 0,",
                                 "panel_margin = { top = 6, bottom = 4, left = 12, right = 12 },")
                        .replace("panel_radius = 0,", "panel_radius = 10,"))
            assert 'panel_position = "top"' in floating and "top = 6" in floating
            config.write_text(floating)
            desktop.send_signal(signal.SIGHUP)
            wait_for(lambda: shell_log.read_text().count(marker) >= 3,
                     processes, "panel moved to the top after reload")
            check_panel(62)
            config.write_text(source.replace("panel_height = 52", "panel_height = 72"))
            desktop.send_signal(signal.SIGHUP)
            wait_for(lambda: shell_log.read_text().count(marker) >= 4,
                     processes, "panel back at the bottom after reload")
            check_panel(72)
            # Invalid data must not kill the shell or replace the active reservation.
            config.write_text("return { shell = { panel_height = -1 } }")
            desktop.send_signal(signal.SIGHUP)
            wait_for(lambda: "Shell reload rejected:" in shell_log.read_text(),
                     processes, "invalid reload rejection")
            check_panel(72)
            config.write_text(source.replace("enabled = true", "enabled = false"))
            desktop.send_signal(signal.SIGHUP)
            assert desktop.wait(timeout=5) == 0, shell_log.read_text()
            processes.remove(desktop)
            check_panel(0)
            for message in ("ReferenceError", "TypeError", "failed", "error in client"):
                assert message not in shell_log.read_text(), shell_log.read_text()
            server.terminate()
            assert server.wait(timeout=5) == 0, compositor_log.read_text()
            print("Qt desktop/panel rendering, launcher action, reservation, resize, rejection, and disable passed")
        except Exception:
            print(compositor_log.read_text(), shell_log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
