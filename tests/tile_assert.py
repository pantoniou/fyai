#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read a PTY capture one COMPLETE frame at a time.

A cut taken at an arbitrary byte is not a frame. The compositor writes only
the cells that changed, so a capture stopped in the middle of an update shows
half of one screen and half of the last, and a reading taken there says
nothing. It wraps every update in synchronized output (DEC 2026), which is
exactly the frame boundary: everything up to a `?2026l` is one finished
screen.

The tiling question also has to be asked of the frames where both screens
were still in the work pane. Whichever program finishes first is committed
into the transcript at a moment that moves with machine load, and after that
its screen is text below the pane rather than a tile in it, so a single cut
point is not a reading either.
"""

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
