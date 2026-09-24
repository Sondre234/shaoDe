"""Load and render both QML roots without a desktop connection."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

executable, config = (str(Path(path).resolve()) for path in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="shaode-ui-") as directory:
    root = Path(directory)
    applications = root / "applications"
    applications.mkdir()
    (applications / "test.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Test application\nExec=true\n"
    )
    empty = root / "empty"
    empty.mkdir()
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
               XDG_DATA_HOME=directory, XDG_DATA_DIRS=str(empty))
    for desktop in (False, True):
        screenshot = root / ("desktop.png" if desktop else "panel.png")
        command = [executable, "--config", config, "--preview", "--quit-after", "300",
                   "--screenshot", str(screenshot)]
        if desktop:
            command.append("--preview-desktop")
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert "ReferenceError" not in result.stderr and "TypeError" not in result.stderr, result.stderr
        image = screenshot.read_bytes()
        assert image[:8] == b"\x89PNG\r\n\x1a\n", "no PNG produced"
        width, height = struct.unpack(">II", image[16:24])
        assert width >= 640 and height >= 300 and len(image) > 1000, "empty or undersized rendering"
    print("Taskbar/launcher and desktop QML rendered successfully")
