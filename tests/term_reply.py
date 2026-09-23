# SPDX-License-Identifier: MIT
"""Terminal replies for a test driver that plays a terminal.

fyai asks the terminal what it supports before the UI opens, and ends its
queries with DA1. Every terminal answers DA1, and the probe waits for that
reply. A driver that does not answer DA1 makes every run wait for the probe
time limit.
"""
import os

DA1_QUERIES = (b"\x1b[c", b"\x1b[0c")
DA1_REPLY = b"\x1b[?62;22c"

# The end of the last read of each descriptor. A read can end inside a
# query: macOS returns at most about 1 KiB of a pty at a time.
_held = {}


def answer_da1(fd, chunk):
    """Answer each DA1 query in @chunk on @fd, also one split across reads."""
    held = _held.get(fd, b"")
    data = held + chunk
    for query in DA1_QUERIES:
        at = data.find(query)
        while at >= 0:
            # a query that ends in the held bytes was answered last time
            if at + len(query) > len(held):
                os.write(fd, DA1_REPLY)
            at = data.find(query, at + 1)
    keep = max(len(q) for q in DA1_QUERIES) - 1
    _held[fd] = data[-keep:]
