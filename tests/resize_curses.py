#!/usr/bin/env python3
"""Deterministic ncurses screen used to test incremental resize repaints."""

import curses
import os
import signal


resized = False


def winch(_signum, _frame):
    global resized
    resized = True


def paint(screen, generation):
    rows, cols = screen.getmaxyx()
    for row in range(rows):
        if row == 0:
            text = "SIZE %dx%d GEN %d" % (rows, cols, generation)
        elif row == 1:
            token = "W%dG%d:" % (cols, generation)
            text = token + "#" * (cols - len(token))
        elif row == rows - 1:
            token = "E%dG%d:" % (row, generation)
            text = token + chr(ord("A") + row % 26) * (cols - len(token))
        else:
            token = "R%02d:" % row
            text = token + chr(ord("A") + row % 26) * (cols - len(token))
        # Leave the bottom-right cell blank to avoid the ncurses wrap error.
        width = cols - (1 if row == rows - 1 else 0)
        try:
            screen.addnstr(row, 0, text, width)
        except curses.error:
            pass
    screen.noutrefresh()
    curses.doupdate()


def run(screen):
    global resized
    generation = 1
    signal.signal(signal.SIGWINCH, winch)
    curses.curs_set(0)
    screen.keypad(True)
    screen.timeout(1000)
    paint(screen, generation)
    last_size = os.get_terminal_size(1)
    while True:
        key = screen.getch()
        if key in (ord("q"), 3):
            return
        size = os.get_terminal_size(1)
        if resized or size != last_size:
            resized = False
            last_size = size
            curses.resizeterm(size.lines, size.columns)
            generation += 1
            paint(screen, generation)


if __name__ == "__main__":
    curses.wrapper(run)
