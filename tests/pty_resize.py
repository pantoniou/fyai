#!/usr/bin/env python3
"""Run one command on a PTY, resize the PTY, and keep the capture.

The first argument is the capture file, the rest is the command. The PTY starts
at FYAI_PTY_ROWS x FYAI_PTY_COLS and changes to FYAI_PTY_ROWS2 x FYAI_PTY_COLS2
once FYAI_PTY_NEEDLE appears, or after FYAI_PTY_DELAY seconds when no needle is
given.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def timeout_scale():
    """How much longer every deadline waits; see tests/pty_driver.py."""
    try:
        scale = float(os.environ.get(
            "FYAI_TIMEOUT_SCALE",
            os.environ.get("FYAI_TIMEOUT_SCALE_DEFAULT", "1")))
    except ValueError:
        return 1.0
    return scale if scale > 0 else 1.0


def main():
    output, *argv = sys.argv[1:]
    scale = timeout_scale()
    rows = int(os.environ.get("FYAI_PTY_ROWS", "24"))
    cols = int(os.environ.get("FYAI_PTY_COLS", "80"))
    rows2 = int(os.environ.get("FYAI_PTY_ROWS2", "30"))
    cols2 = int(os.environ.get("FYAI_PTY_COLS2", "100"))
    needle = os.environ.get("FYAI_PTY_NEEDLE", "").encode()
    delay = float(os.environ.get("FYAI_PTY_DELAY", "1.5"))
    timeout = float(os.environ.get("FYAI_PTY_TIMEOUT", "30")) * scale

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv)

    set_size(fd, rows, cols)
    data = b""
    resized = False
    deadline = time.monotonic() + timeout
    started = time.monotonic()
    while time.monotonic() < deadline:
        if not resized:
            ready = (needle and needle in data) or \
                (not needle and time.monotonic() - started > delay)
            if ready:
                set_size(fd, rows2, cols2)
                # The child leads its own session; the kernel resizes only the
                # PTY, thus the process that watches it is told directly.
                os.kill(pid, signal.SIGWINCH)
                resized = True
        readable, _, _ = select.select([fd], [], [], 0.2)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk

    os.close(fd)
    _, status = os.waitpid(pid, 0)
    with open(output, "wb") as f:
        f.write(data)
    if not resized:
        sys.stderr.write("the PTY was never resized\n")
        return 1
    return 0 if os.WIFEXITED(status) and not os.WEXITSTATUS(status) else 1


if __name__ == "__main__":
    sys.exit(main())
