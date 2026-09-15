#!/bin/bash
# SPDX-License-Identifier: MIT
# The default display/focus_bg is the focus wash of the palette theme. What
# holds the keys stands on it on a light terminal and a dark one alike, and
# not on reverse video, which turns text of a colour into a ground of that
# colour.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

# A bang shell takes the keys as it opens, so the ground is asked for
# without any key having to be pressed.
run_on()
{
    name=$1
    background=$2
    fyai_test_setup
    COLORTERM=truecolor FYAI_PTY_BACKGROUND=$background \
    FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'printf WASHED; sleep 3'" \
    FYAI_PTY_NEEDLE="WASHED" FYAI_PTY_TIMEOUT=25 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-] returns to the prompt|raw:1d|wait-gone:Ctrl-] returns to the prompt" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/$name.out" \
        "$FYAI_BIN" -k test-key --color on --theme ember:auto \
        --set display/markdown=true -m mock-model -i \
        2>"$TEST_DIR/stderr" || true
    if grep -a -q "invalid display.theme 'ember:\|needs a libfypalette" \
            "$TEST_DIR/$name.out" "$TEST_DIR/stderr"; then
        skip "this build has no palette themes"
    fi
    cp "$TEST_DIR/$name.out" "$CAPTURES/$name.out"
}

run_on light rgb:ffff/ffff/ffff
run_on dark rgb:1e1e/1e1e/2e2e

"$PYTHON" - "$CAPTURES/light.out" "$CAPTURES/dark.out" <<'PY' || \
    fail "what holds the keys does not stand on the focus wash"
import re
import sys
from collections import Counter


def grounds(path):
    data = open(path, "rb").read()
    found = Counter(re.findall(rb"\x1b\[48;2;(\d+);(\d+);(\d+)m", data))
    return found, data


def luma(rgb):
    r, g, b = (int(c) for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


problems = []
for path, light in ((sys.argv[1], True), (sys.argv[2], False)):
    found, data = grounds(path)
    if not found:
        problems.append("%s: no 24-bit ground was drawn" % path)
        continue
    # The wash is the ground of the whole focused tile and of the prompt, so
    # it is the ground of many runs. A card of the transcript is a few.
    wash, runs = found.most_common(1)[0]
    if runs < 20:
        problems.append("%s: no ground covers what holds the keys (%s: %d)"
                        % (path, wash, runs))
    if light and luma(wash) < 200:
        problems.append("%s: the ground %s is not light" % (path, wash))
    if not light and luma(wash) > 60:
        problems.append("%s: the ground %s is not dark" % (path, wash))
    # Reverse video is the ground without a wash.
    reversed_runs = data.count(b"\x1b[7m")
    if reversed_runs >= 10:
        problems.append("%s: focus is drawn in reverse video (%d runs)"
                        % (path, reversed_runs))
if problems:
    raise SystemExit("\n".join(problems))
PY

pass
