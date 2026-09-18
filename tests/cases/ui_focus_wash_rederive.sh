#!/bin/bash
# SPDX-License-Identifier: MIT
# A configuration change during a session re-derives the display. The terminal
# answers while the session starts and stops when the user types, as a
# terminal does whose later reply the live display reads. The prompt keeps the
# wash it had: the variant of a conversation already rendered does not change
# under it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

COLORTERM=truecolor FYAI_PTY_BACKGROUND=rgb:ffff/ffff/ffff \
FYAI_PTY_BACKGROUND_WHILE_STARTING=1 \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="/config set display/tool_preview_lines 6|/config set display/work_columns 2" \
FYAI_PTY_NEEDLE="work_columns" FYAI_PTY_TIMEOUT=25 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/out" \
    "$FYAI_BIN" -k test-key --color on --theme ember:auto \
    --set display/theme_ground=terminal \
    --set display/markdown=true -m mock-model -i \
    2>"$TEST_DIR/stderr" || true

if grep -a -q "invalid display.theme 'ember:\|needs a libfypalette" \
        "$TEST_DIR/out" "$TEST_DIR/stderr"; then
    skip "this build has no palette themes"
fi

"$PYTHON" - "$TEST_DIR/out" <<'PY' || \
    fail "the variant changed when the display was re-derived"
import re
import sys
from collections import Counter


def wash(chunk):
    """The ground of the most runs of @chunk: the prompt block stands on it."""
    found = Counter(re.findall(rb"\x1b\[48;2;(\d+);(\d+);(\d+)m", chunk))
    return found.most_common(1)[0][0] if found else None


data = open(sys.argv[1], "rb").read()
# The command re-derives the display. The wash of the prompt is the same
# before and after it: the variant of the session does not change under a
# conversation already rendered.
at = data.rfind(b"tool_preview_lines")
if at < 0:
    raise SystemExit("the command did not report")
before, after = wash(data[:at]), wash(data[at:])
if not before:
    raise SystemExit("no 24-bit ground was drawn")
if not after:
    raise SystemExit("no 24-bit ground was drawn after the re-derive")
if before != after:
    raise SystemExit("the wash went from %s to %s" % (before, after))
# The terminal is light, thus so is the wash of the theme.
r, g, b = (int(c) for c in after)
if 0.2126 * r + 0.7152 * g + 0.0722 * b < 200:
    raise SystemExit("the ground %s is not light" % (after,))
PY

pass
