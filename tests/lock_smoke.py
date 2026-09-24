"""Exercise ext-session-lock-v1 on a private headless compositor."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

compositor, lock_probe, example = (str(Path(p).resolve()) for p in sys.argv[1:4])

with tempfile.TemporaryDirectory(prefix="shaode-lock-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text(Path(example).read_text().replace("xwayland = true", "xwayland = false"))
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    for name in ("WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(name, None)
    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 5
            while "Running Wayland compositor" not in log.read_text():
                assert server.poll() is None and time.monotonic() < deadline, "startup failed"
                time.sleep(.02)
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", log.read_text())[1]
            # A crashed locker leaves the session locked; a new locker may take over.
            for mode in ("abandon", "check-locked", "cycle"):
                subprocess.run([lock_probe, mode], env=env, check=True, timeout=20)
            text = log.read_text()
            assert "Lock client vanished" in text and "Session unlocked" in text, text
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Session lock, rejection, focus isolation, abandonment, and unlock passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            if server.poll() is None:
                server.kill()
                server.wait(timeout=5)
