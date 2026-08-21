#!/bin/bash
# SPDX-License-Identifier: MIT
# A named shell keeps its own process and terminal. The model types into it and
# reads it by name, and the reading follows what the program did: a program
# that writes lines is returned as lines.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "drive the shell"
assert_status 0

# The results the model saw are the tool messages of the last request.
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the session did not work"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) != 5:
    sys.stderr.write("expected five tool results, got %d\n" % len(results))
    sys.exit(1)
start, typed, read, closed, pipes = results

# The call answers as soon as the terminal exists.
if "started" not in start or "box" not in start:
    sys.stderr.write("the session did not start: %r\n" % start)
    sys.exit(1)
# What was typed reached the program, and its answer came back as lines.
if "FROM-SESSION" not in typed:
    sys.stderr.write("the input never reached the shell: %r\n" % typed)
    sys.exit(1)
if "the lines since the last read" not in typed:
    sys.stderr.write("a line program must be read as lines: %r\n" % typed)
    sys.exit(1)
# The session is still there to be read again.
if "FROM-SESSION" not in read:
    sys.stderr.write("the session lost its output: %r\n" % read)
    sys.exit(1)
if "ended" not in closed:
    sys.stderr.write("the session did not end: %r\n" % closed)
    sys.exit(1)
PYEOF

mock_stop 6
pass
