"""Run X11 clients through XWayland on a private headless compositor."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

compositor, x11_probe, wayland_probe, example = (str(Path(p).resolve()) for p in sys.argv[1:5])


def wait_for(predicate, processes, message):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        for process in processes:
            assert process.poll() is None, f"process exited ({process.returncode}): {message}"
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(f"timed out: {message}")


with tempfile.TemporaryDirectory(prefix="shaode-xwayland-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    source = Path(example).read_text()
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    for name in ("WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(name, None)

    # Disabled XWayland must not advertise an X display.
    config.write_text(source.replace("xwayland = true", "xwayland = false"))
    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        try:
            wait_for(lambda: "Running Wayland compositor" in log.read_text(), [server], "startup")
            assert "XWayland ready" not in log.read_text(), log.read_text()
        finally:
            server.terminate()
            server.wait(timeout=5)

    config.write_text(source)
    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            wait_for(lambda: "XWayland ready" in log.read_text(), processes, "XWayland startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["DISPLAY"] = re.search(r"XWayland ready on DISPLAY=(\S+)", text)[1]

            subprocess.run([x11_probe], env=env, check=True, timeout=30)
            # A second client reuses the running Xwayland; close it from the taskbar.
            client = subprocess.Popen([x11_probe, "wait-close"], env=env,
                                      stdout=subprocess.PIPE, text=True)
            processes.append(client)
            assert client.stdout.readline().strip() == "X11 window mapped and focused"
            assert client.stdout.readline().strip() == "waiting for close"
            subprocess.run([wayland_probe, "--close", "shaode-x11-probe"], env=env, check=True,
                           timeout=10)
            assert client.wait(timeout=10) == 0
            processes.remove(client)
            server.terminate()
            assert server.wait(timeout=10) == 0, log.read_text()
            print("XWayland mapping, focus, fullscreen, taskbar close, and disable passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
