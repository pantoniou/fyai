#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A terminal screen model for PTY captures.

Enough of the terminal to answer "what did the user see": the rows the band
paints are moved into with relative cursor motion, not newlines, so a byte
capture cannot show them. Handles the sequences the UI emits - cursor motion,
erase, and scrolling - and ignores the rest (SGR, mode switches, OSC).
"""

import re
import sys

CSI = re.compile(rb"\x1b\[([0-9;?]*)([@-~])")


class Screen:
    def __init__(self, rows=30, cols=100):
        self.rows = rows
        self.cols = cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.row = 0
        self.col = 0
        # Rows that left the screen. A transcript is longer than the screen,
        # so the order of what the user saw needs them.
        self.scrollback = []

    def display(self):
        return ["".join(r).rstrip() for r in self.grid]

    def lines(self):
        """Every row in the order it was shown, scrolled-off rows first."""
        return self.scrollback + self.display()

    def _scroll(self):
        self.scrollback.append("".join(self.grid[0]).rstrip())
        self.grid.pop(0)
        self.grid.append([" "] * self.cols)

    def _put(self, ch):
        if self.col >= self.cols:
            return
        self.grid[self.row][self.col] = ch
        self.col += 1

    def feed(self, data):
        i = 0
        while i < len(data):
            b = data[i:i + 1]
            if b == b"\x1b":
                m = CSI.match(data, i)
                if m:
                    self._csi(m.group(1), m.group(2))
                    i = m.end()
                    continue
                if data[i:i + 2] in (b"\x1b]", b"\x1bP"):   # OSC / DCS
                    end = data.find(b"\x07", i)
                    esc = data.find(b"\x1b\\", i)
                    if esc >= 0 and (end < 0 or esc < end):
                        end = esc + 1
                    i = (end + 1) if end >= 0 else len(data)
                    continue
                i += 2                                      # other escape
                continue
            if b == b"\r":
                self.col = 0
            elif b == b"\n":
                if self.row + 1 >= self.rows:
                    self._scroll()
                else:
                    self.row += 1
            elif b == b"\t":
                self.col = min(self.cols - 1, (self.col // 8 + 1) * 8)
            elif b >= b" ":
                ch = data[i:i + 1]
                length = 1
                if data[i] >= 0xF0:
                    length = 4
                elif data[i] >= 0xE0:
                    length = 3
                elif data[i] >= 0xC0:
                    length = 2
                ch = data[i:i + length]
                self._put(ch.decode("utf-8", "replace"))
                i += length
                continue
            i += 1

    def _csi(self, params, final):
        args = [int(p) for p in params.split(b";") if p.isdigit()]
        n = args[0] if args else 1
        if final == b"A":
            self.row = max(0, self.row - max(1, n))
        elif final == b"B":
            self.row = min(self.rows - 1, self.row + max(1, n))
        elif final == b"C":
            self.col = min(self.cols - 1, self.col + max(1, n))
        elif final == b"D":
            self.col = max(0, self.col - max(1, n))
        elif final == b"G":
            self.col = max(0, (n if args else 1) - 1)
        elif final == b"H":
            self.row = max(0, (args[0] if args else 1) - 1)
            self.col = max(0, (args[1] if len(args) > 1 else 1) - 1)
        elif final == b"J" and (not args or args[0] == 0):
            for c in range(self.col, self.cols):
                self.grid[self.row][c] = " "
            for r in range(self.row + 1, self.rows):
                self.grid[r] = [" "] * self.cols
        elif final == b"K":
            mode = args[0] if args else 0
            lo = self.col if mode == 0 else 0
            hi = self.cols if mode in (0, 2) else self.col + 1
            for c in range(lo, hi):
                self.grid[self.row][c] = " "
        elif final == b"L":
            for _ in range(max(1, n)):
                self.grid.insert(self.row, [" "] * self.cols)
                self.grid.pop()
        elif final == b"M":
            for _ in range(max(1, n)):
                self.grid.pop(self.row)
                self.grid.append([" "] * self.cols)
        elif final == b"S":
            for _ in range(max(1, n)):
                self._scroll()


def rows_at(path, needle, rows=30, cols=100, extra=400):
    """The screen as it stood just after @needle first appeared."""
    data = open(path, "rb").read()
    at = data.find(needle)
    if at < 0:
        raise SystemExit("needle not in capture: %r" % needle)
    screen = Screen(rows, cols)
    screen.feed(data[:at + extra])
    return [r for r in screen.display() if r.strip()]


if __name__ == "__main__":
    for row in rows_at(sys.argv[1], sys.argv[2].encode()):
        print(row)
