#!/bin/bash
# SPDX-License-Identifier: MIT
# A terminal session is in the transcript as the exchange it stands for.
#
# The calls that open and drive a session show nothing of their own: the
# session has one screen and what those calls did is on it. That screen is
# drawn on the terminal, but a terminal is not the transcript, so the session
# records the exchange itself. Without it the turn stores nothing for those
# calls and history rebuilds them from the wire - raw arguments and the
# bracketed text of each result - which is not what the user saw.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session_history.json

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set shell/input_poll_ms=0 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "run two tty shells"
assert_status 0

"$FYAI_BIN" --color off history --last 1 > "$TEST_DIR/replay.txt"

"$PYTHON" - "$TEST_DIR/replay.txt" <<'PY' || fail "the session is not in the transcript"
import sys

rows = open(sys.argv[1]).read().splitlines()

# Each session reads as the shell it is: its label, what it was asked to run,
# and what it produced.
for i, desc in ((1, "First wait"), (2, "Second wait")):
    head = [n for n, r in enumerate(rows) if "shell [%s]" % desc in r]
    if not head:
        raise SystemExit("the session '%s' has no title row" % desc)
    n = head[0]
    if "tty shell %d complete" % i not in rows[n + 1]:
        if "⎿" not in rows[n + 1]:
            raise SystemExit("the command of '%s' is not under its title: %r"
                             % (desc, rows[n + 1]))
        if "tty shell %d complete" % i not in rows[n + 2]:
            raise SystemExit("what session %d produced is not under its "
                             "command: %r" % (i, rows[n + 2]))

# None of the wire text the model was given is in the transcript: it is the
# mark of a turn rebuilt from the messages instead of replayed.
for row in rows:
    if "terminal session '" in row or "Read it with shell_output" in row:
        raise SystemExit("the turn was rebuilt from the wire: %r" % row)
    if "exec_command" in row or "shell_output `" in row:
        raise SystemExit("a session call was shown as a raw tool: %r" % row)
PY

mock_stop 3
pass
