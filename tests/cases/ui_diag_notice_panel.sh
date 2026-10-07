#!/bin/bash
# SPDX-License-Identifier: MIT
# A notice that a turn raises, here a sub-agent that outlives its advisory
# limit, is shown in the notice panel under a heading. It is not drawn into the
# transcript as a bare row, where it would read as a part of the conversation.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

for screen in fullscreen inline; do
	mock_start agent_tool_responses.json
	FYAI_PTY_INPUT="delegate a greeting to a sub-agent" \
	FYAI_PTY_NEEDLE="delegate a" FYAI_PTY_TIMEOUT=30 \
	FYAI_PTY_AFTER="wait-screen:Delegated and done.|wait-screen:advisory|snapshot" \
	FYAI_PTY_SNAPSHOT="$TEST_DIR/live-$screen.out" \
	"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty-$screen.out" \
	    "$FYAI_BIN" -k test-key --theme dark \
	    --set display/markdown=true --set display/stream=false \
	    --set "display/screen=$screen" --set display/tool_display=inline \
	    --set tools=true --set api=responses --set builtin_shell=true \
	    --set agent/timeout_ms=800 \
	    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i
	mock_stop 4

	"$PYTHON" - "$TEST_DIR/live-$screen.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 100)
s.feed(open(sys.argv[1], "rb").read())
rows = [r.rstrip() for r in s.display() if r.strip()]
text = "\n".join(rows)
note = next((i for i, r in enumerate(rows) if "advisory" in r), None)
if note is None:
    raise SystemExit("no notice on screen:\n" + text)
# The panel names what it shows: on the row of the notice, or above it.
if not any("notice" in r for r in rows[max(0, note - 1):note + 1]):
    raise SystemExit("the notice has no heading, it is a row of the "
                     "transcript:\n" + text)
PY
		fail "the notice of the turn is not in the panel ($screen)"
done

pass
