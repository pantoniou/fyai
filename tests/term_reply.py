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


def answer_da1(fd, chunk):
    """Answer each DA1 query in @chunk on @fd."""
    for query in DA1_QUERIES:
        for _ in range(chunk.count(query)):
            os.write(fd, DA1_REPLY)
