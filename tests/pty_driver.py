#!/usr/bin/env python3
"""Drive one fyai interactive session through a real PTY."""
import fcntl
import os
import select
import struct
import sys
import termios
import time


def read_until(fd, data, needle, deadline):
    while needle not in data and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    if needle not in data:
        raise RuntimeError("PTY output never contained %r; tail=%r" %
                           (needle, data[-2000:]))
    return data


def read_until_count(fd, data, needle, count, deadline):
    while data.count(needle) < count and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    if data.count(needle) < count:
        raise RuntimeError(
            "PTY output did not repeat %r; tail=%r" %
            (needle, data[-2000:]))
    return data


def timeout_scale():
    """How much longer every deadline waits.

    Deadlines only: a value that paces the driver, such as how long it waits
    before it types, is a step of the case and not a budget for one.

    A deadline is a guess about a machine. A sanitized build, or a suite run
    with many jobs at once, takes several times as long for the same work, and
    a case that waits for output must wait that much longer rather than report
    a failure that says nothing about the code. $FYAI_TIMEOUT_SCALE carries it,
    and CMake sets $FYAI_TIMEOUT_SCALE_DEFAULT for the sanitized tree.
    """
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
    prompt = os.environ.get("FYAI_PTY_INPUT", "hello").encode()
    needle = os.environ.get(
        "FYAI_PTY_NEEDLE", "Streaming hello from the mock.").encode()
    progress_needle = os.environ.get("FYAI_PTY_PROGRESS_NEEDLE", "").encode()
    progress_timeout = float(
        os.environ.get("FYAI_PTY_PROGRESS_TIMEOUT", "1.5")) * scale
    mid_needle = os.environ.get("FYAI_PTY_MID_NEEDLE", "").encode()
    mid_timeout = float(os.environ.get("FYAI_PTY_MID_TIMEOUT", "3")) * scale
    interrupt_after_progress = os.environ.get(
        "FYAI_PTY_INTERRUPT_AFTER_PROGRESS", "0") in ("1", "true", "yes")
    interrupt_key = os.environ.get("FYAI_PTY_INTERRUPT_KEY", "escape")
    during_input = os.environ.get("FYAI_PTY_DURING_INPUT", "").encode()
    during_delay = float(os.environ.get("FYAI_PTY_DURING_DELAY", "0.2"))
    during_submit = os.environ.get(
        "FYAI_PTY_DURING_SUBMIT", "1") not in ("0", "false", "no")
    interrupt_after_during = os.environ.get(
        "FYAI_PTY_INTERRUPT_AFTER_DURING", "0") in ("1", "true", "yes")
    interrupt_delay = float(
        os.environ.get("FYAI_PTY_INTERRUPT_DELAY", "0.2"))
    submit_recalled = os.environ.get(
        "FYAI_PTY_SUBMIT_RECALLED", "0") in ("1", "true", "yes")
    clear_before_exit = os.environ.get(
        "FYAI_PTY_CLEAR_BEFORE_EXIT", "0") in ("1", "true", "yes")
    resize_cols = int(os.environ.get("FYAI_PTY_RESIZE_COLS", "0"))
    edit_input = os.environ.get(
        "FYAI_PTY_EDIT_INPUT", "0") in ("1", "true", "yes")
    edit_needle = os.environ.get(
        "FYAI_PTY_EDIT_NEEDLE", "edited prompt").encode()
    rows = int(os.environ.get("FYAI_PTY_ROWS", "30"))
    cols = int(os.environ.get("FYAI_PTY_COLS", "100"))
    session_timeout = float(os.environ.get("FYAI_PTY_TIMEOUT", "15")) * scale
    # Steps taken after the turn has answered and before the session is left,
    # which is where anything the USER does to a still-running program
    # belongs: the turn is over, the prompt is back, and the program is still
    # on screen. Separated by "|":
    #
    #   send:TEXT   type TEXT and submit it
    #   raw:HEX     write these bytes and submit nothing (a control key)
    #   wait:TEXT   read until TEXT is on the capture
    #
    # A wait is a deadline, so it is scaled; a pause is a step of the case
    # and is not (see CLAUDE.md on FYAI_TIMEOUT_SCALE).
    after_script = [step for step in
                    os.environ.get("FYAI_PTY_AFTER", "").split("|") if step]
    after_timeout = float(os.environ.get("FYAI_PTY_AFTER_TIMEOUT", "5")) * scale
    after_pause = float(os.environ.get("FYAI_PTY_AFTER_PAUSE", "0.3"))
    master, child = os.openpty()
    fcntl.ioctl(child, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        # Create a controlling terminal with a foreground process group.
        os.setsid()
        fcntl.ioctl(child, termios.TIOCSCTTY, 0)
        os.dup2(child, 0)
        os.dup2(child, 1)
        os.dup2(child, 2)
        os.close(master)
        os.environ.setdefault("TERM", "xterm-256color")
        os.execv(argv[0], argv)
    os.close(child)
    data = b""
    deadline = time.monotonic() + session_timeout
    try:
        # Wait until the initial synchronized update has made the input cursor
        # visible. Fixed sleeps race ASAN and slower CI runners, causing input
        # to be echoed by the tty before fytimui enters raw mode.
        data = read_until(master, data, b"\x1b[?25h", deadline)
        if edit_input:
            os.write(master, prompt + b"\x07")
            data = read_until(master, data, edit_needle,
                              time.monotonic() + progress_timeout)
            os.write(master, b"\n")
        else:
            os.write(master, prompt + b"\n")
        if during_input:
            time.sleep(during_delay)
            os.write(master, during_input + (b"\n" if during_submit else b""))
        if interrupt_after_during:
            time.sleep(interrupt_delay)
            data = read_until(master, data, during_input,
                              time.monotonic() + progress_timeout)
            occurrences = data.count(during_input)
            os.write(master, b"\x1b\x1b")
            data = read_until_count(master, data, during_input,
                                    occurrences + 1,
                                    time.monotonic() + progress_timeout)
            if submit_recalled:
                os.write(master, b"\n")
        if progress_needle:
            data = read_until(master, data, progress_needle,
                              time.monotonic() + progress_timeout)
            if interrupt_after_progress:
                os.write(master, b"\x03" if interrupt_key == "ctrl-c"
                         else b"\x1b")
            if resize_cols:
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 30, resize_cols, 0, 0))
        if mid_needle:
            data = read_until(master, data, mid_needle,
                              time.monotonic() + mid_timeout)
        data = read_until(master, data, needle, deadline)
        for step in after_script:
            kind, _, value = step.partition(":")
            if kind == "send":
                os.write(master, value.encode() + b"\n")
                time.sleep(after_pause)
            elif kind == "raw":
                os.write(master, bytes.fromhex(value))
                time.sleep(after_pause)
            elif kind == "wait":
                data = read_until(master, data, value.encode(),
                                  time.monotonic() + after_timeout)
            else:
                raise RuntimeError("unknown FYAI_PTY_AFTER step: %r" % step)
        if clear_before_exit:
            os.write(master, b"\x15")
        os.write(master, b"/exit\n")
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                data += chunk
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
                    raise RuntimeError("fyai exited unsuccessfully")
                break
        else:
            raise RuntimeError("fyai did not exit")
    finally:
        with open(output, "wb") as fp:
            fp.write(data)
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        os.close(master)


if __name__ == "__main__":
    main()
