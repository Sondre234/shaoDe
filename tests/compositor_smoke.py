# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise a real compositor in an isolated headless runtime directory."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

from harness import wait_for

compositor, probe, example = map(lambda p: str(Path(p).resolve()), sys.argv[1:4])
task_model_test = str(Path(sys.argv[4]).resolve()) if len(sys.argv) > 4 else None

# Reject standalone mode inside a GUI before attempting device/session access.
for arguments, expected in [
    (["--session"], "outside an existing graphical session"),
    (["--session", "--headless"], "choose only one backend"),
]:
    result = subprocess.run([compositor, "--config", example, *arguments],
                            env=dict(os.environ, WAYLAND_DISPLAY="test-parent"),
                            capture_output=True, text=True, timeout=5)
    assert result.returncode != 0 and expected in result.stderr, result.stderr

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
            wait_for(lambda: "Running Wayland compositor" in log.read_text(), [process], "startup")
            socket = re.search(r"WAYLAND_DISPLAY=(\S+)", log.read_text()).group(1)
            env["WAYLAND_DISPLAY"] = socket
            # Browsers and Electron apps expect these beyond the core desktop protocols.
            advertised = set(subprocess.run([probe, "--globals"], env=env, check=True,
                                            capture_output=True, text=True, timeout=10).stdout.split())
            expected = {"wp_viewporter", "wp_fractional_scale_manager_v1", "zxdg_output_manager_v1",
                        "wp_presentation", "zwp_primary_selection_device_manager_v1",
                        "zwlr_data_control_manager_v1", "ext_data_control_manager_v1",
                        "xdg_activation_v1", "zwp_relative_pointer_manager_v1",
                        "zwp_pointer_constraints_v1", "wp_single_pixel_buffer_manager_v1",
                        "zxdg_exporter_v2", "zxdg_importer_v2", "xdg_wm_dialog_v1",
                        "zwlr_gamma_control_manager_v1", "zwlr_screencopy_manager_v1",
                        "zwlr_export_dmabuf_manager_v1", "ext_image_copy_capture_manager_v1",
                        "ext_output_image_capture_source_manager_v1",
                        "ext_foreign_toplevel_list_v1",
                        "ext_foreign_toplevel_image_capture_source_manager_v1"}
            assert expected <= advertised, f"missing globals: {sorted(expected - advertised)}"
            for _ in range(3):
                subprocess.run([probe], env=env, check=True, timeout=10)
            if task_model_test:
                subprocess.run([task_model_test, probe], env=env, check=True, timeout=15)
            config.write_text("return {appearance={background='#315071'}, layout={gap=12}}")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: "Configuration reloaded" in log.read_text(), [process], "valid reload")
            config.write_text("return { layout = {gap = -1} }")
            process.send_signal(signal.SIGHUP)
            wait_for(lambda: "Reload rejected" in log.read_text(), [process], "rejected reload")
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
