#!/bin/bash
# SPDX-License-Identifier: MIT
# A bang session that waits for input is not reported to the model.
#
# The user owns a bang session and reads its screen. The model must not get
# its output, and a turn started for it would take the screen from the user.
set -eu
. "$(dirname "$0")/../harness.sh"

# Only Linux reports which descriptor a stopped process reads. See
# fyai_process_reads_stdin().
[ "$(uname -s)" = "Linux" ] || skip "an input wait is reported on Linux only"

fyai_test_setup
mock_start shell_input_wanted.json

# The bang session waits in cat. Then the model opens a shell of its own that
# waits too, and is told so. The watch of the bang session started first and
# polls at the same interval, so by the time the model answers its own shell,
# the bang session was looked at at least as many times.
FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
FYAI_PTY_INPUT="!sh -c 'printf \"%s-%s\\n\" ASK ME; exec cat'" \
FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=40 \
FYAI_PTY_AFTER="wait-screen:ASK-ME|raw:1d|wait-gone:Ctrl-]|"\
"send:open a shell that asks|wait-screen:It answered.|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/stream=false --set tools=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    fail "the sessions did not run"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the model was told about a bang session"
import json
import sys

reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
users = [m["content"] for m in reqs[-1]["messages"] if m.get("role") == "user"]
asked = [u for u in users if "waiting for input" in u]
# The shell of the model was reported: the watch ran.
if not any("'asker'" in u for u in asked):
    raise SystemExit("the shell of the model was not reported: %r" % asked)
# The bang session was not.
if any("bang-1" in u or "ASK-ME" in u for u in asked):
    raise SystemExit("the bang session was reported: %r" % asked)
PYEOF

mock_stop 4
pass
