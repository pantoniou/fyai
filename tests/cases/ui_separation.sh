#!/bin/bash
# SPDX-License-Identifier: MIT
# The separation a live session draws, measured on the terminal cells. A fenced
# card supplies a blank row at each end, thus the manager adds none. A call is
# fenced from the call above it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start display_parity.json

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

FYAI_PTY_ROWS=70 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="do things" FYAI_PTY_NEEDLE="Done." FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "the live separation is wrong"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

rows = [r.rstrip() for r in list(frames(open(sys.argv[1], "rb").read(), 70, 100))[-1]]


def index(prefix):
    for i, r in enumerate(rows):
        if r.startswith(prefix):
            return i
    raise SystemExit("row %r is not on the screen" % prefix)


def blanks_before(i):
    n = 0
    while i - 1 - n >= 0 and not rows[i - 1 - n]:
        n += 1
    return n


# A fenced card supplies the row under its words.
card = index("  │ do things")
first = index("● read hello.c")
got = blanks_before(first)
if got != 1:
    raise SystemExit("%d blank rows stand under the card, want 1" % got)
if first <= card:
    raise SystemExit("the answer is above the card")

# A call is fenced from the call above it.
for prefix in ("● write note.txt", "● update hello.c", "● shell [say things]"):
    got = blanks_before(index(prefix))
    if got != 1:
        raise SystemExit("%d blank rows stand over %r, want 1" % (got, prefix))

# The prose after a call stands off it.
got = blanks_before(index("  Prose with a code block:"))
if got != 1:
    raise SystemExit("%d blank rows stand over the prose, want 1" % got)

# turn_separator belongs to the transcript view. A live session draws no rule.
for r in rows[card + 1:first]:
    if r.strip().startswith("──"):
        raise SystemExit("a live session drew a turn rule: %r" % r)
PY

mock_stop 5
pass
