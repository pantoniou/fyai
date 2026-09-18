#!/bin/bash
# SPDX-License-Identifier: MIT
# display/renderer=page states the live screen as one UI Markdown page. It
# must look as the band stack does: the same tool call is run under each
# renderer, and the screens are compared at the same point of the call.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

run_with()
{
    renderer=$1
    fyai_test_setup
    mock_start ui_band_invocation_hold_first.json

    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="run it" \
    FYAI_PTY_NEEDLE="Done." \
    FYAI_PTY_PROGRESS_NEEDLE="[10%] Building object 1" \
    FYAI_PTY_PROGRESS_TIMEOUT=5 \
    FYAI_PTY_PROGRESS_RELEASE="$TMPDIR/band-release" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/screen.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=false \
        --set display/tool_update_interval_ms=0 \
        --set display/tool_preview_lines=5 \
        --set "display/renderer=$renderer" \
        --set display/work_cap=true \
        --set 'display/prompt=PROMPTMARK ' \
        --set 'display/prompt_top=HEADMARK' \
        --set builtin_shell=true --set api=responses \
        --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

    mock_stop 2
    cp "$TEST_DIR/screen.out" "$CAPTURES/$renderer.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer.trace"
}

run_with stack
run_with page

if grep -a -q "needs a libfytimui" "$CAPTURES/page.out"; then
    skip "this build has no page support"
fi

# The band stack draws the same screen: only the trace says that the page
# composed it.
grep -a -q "page.*regions=" "$CAPTURES/page.trace" ||
    fail "no page was rendered"

"$PYTHON" - "$CAPTURES/stack.out" "$CAPTURES/page.out" "$TESTS_DIR" \
    <<'PY' || fail "the page did not look as the band stack does"
import re
import sys

sys.path.insert(0, sys.argv[3])
from screen import rows_at

def same(a, b):
    """Equal rows, or rows that differ only in the activity mark of the
    status gutter or elapsed time: two runs meet them in other phases."""
    a, b = a.rstrip(), b.rstrip()
    a = re.sub(r" \d+s(?=\s|$)", " TIME", a)
    b = re.sub(r" \d+s(?=\s|$)", " TIME", b)
    if a == b:
        return True
    gutter = 2
    marks = set(a[:gutter] + b[:gutter]) - {" "}
    return a[gutter:] == b[gutter:] and all(ord(c) > 127 for c in marks)

# A frame while the tool call runs, and the one after it ends. The shell
# waits for the release of the driver after its first line, so that frame
# stands still: a frame later in the call holds what the run had drawn by
# then, which is not the same in two runs.
for needle in ("[10%] Building object 1", "Done."):
    stack = rows_at(sys.argv[1], needle.encode())
    page = rows_at(sys.argv[2], needle.encode())
    differ = [(i, a, b) for i, (a, b) in enumerate(zip(stack, page))
              if not same(a, b)]
    if differ:
        raise SystemExit("at %r:\n%s" % (needle, "\n".join(
            "%2d stack|%s\n   page |%s" % (i, a.rstrip(), b.rstrip())
            for i, a, b in differ)))
PY

pass
