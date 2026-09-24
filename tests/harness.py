"""Helpers shared by the integration tests."""
import time


def wait_for(predicate, processes, message, timeout=5, detail=None):
    """Polls `predicate` until it holds, failing if a process exits or time runs out."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for process in processes:
            assert process.poll() is None, f"process exited ({process.returncode}): {message}"
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(f"timed out: {message}" + (f"; {detail()}" if detail else ""))
