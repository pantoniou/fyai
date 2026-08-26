#!/bin/bash
# SPDX-License-Identifier: MIT
# The display of a terminal session belongs to the session, not to the calls
# that drive it.
#
# The watch for a program stopped for input is off here. This case leaves a
# shell at its prompt, which that watch reports as a turn of its own, and the
# scenario has no answer for it: what a session displays is the subject, and
# `case_shell_input_wanted` is where that watch is covered.
#
# The call that opens one shows a screen marked running for as long as the
# program is there, and that screen is committed when it goes. The calls that
# type into it and read it show nothing of their own: what they did is on that
# screen. They are still recorded as they always were, which the tool messages
# of the request show.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session.json

FYAI_PTY_INPUT="drive the shell" FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || fail "the session display is wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
plain = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\\\)", b"", plain)

# The calls that drive the session are not shown at all.
for name in (b"shell_input", b"shell_output", b"shell_close"):
    if name in plain:
        raise SystemExit("%s was displayed" % name.decode())
# Nor is the text of their results, which is what a tool body would carry.
if b"the lines since the last read" in plain:
    raise SystemExit("a session reading was displayed as a tool result")

# What the program drew is on the screen of the session, and every row of that
# screen carries the margin that says whose screen it is. The screen starts
# after the margin, under the name of the call that opened it.
if b"FROM-SESSION" not in plain:
    raise SystemExit("the screen of the session was never shown")
if not re.search(rb"\xe2\x94\x82 [^\n]*FROM-SESSION", plain):
    raise SystemExit("the screen of the session carries no margin")

# A program ends a line with a line feed alone. The screen is drawn from
# cells, so without the terminal's own answer to that each line would start
# where the last one ended: every row begins right after the margin.
rows = re.findall(rb"\xe2\x94\x82 ([^\n]*PIPE-[AB][^\n]*)", plain)
if not rows:
    raise SystemExit("the screen of the second session was never shown")
for row in rows:
    if row != row.lstrip():
        raise SystemExit("a line feed did not return the carriage: %r" % row)

# While the program is there the session is marked running (yellow), and the
# screen that is committed when it goes is marked done (green).
if not re.search(rb"\x1b\[33m\xe2\x97\x8f[^\n]*shell", data):
    raise SystemExit("the session was never marked running")
if not re.search(rb"\x1b\[32m\xe2\x97\x8f[^\n]*shell", data):
    raise SystemExit("the session was never committed as done")
EOF

# The record is untouched: every call still answered the model.
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail "the session record changed"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) < 3:
    raise SystemExit("the driving calls were not recorded: %d" % len(results))
if not any("FROM-SESSION" in r for r in results):
    raise SystemExit("what the shell answered was not recorded")
EOF

mock_stop 6

# The margin is the configured one.
mock_start shell_session.json
FYAI_PTY_INPUT="drive the shell" FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/margin.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set 'display/session_margin="[] "' \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/margin.out" <<'EOF' || fail "the margin is not configurable"
import re
import sys

plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"",
               open(sys.argv[1], "rb").read())
if not re.search(rb"\[\] [^\n]*FROM-SESSION", plain):
    raise SystemExit("the configured margin was not used")
if re.search(rb"\xe2\x94\x82 [^\n]*FROM-SESSION", plain):
    raise SystemExit("the default margin was drawn instead")
EOF

mock_stop 6
pass
