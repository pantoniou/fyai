#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wait for a process that is not a child to exit.

Usage: wait_exit.py PID DEADLINE_SECONDS

The wait is on the kernel: a pidfd is readable when the process has exited
(a zombie included), and a kqueue reports NOTE_EXIT. The deadline bounds a
failure and paces nothing. Exit status: 0 when the process has exited or
never existed, 1 when the deadline passed.
"""
import os
import select
import sys


def wait_pidfd(pid, deadline):
    try:
        fd = os.pidfd_open(pid)
    except ProcessLookupError:
        return True
    try:
        return bool(select.select([fd], [], [], deadline)[0])
    finally:
        os.close(fd)


def wait_kqueue(pid, deadline):
    kq = select.kqueue()
    event = select.kevent(pid, select.KQ_FILTER_PROC,
                          select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                          select.KQ_NOTE_EXIT)
    try:
        return bool(kq.control([event], 1, deadline))
    except ProcessLookupError:
        return True
    finally:
        kq.close()


def main():
    pid = int(sys.argv[1])
    deadline = float(sys.argv[2])
    if hasattr(os, "pidfd_open"):
        done = wait_pidfd(pid, deadline)
    else:
        done = wait_kqueue(pid, deadline)
    sys.exit(0 if done else 1)


main()
