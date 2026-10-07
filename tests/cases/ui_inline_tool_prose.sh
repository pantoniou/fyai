#!/bin/bash
# SPDX-License-Identifier: MIT
# Prose that streams before a tool call stays on the screen, once, while the
# call runs: the rows the renderer froze join the live rows when the prose
# ends, and no frame draws a paragraph twice. The page is
# $FYAI_INLINE_SCREEN, fullscreen by default, with the inline tool display.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start inline_tool_prose.json

FYAI_PTY_INPUT="run foo" FYAI_PTY_NEEDLE="run foo" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-row:line-3|release:$TMPDIR/dup-release|"\
"wait-screen:All done." \
FYAI_PTY_AFTER_PAUSE=0 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set "display/screen=${FYAI_INLINE_SCREEN:-fullscreen}" \
    --set display/tool_display=inline \
    --set tools=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' || fail "the prose before the call is wrong"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
running = 0
end = data.find(b"\x1b[?2026l")
while end >= 0:
    s = Screen(30, 100)
    s.feed(data[:end + 8])
    rows = [r.strip() for r in s.display()]
    for text in ("Executing foo now.", "Second paragraph of prose."):
        if rows.count(text) > 1:
            raise SystemExit("a frame draws %r twice:\n%s" %
                             (text, "\n".join(rows)))
    # While the call runs, the prose stands above it.
    if "line-3" in rows and "All done." not in rows:
        running += 1
        if "Executing foo now." not in rows:
            raise SystemExit("the prose left the screen during the call:\n" +
                             "\n".join(rows))
    end = data.find(b"\x1b[?2026l", end + 1)
if not running:
    raise SystemExit("no frame shows the running call")
PY

mock_stop 2
pass
