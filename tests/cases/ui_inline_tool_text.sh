#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/tool_display=inline, a shell call without a terminal draws its
# live output in the transcript of the page ($FYAI_INLINE_SCREEN, fullscreen
# by default), under the card that asked for it, and not in a tile of the work
# pane. The block shows the last rows of the preview, and the stored call
# takes its place when it ends.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_band_invocation_held.json

FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_COLS=100 FYAI_PTY_INPUT="run it" \
FYAI_PTY_NEEDLE="run it" \
FYAI_PTY_AFTER="wait-screen:Building object 7|snapshot|release:$TMPDIR/band-release|wait-screen:Done." \
FYAI_PTY_AFTER_PAUSE=0 \
FYAI_PTY_SNAPSHOT="$TEST_DIR/live.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/tool_update_interval_ms=0 \
    --set display/tool_preview_lines=5 \
    --set "display/screen=${FYAI_INLINE_SCREEN:-fullscreen}" --set display/tool_display=inline \
    --set builtin_shell=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/live.out" "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' || fail "the live output is not in the transcript"
import sys

sys.path.insert(0, sys.argv[3])
from screen import Screen


def screen(path):
    data = open(path, "rb").read()
    # The last frame before the session gives the screen back.
    end = data.rfind(b"\x1b[?1049l")
    s = Screen(30, 100)
    s.feed(data[:end] if end > 0 else data)
    return [r for r in s.display() if r.strip()]


def index(rows, pred, what):
    for i, r in enumerate(rows):
        if pred(r):
            return i
    raise SystemExit("no %s on screen:\n%s" % (what, "\n".join(rows)))


rows = screen(sys.argv[1])
card = index(rows, lambda r: "run it" in r, "user card")
label = index(rows, lambda r: r.strip().endswith("shell"), "shell label")
out = index(rows, lambda r: "Building object 7" in r, "live output")
if not card < label < out:
    raise SystemExit("the call is not under its card:\n" + "\n".join(rows))
# The transcript keeps no history of a call, so the block draws no bar.
if any("▴" in r for r in rows):
    raise SystemExit("the block drew a scroll bar:\n" + "\n".join(rows))
# The block shows the last rows of the preview.
if any("Building object 1" in r for r in rows):
    raise SystemExit("the block outgrew its preview:\n" + "\n".join(rows))

rows = screen(sys.argv[2])
label = index(rows, lambda r: r.strip().endswith("shell"), "shell label")
done = index(rows, lambda r: "Done." in r, "answer")
if not label < done:
    raise SystemExit("the stored call is not above the answer:\n" +
                     "\n".join(rows))
PY

# Every frame of the page counts the tiles of the work pane.
grep -q " page: " "$TEST_DIR/trace.log" || fail "the trace has no page"
if grep -E " page: .* tiles=[1-9]" "$TEST_DIR/trace.log" >/dev/null; then
    fail "the call opened a tile of the work pane"
fi

mock_stop 2
pass
