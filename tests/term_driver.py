#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive one `fyai term` run through a real PTY.

The verb only draws when it owns a terminal, thus a case gives it one, types
into it, and keeps every byte it painted. The capture is the picture the user
saw; tests/screen.py reads it back.
"""
import fcntl
import os
import select
import struct
import sys
import termios
import time


def unescape(text):
    return text.encode("utf-8").decode("unicode_escape").encode("latin1")


def read_until(fd, sink, needle, deadline):
    """Read until @needle appears. The bytes go into @sink, which the caller
    keeps: a run that never sees the needle must still leave its capture."""
    while needle not in sink[0] and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        sink[0] += chunk
    if needle not in sink[0]:
        raise RuntimeError("the terminal never painted %r; tail=%r" %
                           (needle, sink[0][-2000:]))


def main():
    output, *argv = sys.argv[1:]
    rows = int(os.environ.get("FYAI_TERM_ROWS", "24"))
    cols = int(os.environ.get("FYAI_TERM_COLS", "80"))
    wait = os.environ.get("FYAI_TERM_WAIT", "").encode()
    send = os.environ.get("FYAI_TERM_SEND", "")
    wait2 = os.environ.get("FYAI_TERM_WAIT2", "").encode()
    send2 = os.environ.get("FYAI_TERM_SEND2", "")
    send_gap = float(os.environ.get("FYAI_TERM_SEND_GAP", "0.3"))
    resize = os.environ.get("FYAI_TERM_RESIZE", "")
    resize_delay = float(os.environ.get("FYAI_TERM_RESIZE_DELAY", "0"))
    timeout = float(os.environ.get("FYAI_TERM_TIMEOUT", "20"))

    master, child = os.openpty()
    fcntl.ioctl(child, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(child, termios.TIOCSCTTY, 0)
        os.dup2(child, 0)
        os.dup2(child, 1)
        os.dup2(child, 2)
        os.close(master)
        os.environ.setdefault("TERM", "xterm-256color")
        os.execv(argv[0], argv)
    os.close(child)

    sink = [b""]
    deadline = time.monotonic() + timeout
    status = None
    try:
        if wait:
            read_until(master, sink, wait, deadline)
        if resize:
            if resize_delay:
                time.sleep(resize_delay)
            r, c = (int(v) for v in resize.split("x"))
            fcntl.ioctl(master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", r, c, 0, 0))
        if send:
            os.write(master, unescape(send))
        if send2:
            # A second keystroke, after a gap: a chord a person types is two
            # keys, and the library reads a frame at a time.
            time.sleep(send_gap)
            os.write(master, unescape(send2))
        if wait2:
            read_until(master, sink, wait2, deadline)
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    chunk = b""
                sink[0] += chunk
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
                break
        if status is None:
            raise RuntimeError("fyai term did not exit")
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
            raise RuntimeError("fyai term exited unsuccessfully: %r" % status)
    finally:
        with open(output, "wb") as fp:
            fp.write(sink[0])
        if status is None:
            try:
                os.kill(pid, 9)
                os.waitpid(pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
        os.close(master)


if __name__ == "__main__":
    main()
