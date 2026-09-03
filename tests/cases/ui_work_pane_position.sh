#!/bin/bash
# SPDX-License-Identifier: MIT
# display/work_position says where the work pane stands. Above the prompt is
# the default; below-prompt puts the live work under the input, so a pane that
# grows moves nothing the user is reading.
set -eu
. "$(dirname "$0")/../harness.sh"

# The captures outlive fyai_test_setup, which makes a scratch directory of
# its own for each run.
CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

# Run one tool call with the pane at $1 and report where its band was drawn
# against the prompt marker.
run_at()
{
    position=$1
    fyai_test_setup
    mock_start ui_band_invocation.json

    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="run it" \
    FYAI_PTY_NEEDLE="Done." \
    FYAI_PTY_PROGRESS_NEEDLE="⋯" \
    FYAI_PTY_PROGRESS_TIMEOUT=5 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/$position.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=false \
        --set display/tool_update_interval_ms=0 \
        --set display/tool_preview_lines=5 \
        --set "display/work_position=$position" \
        --set 'display/prompt=PROMPTMARK ' \
        --set builtin_shell=true --set api=responses \
        --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

    mock_stop 2
    cp "$TEST_DIR/$position.out" "$CAPTURES/$position.out"
}

# The same, set from the prompt of a live session instead of the command
# line: a display setting must take on the next frame, not on the next run.
run_set()
{
    fyai_test_setup
    mock_start ui_band_invocation.json

    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="/config set display/work_position below-prompt" \
    FYAI_PTY_NEEDLE="display/work_position" \
    FYAI_PTY_AFTER="send:run it|wait:Done." \
    FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/set.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/stream=false \
        --set display/tool_update_interval_ms=0 \
        --set display/tool_preview_lines=5 \
        --set 'display/prompt=PROMPTMARK ' \
        --set builtin_shell=true --set api=responses \
        --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

    mock_stop 2
    cp "$TEST_DIR/set.out" "$CAPTURES/set.out"
}

run_at above-prompt
run_at below-prompt
run_set

"$PYTHON" - "$CAPTURES/above-prompt.out" "$CAPTURES/below-prompt.out" \
    "$CAPTURES/set.out" "$TESTS_DIR" <<'PY' || fail "work_position did not move the pane"
import sys

sys.path.insert(0, sys.argv[4])
from screen import rows_at

def where(path):
    rows = rows_at(path, "⋯".encode())
    band = [i for i, r in enumerate(rows) if "⋯" in r]
    prompt = [i for i, r in enumerate(rows) if "PROMPTMARK" in r]
    if not band:
        raise SystemExit("no live band on screen in %s" % path)
    if not prompt:
        raise SystemExit("no prompt on screen in %s" % path)
    return band[0], prompt[0]

band, prompt = where(sys.argv[1])
if not band < prompt:
    raise SystemExit("the default pane was not above the prompt")
band, prompt = where(sys.argv[2])
if not prompt < band:
    raise SystemExit("a below-prompt pane was not under the prompt")
band, prompt = where(sys.argv[3])
if not prompt < band:
    raise SystemExit("a position set in the session did not take")
PY

pass
