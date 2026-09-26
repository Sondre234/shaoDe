# SPDX-License-Identifier: GPL-3.0-or-later
"""Take screenshots through the control socket with stand-ins for grim, slurp, and wl-copy."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness import wait_for

compositor, probe, example = (str(Path(p).resolve()) for p in sys.argv[1:4])

# Each stand-in records its arguments; grim writes the file it is given. Only shell builtins
# are used, because the compositor's PATH holds nothing but these.
TOOLS = {
    "grim": 'echo "grim $*" >> "$TOOL_LOG"; for last; do :; done; echo "fake png" > "$last"',
    # Waits, like a user choosing a region, until the test creates $TOOL_LOG.go.
    "slurp": 'echo "slurp" >> "$TOOL_LOG"; while [ ! -e "$TOOL_LOG.go" ]; do :; done; '
             'echo "10,20 30x40"',
    "wl-copy": 'IFS= read -r data; echo "wl-copy $* <$data>" >> "$TOOL_LOG"',
    "notify-send": 'echo "notify-send $*" >> "$TOOL_LOG"',
}

with tempfile.TemporaryDirectory(prefix="shaode-screenshot-test-") as directory:
    root = Path(directory)
    tools = root / "bin"
    tools.mkdir()
    for name, body in TOOLS.items():
        (tools / name).write_text("#!/bin/sh\n" + body + "\n")
        (tools / name).chmod(0o755)
    tool_log = root / "tools.log"
    tool_log.touch()
    shots = root / "shots"
    config = root / "init.lua"
    config.write_text(Path(example).read_text()
                      .replace("xwayland = true", "xwayland = false")
                      .replace('-- directory = "~/Pictures/Screenshots"',
                               f'directory = "{shots}"'))
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman",
               PATH=str(tools), TOOL_LOG=str(tool_log))
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET"):
        env.pop(name, None)

    def msg(*words, ok=True):
        result = subprocess.run([compositor, "msg", *words], env=env, capture_output=True,
                                text=True, timeout=5)
        assert (result.returncode == 0) == ok, (words, result.stdout, result.stderr)
        return result.stdout if ok else result.stderr

    def calls():
        return tool_log.read_text().splitlines()

    def saved():
        return set(shots.glob("Screenshot_*.png"))

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            wait_for(lambda: "Running Wayland compositor" in log.read_text(), processes, "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            assert "no focused window" in msg("screenshot", "window", ok=False)
            assert "mode must be" in msg("screenshot", "screen", ok=False)
            assert "one mode" in msg("screenshot", "window", "output", ok=False)

            # The output under the pointer, copied to the clipboard and announced.
            output_name = msg("get", "outputs").split("\t")[0]
            msg("screenshot", "output")
            wait_for(lambda: any(c.startswith("notify-send") for c in calls()), processes,
                     "output screenshot", detail=calls)
            (first,) = saved()
            assert first.parent == shots and re.fullmatch(
                r"Screenshot_\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.png", first.name), first
            assert first.read_text() == "fake png\n"
            assert calls() == [f"grim -o {output_name} {first}",
                               "wl-copy --type image/png <fake png>",
                               f"notify-send -a shaoDe -i {first} Screenshot saved {first}"], \
                calls()
            assert f"Screenshot saved: {first}" in log.read_text()

            # The focused window's box, in layout coordinates.
            tool_log.write_text("")
            window = subprocess.Popen([probe, "--external-control"], env=env,
                                      stdout=subprocess.DEVNULL)
            processes.append(window)

            def focused():
                rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
                return [tuple(map(int, row[4:8])) for row in rows if row[1] == "1"]
            wait_for(lambda: focused(), processes, "focused window")
            (x, y, width, height), = focused()
            msg("screenshot", "window")
            wait_for(lambda: len(calls()) == 3, processes, "window screenshot", detail=calls)
            (second,) = saved() - {first}
            assert calls()[0] == f"grim -g {x},{y} {width}x{height} {second}", calls()

            # Screenshots within the same second get distinct names. Each waits for the one
            # before to be reaped, since a request while one is running is refused.
            def screenshot_accepted(*words):
                result = subprocess.run([compositor, "msg", "screenshot", *words], env=env,
                                        capture_output=True, text=True, timeout=5)
                assert result.returncode == 0 or "already" in result.stderr, result.stderr
                return result.returncode == 0
            tool_log.write_text("")
            before = saved()
            wait_for(lambda: screenshot_accepted("output"), processes, "first quick screenshot")
            wait_for(lambda: screenshot_accepted("output"), processes, "second quick screenshot")
            wait_for(lambda: len(calls()) == 6, processes, "two quick screenshots", detail=calls)
            assert len(saved() - before) == 2, saved()

            # A region comes from slurp; without wl-copy the file is still saved.
            (tools / "wl-copy").unlink()
            tool_log.write_text("")
            before = saved()
            wait_for(lambda: screenshot_accepted(), processes, "region screenshot start")
            wait_for(lambda: calls() == ["slurp"], processes, "slurp", detail=calls)
            # Pressing Print again while choosing a region takes no second screenshot.
            assert "already being taken" in msg("screenshot", ok=False)
            assert "already being taken" in msg("screenshot", "output", ok=False)
            Path(str(tool_log) + ".go").touch()
            wait_for(lambda: len(calls()) == 3, processes, "region screenshot", detail=calls)
            (region,) = saved() - before
            assert calls()[:2] == ["slurp", f"grim -g 10,20 30x40 {region}"], calls()
            assert calls()[2].startswith("notify-send"), calls()
            assert "wl-copy is not installed" in log.read_text()

            # Missing tools are reported to the caller and nothing runs.
            (tools / "slurp").unlink()
            assert "grim and slurp" in msg("screenshot", "region", ok=False)
            (tools / "grim").unlink()
            assert "need grim" in msg("screenshot", "output", ok=False)
            assert len(saved()) == 5

            window.kill()
            window.wait(timeout=5)
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Output, window, and region screenshots and missing-tool errors passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            print(tool_log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
