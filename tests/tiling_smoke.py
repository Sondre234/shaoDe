# SPDX-License-Identifier: GPL-3.0-or-later
"""Toggle dwindle tiling through the control socket and watch real windows follow it."""
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile

import harness

compositor, probe, example = (str(Path(p).resolve()) for p in sys.argv[1:4])
GAP = 8

with tempfile.TemporaryDirectory(prefix="shaode-tiling-test-") as directory:
    root = Path(directory)
    config = root / "init.lua"
    config.write_text(Path(example).read_text().replace("xwayland = true", "xwayland = false"))
    log = root / "compositor.log"
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_RENDERER="pixman")
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "SHAODE_SOCKET"):
        env.pop(name, None)

    def msg(*words):
        result = subprocess.run([compositor, "msg", *words], env=env, capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == 0, (words, result.stdout, result.stderr)
        return result.stdout

    def windows():
        """(workspace, focused, tiled, x, y, width, height) per window, oldest first."""
        rows = [line.split("\t") for line in msg("get", "windows").splitlines()]
        return [(int(r[0]), r[1] == "1", r[3] == "1", *map(int, r[4:8])) for r in rows]

    def wait_for(predicate, processes, message):
        harness.wait_for(predicate, processes, message, detail=lambda: f"windows: {windows()}")

    def boxes(workspace=1):
        return sorted(w[3:] for w in windows() if w[0] == workspace)

    def side_by_side(left, right):
        return (left[1] == right[1] and left[3] == right[3] and
                left[0] + left[2] + GAP == right[0])

    def disjoint(rects):
        return all(a[0] + a[2] <= b[0] or b[0] + b[2] <= a[0] or
                   a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1]
                   for i, a in enumerate(rects) for b in rects[i + 1:])

    events = b""

    def received(line):
        global events
        subscriber.setblocking(False)
        try:
            while chunk := subscriber.recv(4096):
                events += chunk
        except BlockingIOError:
            pass
        return line.encode() in events

    with log.open("w") as output:
        server = subprocess.Popen([compositor, "--headless", "--config", str(config)],
                                  env=env, stdout=output, stderr=output)
        processes = [server]
        try:
            wait_for(lambda: "Control socket" in log.read_text(), processes, "startup")
            text = log.read_text()
            env["WAYLAND_DISPLAY"] = re.search(r"WAYLAND_DISPLAY=(\S+)", text)[1]
            env["SHAODE_SOCKET"] = re.search(r"Control socket: (\S+)", text)[1]
            assert msg("get", "tiling") == "off\n"
            subscriber = socket.socket(socket.AF_UNIX)
            subscriber.connect(env["SHAODE_SOCKET"])
            subscriber.sendall(b"subscribe\n")
            wait_for(lambda: received("ok\ntiling off\nworkspace 1\n"), processes,
                     "subscription state")

            def launch():
                window = subprocess.Popen([probe, "--external-control"], env=env,
                                          stdout=subprocess.DEVNULL)
                processes.append(window)
                return window

            launch()
            wait_for(lambda: len(windows()) == 1, processes, "first window")
            assert windows()[0][2:] == (False, *windows()[0][3:]), "tiled while tiling is off"
            floating = boxes()[0]
            assert floating[2:] == (320, 240), floating

            msg("toggle_tiling")
            wait_for(lambda: received("tiling on\n"), processes, "tiling on event")
            assert msg("get", "tiling") == "on\n"
            wait_for(lambda: windows()[0][2] and boxes()[0][2] > 320, processes,
                     "single window fills the output")
            full = boxes()[0]
            assert full[0] == GAP and full[1] == GAP, full

            # A new window splits the focused one; both share the height.
            launch()
            wait_for(lambda: len(boxes()) == 2 and side_by_side(*boxes()), processes,
                     "second window tiled beside the first")
            left, right = boxes()
            assert left[0] == GAP and right[0] + right[2] == full[0] + full[2], boxes()
            assert all(w[2] for w in windows())

            # Moving a tile to another workspace gives the space back and tiles it there.
            msg("move_to_workspace", "2")
            # Every probe adds a test panel, so compare across the width only.
            def fills(workspace=1):
                found = boxes(workspace)
                return len(found) == 1 and found[0][0] == GAP and found[0][2] == full[2]
            wait_for(fills, processes, "remaining window refilled")
            assert [w[2] for w in windows() if w[0] == 2] == [True]
            msg("workspace", "2")
            wait_for(lambda: fills(2), processes, "moved window fills workspace 2")
            msg("workspace", "1")

            launch()
            wait_for(lambda: len(boxes()) == 2 and side_by_side(*boxes()), processes,
                     "third window tiled on workspace 1")
            launch()
            wait_for(lambda: len(boxes()) == 3 and all(w[2] for w in windows()) and
                     disjoint(boxes()), processes,
                     "fourth window split a tile")

            # Floating the focused window returns it to its floating size and reflows the rest.
            msg("toggle_floating")
            wait_for(lambda: sorted(b[2:] for b in boxes())[0] == (320, 240) and
                     sum(w[2] for w in windows() if w[0] == 1) == 2, processes,
                     "focused window floats")
            msg("toggle_floating")
            wait_for(lambda: sum(w[2] for w in windows() if w[0] == 1) == 3, processes,
                     "window tiled again")

            msg("toggle_tiling")
            wait_for(lambda: received("tiling off\n"), processes, "tiling off event")
            wait_for(lambda: not any(w[2] for w in windows()) and
                     all(b[2:] == (320, 240) for b in boxes() + boxes(2)), processes,
                     "floating sizes restored")

            # Toggling faster than the clients answer must not save a tile as the floating size.
            floating = boxes() + boxes(2)
            msg("toggle_tiling")
            wait_for(lambda: all(w[2] for w in windows()) and
                     all(b[2:] != (320, 240) for b in boxes() + boxes(2)), processes, "tiled again")
            # Off and on within one dispatch: no client has committed its floating size yet.
            burst = [socket.socket(socket.AF_UNIX) for _ in range(2)]
            for connection in burst:
                connection.connect(env["SHAODE_SOCKET"])
            for connection in burst:
                connection.sendall(b"toggle_tiling\n")
            for connection in burst:
                connection.recv(64)
                connection.close()
            msg("toggle_tiling")
            wait_for(lambda: not any(w[2] for w in windows()) and
                     boxes() + boxes(2) == floating, processes,
                     "floating geometry kept across rapid toggles")

            subscriber.close()
            msg("workspace", "2")  # Notifying a closed subscriber must not hurt the server.
            for window in processes[1:]:
                window.kill()
                window.wait(timeout=5)
            del processes[1:]
            server.terminate()
            assert server.wait(timeout=5) == 0, log.read_text()
            print("Tiling toggle, dwindle splits, workspaces, floating, and subscription passed")
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
