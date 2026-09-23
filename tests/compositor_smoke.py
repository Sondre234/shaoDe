"""Exercise a real compositor in an isolated headless runtime directory."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

compositor, probe, example = map(lambda p: str(Path(p).resolve()), sys.argv[1:])

# Reject standalone mode inside a GUI before attempting device/session access.
for arguments, expected in [
    (["--session"], "outside an existing graphical session"),
    (["--session", "--headless"], "choose only one backend"),
]:
    result = subprocess.run([compositor, "--config", example, *arguments],
                            env=dict(os.environ, WAYLAND_DISPLAY="test-parent"),
                            capture_output=True, text=True, timeout=5)
    assert result.returncode != 0 and expected in result.stderr, result.stderr

def wait_for(predicate, process, message):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"compositor exited with {process.returncode}: {message}")
        if predicate():
            return
        time.sleep(0.02)
    raise RuntimeError(f"timed out: {message}")

with tempfile.TemporaryDirectory(prefix="shaode-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text(Path(example).read_text())
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    with log.open("w") as output:
        process = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                   env=env, stdout=output, stderr=output)
        try:
            wait_for(lambda: "Running Wayland compositor" in log.read_text(), process, "startup")
            socket = re.search(r"WAYLAND_DISPLAY=(\S+)", log.read_text()).group(1)
            env["WAYLAND_DISPLAY"] = socket
            for _ in range(3):
                subprocess.run([probe], env=env, check=True, timeout=10)
            config.write_text("return {appearance={background='#315071'}, layout={gap=12}}")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: "Configuration reloaded" in log.read_text(), process, "valid reload")
            config.write_text("return { layout = {gap = -1} }")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: "Reload rejected" in log.read_text(), process, "rejected reload")
            subprocess.run([probe], env=env, check=True, timeout=10)
            # --check-config must reject invalid data without starting a display.
            result = subprocess.run([compositor, "--config", str(config), "--check-config"],
                                    env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode != 0 and "gap" in result.stderr
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=5) == 0, log.read_text()
            assert not (root / socket).exists(), "Wayland socket was not removed"
            print("Headless clients, maximize/restore, reload, rejection, and clean shutdown passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
