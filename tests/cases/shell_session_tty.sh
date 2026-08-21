#!/bin/bash
# SPDX-License-Identifier: MIT
# A session runs on pipes unless a terminal is asked for.
#
# A terminal is what makes a program behave as it does for a person: git opens
# a pager, and the session then waits for a key nobody is going to press. A
# session is a process that stays open, not a terminal, so it gets one only
# when the call asks for it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session_tty.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "probe the session"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "a session took the wrong terminal"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) != 6:
    sys.stderr.write("expected six tool results, got %d\n" % len(results))
    sys.exit(1)
plain, plain_read, tty, tty_read, lines, lines_read = results

# Named, with no request for a terminal: the program finds a pipe.
if "ON-A-PIPE" not in plain_read:
    sys.stderr.write("a session was given a terminal nobody asked for: %r\n"
                     % plain_read)
    sys.exit(1)
# Asked for: the program finds a terminal.
if "ON-A-TERMINAL" not in tty_read:
    sys.stderr.write("a session that asked for a terminal had none: %r\n"
                     % tty_read)
    sys.exit(1)
# A program on a pipe ends a line with a line feed alone. Its screen must
# read as lines, not as a staircase: each line starts at the first column.
for row in lines_read.splitlines():
    row = row.rstrip()
    if row.lstrip().startswith("LINE-") and row != row.lstrip():
        sys.stderr.write("a line feed did not return the carriage: %r\n" % row)
        sys.exit(1)
for want in ("LINE-A", "LINE-B", "LINE-C"):
    if want not in lines_read:
        sys.stderr.write("the screen lost %s: %r\n" % (want, lines_read))
        sys.exit(1)

# Either way it is a session: it answered before the program ended.
for r in (plain, tty, lines):
    if "started" not in r:
        sys.stderr.write("the session did not start: %r\n" % r)
        sys.exit(1)
PYEOF

mock_stop 7
pass
