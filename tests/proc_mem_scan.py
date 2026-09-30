#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Search the writable memory of a process for a string.

usage: proc_mem_scan.py PID VARIABLE

The string is the value of the environment variable VARIABLE, so that it is
not in an argument list. Exit 0 when the string is not in the process, 1 when
it is, 2 when the memory cannot be read (a kernel that does not let a
descendant read it).
"""
import os
import re
import sys

CHUNK = 1 << 20
# A sanitizer maps shadow memory of terabytes. Skip a region that large: a
# credential is not held in one.
MAX_REGION = 1 << 30
# A credential is in private anonymous memory, the heap or the stack. A shared
# or file-backed mapping, such as the arena, is not scanned: reading it costs
# input and output and finds nothing of the process.
ANON = ("", "[heap]", "[stack]")


PAGE = 4096


def resident_runs(pagemap, lo, hi):
    """Yield (start, end) of the runs of pages in [lo, hi) that hold data.

    A region can be large and sparse: reading its untouched pages one by one
    costs seconds and finds nothing. The page map says which pages exist.
    """
    pages = (hi - lo) // PAGE
    try:
        pagemap.seek(lo // PAGE * 8)
        raw = pagemap.read(pages * 8)
    except (OSError, OverflowError, ValueError):
        return
    start = None
    for i in range(len(raw) // 8):
        entry = int.from_bytes(raw[i * 8:i * 8 + 8], "little")
        # Bit 63: present in memory. Bit 62: in swap.
        if entry >> 62:
            if start is None:
                start = i
        elif start is not None:
            yield lo + start * PAGE, lo + i * PAGE
            start = None
    if start is not None:
        yield lo + start * PAGE, lo + (len(raw) // 8) * PAGE


def main():
    pid = sys.argv[1]
    needle = (os.environ.get(sys.argv[2]) if len(sys.argv) > 2 else "").encode()
    if not needle:
        return 2
    try:
        maps = open("/proc/%s/maps" % pid).read().splitlines()
        mem = open("/proc/%s/mem" % pid, "rb", 0)
        pagemap = open("/proc/%s/pagemap" % pid, "rb", 0)
    except OSError:
        return 2
    tail = len(needle) - 1
    readable = False
    for line in maps:
        m = re.match(r"([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$", line)
        if not m or m.group(3)[0:2] != "rw" or m.group(3)[3:4] != "p":
            continue
        if m.group(4) not in ANON:
            continue
        lo, hi = int(m.group(1), 16), int(m.group(2), 16)
        if hi - lo > MAX_REGION:
            continue
        for rlo, rhi in resident_runs(pagemap, lo, hi):
            pos, keep = rlo, b""
            while pos < rhi:
                try:
                    mem.seek(pos)
                    data = mem.read(min(CHUNK, rhi - pos))
                except (OSError, OverflowError, ValueError):
                    break
                if not data:
                    break
                readable = True
                if needle in keep + data:
                    return 1
                keep = (keep + data)[-tail:] if tail else b""
                pos += len(data)
    return 0 if readable else 2


if __name__ == "__main__":
    sys.exit(main())
