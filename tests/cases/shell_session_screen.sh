#!/bin/bash
# SPDX-License-Identifier: MIT
# A program that draws a screen is read as the screen it drew. The model opens
# an editor, types into it, sees the drawing, and drives the program out
# itself, which is how such a program must be ended.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

if ! command -v vi >/dev/null 2>&1; then
	echo "vi is not installed; nothing to drive" >&2
	pass
fi

# The editor starts after a delay. A program does not start at the moment the
# call requests it, and a machine with a high load is slower. If input goes to
# a program that did not configure its terminal yet, the line discipline gets
# the keys and the program does not.
mock_start shell_session_screen.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "edit the file"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the editor was not driven"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) < 4:
    sys.stderr.write("expected the full exchange, got %d results\n"
                     % len(results))
    sys.exit(1)
screen = results[2]
# The reading says it is a screen, and carries what the editor drew.
if "the screen it draws" not in screen:
    sys.stderr.write("a drawing program must be read as a screen: %r\n"
                     % screen)
    sys.exit(1)
if "hello from vi" not in screen:
    sys.stderr.write("the typed text is not on the screen: %r\n" % screen)
    sys.exit(1)
# The editor left by itself, thus it could save.
if "ended" not in results[3]:
    sys.stderr.write("the editor did not leave: %r\n" % results[3])
    sys.exit(1)
PYEOF

assert_file_content note.txt 'hello from vi'

mock_stop 6
pass
