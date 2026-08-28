#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inspect complete synchronized-output frames in a PTY capture."""

FRAME_END = b"\x1b[?2026l"


def frames(data, rows, cols):
    """Every completed frame, as a list of display rows."""
    import os
    import sys

    sys.path.insert(0, os.environ["TESTS_DIR"])
    from screen import Screen

    screen = Screen(rows, cols)
    out = []
    at = 0
    while True:
        end = data.find(FRAME_END, at)
        if end < 0:
            break
        end += len(FRAME_END)
        screen.feed(data[at:end])
        out.append(screen.display())
        at = end
    return out


def frames_with_both(data, needle_a, needle_b, rows, cols):
    """The frames showing both needles, as (row_a, row_b, line_a) triples."""
    seen = []
    for disp in frames(data, rows, cols):
        ra = next((i for i, r in enumerate(disp) if needle_a in r), -1)
        rb = next((i for i, r in enumerate(disp) if needle_b in r), -1)
        if ra >= 0 and rb >= 0:
            seen.append((ra, rb, disp[ra]))
    return seen
