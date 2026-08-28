#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assert completed work does not remain beside a later status pane."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from screen import Screen

if len(sys.argv) < 3:
    raise SystemExit("usage: assert_work_retired.py CAPTURE HEADING...")

screen = Screen(30, 100)
screen.feed(open(sys.argv[1], "rb").read())
lines = screen.lines()
status = [i for i, line in enumerate(lines) if "● status" in line]
if not status:
    raise SystemExit("the post-completion status pane was not shown")
start = status[-1]
for heading in sys.argv[2:]:
    stale = [line for line in lines[start:] if heading in line]
    if stale:
        raise SystemExit("completed work remained after status: %r" % stale)
