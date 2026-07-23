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


def main():
    output, *argv = sys.argv[1:]
    prompt = os.environ.get("FYAI_PTY_INPUT", "hello").encode()
    needle = os.environ.get(
        "FYAI_PTY_NEEDLE", "Streaming hello from the mock.").encode()
    progress_needle = os.environ.get("FYAI_PTY_PROGRESS_NEEDLE", "").encode()
    progress_timeout = float(
        os.environ.get("FYAI_PTY_PROGRESS_TIMEOUT", "1.5"))
    master, child = os.openpty()
    fcntl.ioctl(child, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.dup2(child, 0)
        os.dup2(child, 1)
        os.dup2(child, 2)
        os.close(master)
        os.environ.setdefault("TERM", "xterm-256color")
        os.execv(argv[0], argv)
    os.close(child)
    data = b""
    deadline = time.monotonic() + 15
    try:
        time.sleep(0.2)
        os.write(master, prompt + b"\n")
        if progress_needle:
            data = read_until(master, data, progress_needle,
                              time.monotonic() + progress_timeout)
        data = read_until(master, data, needle, deadline)
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
