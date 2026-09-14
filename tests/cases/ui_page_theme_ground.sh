#!/bin/bash
# SPDX-License-Identifier: MIT
# A fullscreen palette page fills default backgrounds with its theme ground.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

session()
{
    label=$1
    ground=$2
    variant=$3
    screen=$4
    fyai_test_setup
    mock_start chat_themed_markdown.json
    driver=0
    # The grounds are compared as RGB, which a 256-colour terminal cannot draw.
    COLORTERM=truecolor FYAI_PTY_BACKGROUND=rgb:1e1e/1e1e/2e2e \
    FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="hello" \
    FYAI_PTY_NEEDLE="Heading" FYAI_PTY_TIMEOUT=20 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --color on --set display/markdown=true \
        --set "display/theme=ember:$variant" --set "display/theme_ground=$ground" \
        --set display/renderer=page --set "display/screen=$screen" \
        --set display/stream=false --set api=chat-completions \
        --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i \
        2>"$TEST_DIR/stderr" || driver=$?
    mock_stop_quiet
    if grep -a -q "invalid display.theme 'ember:\|needs a libfypalette\|needs a libfytimui" \
            "$TEST_DIR/pty.out" "$TEST_DIR/stderr"; then
        skip "this build has no fullscreen palette support"
    fi
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$TEST_DIR/pty.out" >&2
        fail "the $label session did not show the answer"
    fi
    cp "$TEST_DIR/pty.out" "$CAPTURES/$label.out"
}

session dark theme dark fullscreen
session light theme light fullscreen
session terminal terminal dark fullscreen
session inline theme dark inline

"$PYTHON" - "$CAPTURES" "$TESTS_DIR" <<'EOF' ||
import pathlib
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

class Colors(Screen):
    def __init__(self):
        super().__init__(30, 100)
        self.fg = self.bg = None
        self.backgrounds = [[None] * self.cols for _ in range(self.rows)]

    def _sgr(self, params):
        super()._sgr(params)
        codes = [int(p) if p.isdigit() else 0 for p in params.split(b";")]
        i = 0
        while i < len(codes):
            code = codes[i]
            if code in (38, 48, 58):
                mode = codes[i + 1] if i + 1 < len(codes) else 0
                count = 5 if mode == 2 else 3
                color = tuple(codes[i + 2:i + count])
                if code == 38:
                    self.fg = color
                elif code == 48:
                    self.bg = color
                i += count
                continue
            if code == 0:
                self.fg = self.bg = None
            elif code == 39:
                self.fg = None
            elif code == 49:
                self.bg = None
            elif 30 <= code <= 37 or 90 <= code <= 97:
                self.fg = (code,)
            elif 40 <= code <= 47 or 100 <= code <= 107:
                self.bg = (code,)
            i += 1

    def _put(self, ch):
        if self.col < self.cols:
            self.backgrounds[self.row][self.col] = self.fg if self.reverse else self.bg
        super()._put(ch)

    def _clear(self, row, lo, hi):
        super()._clear(row, lo, hi)
        for col in range(lo, hi):
            self.backgrounds[row][col] = self.bg

    def _scroll(self):
        super()._scroll()
        self.backgrounds.pop(0)
        self.backgrounds.append([self.bg] * self.cols)


def frame(label):
    data = (pathlib.Path(sys.argv[1]) / (label + ".out")).read_bytes()
    screen = Colors()
    end = b"\x1b[?2026l"
    pos = 0
    while True:
        at = data.find(end, pos)
        if at < 0:
            raise SystemExit(label + ": no frame with the complete answer")
        screen.feed(data[pos:at + len(end)])
        pos = at + len(end)
        rows = screen.display()
        heading = next((y for y, row in enumerate(rows) if "Heading" in row), None)
        code = next((y for y, row in enumerate(rows) if "int value" in row), None)
        if heading is not None and code is not None:
            return screen, rows, heading, code

for label in ("dark", "light"):
    screen, rows, heading, code = frame(label)
    col = rows[heading].index("Heading")
    ground = screen.backgrounds[heading][col]
    if ground is None or len(ground) != 3:
        raise SystemExit(label + ": ordinary text kept the terminal background")
    if not ground[0] >= ground[1] >= ground[2]:
        raise SystemExit(label + ": the page ground is not the warm theme ground")
    for y in (0, heading, heading + 1):
        for x in (0, screen.cols - 1):
            if screen.backgrounds[y][x] != ground:
                raise SystemExit(f"{label}: blank cell {y},{x} lost the page ground")
    raised = screen.backgrounds[code][rows[code].index("int value")]
    if raised is None or raised == ground:
        raise SystemExit(label + ": the code block lost its raised background")
    if (label == "dark" and max(raised) <= max(ground)) or \
            (label == "light" and min(raised) >= min(ground)):
        raise SystemExit(label + ": the code block is not raised over the page")

for label in ("terminal", "inline"):
    screen, rows, heading, code = frame(label)
    if screen.backgrounds[heading][rows[heading].index("Heading")] is not None:
        raise SystemExit(label + ": the page replaced the terminal background")
EOF
    fail "the fullscreen page did not keep its theme ground"

pass
