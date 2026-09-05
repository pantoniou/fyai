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
    mock_start ui_band_invocation_held.json

    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="run it" \
    FYAI_PTY_NEEDLE="Done." \
    FYAI_PTY_PROGRESS_NEEDLE="[10%] Building object 1" \
    FYAI_PTY_PROGRESS_TIMEOUT=5 \
    FYAI_PTY_PROGRESS_RELEASE="$TMPDIR/band-release" \
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
    mock_start ui_band_invocation_held.json

    # The set prints nothing, so its echo is not the commit: only a
    # round trip proves the live session applied it. The get runs after
    # the set in the same session, and its report carries the committed
    # value; the turn below starts only once that report is on screen,
    # so it runs under the new position instead of racing the frame
    # that applies it. An echo-matched needle would send the turn while
    # the commit is still queued, and the band would run above-prompt
    # even though the setting takes a frame later.
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="/config set display/work_position below-prompt" \
    FYAI_PTY_NEEDLE="below-prompt" \
    FYAI_PTY_AFTER="send:/config get display/work_position|wait-frame:below-prompt|send:run it|wait-frame:[10%] Building object 1|release:$TMPDIR/band-release|wait:Done." \
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
    # The fixture stays live until its first output frame is complete.
    marker = "[10%] Building object 1"
    rows = rows_at(path, marker.encode())
    band = [i for i, r in enumerate(rows) if marker in r]
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
