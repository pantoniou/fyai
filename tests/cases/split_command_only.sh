#!/bin/bash
# SPDX-License-Identifier: MIT
# A client of a session that holds no question keeps the output of its
# commands: the stored command turns replace the live rows and draw them
# again, also after a later command.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
# A session with its settings and no question: the head holds the system turn.
mock_start chat_basic.json
run_fyai -b newer --set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set display/stream=false
assert_status 0

pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" --split --set display/screen=fullscreen \
	--set display/command_output=transcript \
	--set display/notice_output=transcript \
	-k test-key -m mock-model resume --last <<'PTY'
steps:
  - wait-screen: "❯"
  - type: "/fleet list"
  - wait-screen: "Sessions of the fleet"
  - type: "/branch list"
  - wait-screen: "Branches"
PTY

"$PYTHON" -I - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' || fail "a command output left the transcript"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 100)
s.feed(open(sys.argv[1], "rb").read())
rows = [r.rstrip() for r in s.display() if r.strip()]
text = "\n".join(rows)
for need in ("/fleet list", "Sessions of the fleet", "/branch list", "Branches"):
    if need not in text:
        raise SystemExit("no %r on screen:\n%s" % (need, text))
PY
mock_stop 0
pass
