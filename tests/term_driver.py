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


def timeout_scale():
    """How much longer every deadline waits; see tests/pty_driver.py."""
    try:
        scale = float(os.environ.get(
            "FYAI_TIMEOUT_SCALE",
            os.environ.get("FYAI_TIMEOUT_SCALE_DEFAULT", "1")))
    except ValueError:
        return 1.0
    return scale if scale > 0 else 1.0


def unescape(text):
    return text.encode("utf-8").decode("unicode_escape").encode("latin1")


def drain(fd, sink, deadline, stop=None, tick=None):
    """Read from the terminal until @stop is true or @deadline passes.

    A driver that stops reading fills the pseudo-terminal buffer. The
    program under test then blocks in its write until the driver reads
    again. The buffer is 1 KiB on macOS and 12 KiB on Linux, thus a wait
    that does not read stops the program for the length of the wait. Every
    wait in this driver reads.

    The bytes go into @sink, which the caller keeps: a run that fails must
    still leave its capture. Returns True if @stop became true.
    """
    while True:
        if stop and stop():
            return True
        if tick:
            tick()
        if time.monotonic() >= deadline:
            return False
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        sink[0] += chunk
    return bool(stop and stop())


def read_until(fd, sink, needle, deadline):
    """Read until @needle appears."""
    if not drain(fd, sink, deadline, stop=lambda: needle in sink[0]):
        raise RuntimeError("the terminal never painted %r; tail=%r" %
                           (needle, sink[0][-2000:]))


def main():
    output, *argv = sys.argv[1:]
    scale = timeout_scale()
    rows = int(os.environ.get("FYAI_TERM_ROWS", "24"))
    cols = int(os.environ.get("FYAI_TERM_COLS", "80"))
    wait = os.environ.get("FYAI_TERM_WAIT", "").encode()
    send = os.environ.get("FYAI_TERM_SEND", "")
    wait2 = os.environ.get("FYAI_TERM_WAIT2", "").encode()
    send2 = os.environ.get("FYAI_TERM_SEND2", "")
    send_gap = float(os.environ.get("FYAI_TERM_SEND_GAP", "0.3"))
    resize = os.environ.get("FYAI_TERM_RESIZE", "")
    resize_delay = float(os.environ.get("FYAI_TERM_RESIZE_DELAY", "0"))
    timeout = float(os.environ.get("FYAI_TERM_TIMEOUT", "20")) * scale

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
    resize_dims = None
    # Bytes before the resize belong to the old size: reports the
    # program made before it must not satisfy the wait after it.
    wait2_from = 0
    try:
        if wait:
            read_until(master, sink, wait, deadline)
        if resize:
            if resize_delay:
                drain(master, sink,
                      min(deadline, time.monotonic() + resize_delay))
            r, c = (int(v) for v in resize.split("x"))
            fcntl.ioctl(master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", r, c, 0, 0))
            resize_dims = (r, c)
            wait2_from = len(sink[0])
            # Let the verb forward the new grid to the child before any
            # quit key tears it down. Without this a slow runner processes
            # the resize only after the program has exited, and the size
            # report the case greps for never reaches the capture.
            drain(master, sink, min(deadline, time.monotonic() + 1.0))
        if send:
            os.write(master, unescape(send))
        if send2:
            # A second keystroke, after a gap: a chord a person types is two
            # keys, and the library reads a frame at a time.
            drain(master, sink, min(deadline, time.monotonic() + send_gap))
            os.write(master, unescape(send2))
        if wait2:
            # Re-assert the size while waiting: a resize the verb missed
            # (slow or heavily loaded runner) is delivered again instead of
            # failing the case on one lost SIGWINCH.
            reassert = [time.monotonic()]

            def reassert_size():
                if not resize_dims:
                    return
                if time.monotonic() - reassert[0] < 1.0:
                    return
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", resize_dims[0],
                                        resize_dims[1], 0, 0))
                reassert[0] = time.monotonic()

            if not drain(master, sink, deadline,
                         stop=lambda: wait2 in sink[0][wait2_from:],
                         tick=reassert_size):
                raise RuntimeError("the terminal never painted %r; tail=%r" %
                                   (wait2, sink[0][-2000:]))
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
        if os.WIFEXITED(status) and not os.WEXITSTATUS(status):
            pass
        elif os.WIFSIGNALED(status):
            raise RuntimeError(
                "fyai term died from signal %d" % os.WTERMSIG(status))
        else:
            raise RuntimeError(
                "fyai term exited with status %d: %r" %
                (os.WEXITSTATUS(status) if os.WIFEXITED(status)
                 else status, status))
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
