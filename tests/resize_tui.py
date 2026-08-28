#!/usr/bin/env python3
"""Deterministic full-screen terminal used by PTY resize tests."""

import os
import select
import signal
import sys
import termios
import tty


dirty = True
generation = 0


def resized(_signum, _frame):
    global dirty
    dirty = True


def paint():
    global generation
    generation += 1
    size = os.get_terminal_size(sys.stdout.fileno())
    rows, cols = size.lines, size.columns
    label = (sys.argv[1] + " ") if len(sys.argv) > 1 else ""
    out = ["\x1b[?25l\x1b[?7l\x1b[H"]
    for row in range(1, rows):
        token = "R%02d:" % row
        text = token + chr(ord("A") + row % 26) * (cols - len(token))
        out.append("\x1b[%d;1H%s\x1b[K" % (row + 1, text[:cols]))
    # Write the size line last as the frame-completion marker.
    text = "%sSIZE %dx%d GEN %d" % (label, rows, cols, generation)
    out.append("\x1b[1;1H%s\x1b[K" % text[:cols])
    os.write(sys.stdout.fileno(), "".join(out).encode())


def main():
    global dirty
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    # Wake select() from the SIGWINCH handler through a pipe.
    wake_r, wake_w = os.pipe()
    os.set_blocking(wake_w, False)
    os.set_blocking(wake_r, False)
    signal.set_wakeup_fd(wake_w)
    signal.signal(signal.SIGWINCH, resized)
    try:
        tty.setraw(fd)
        os.write(sys.stdout.fileno(), b"\x1b[?1049h")
        last_size = None
        while True:
            # Repaint on a signal or an observed size change.
            size = os.get_terminal_size(sys.stdout.fileno())
            if dirty or size != last_size:
                dirty = False
                last_size = size
                paint()
            ready, _, _ = select.select([fd, wake_r], [], [], 0.1)
            if wake_r in ready:
                try:
                    os.read(wake_r, 64)
                except BlockingIOError:
                    pass
            if fd in ready and os.read(fd, 1) in (b"q", b"\x03"):
                break
    finally:
        signal.set_wakeup_fd(-1)
        os.close(wake_r)
        os.close(wake_w)
        os.write(sys.stdout.fileno(), b"\x1b[?7h\x1b[?25h\x1b[?1049l")
        termios.tcsetattr(fd, termios.TCSANOW, saved)


if __name__ == "__main__":
    main()
