#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent gets no session of the process that forked it.
#
# A session belongs to the process that opened it. A fork copies the records,
# and the copies address a program this child cannot drive: the names are taken
# for sessions the sub-agent would open, and a read of one returns what the
# program of the parent wrote. The sub-agent here asks for the name the parent
# is using, and must get a shell of its own.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_session_names.json

run_fyai --set api=responses --set display/stream=false --set tools=true \
	 --set shell/timeout_ms=40000 \
	 --set "api_url=$MOCK_URL/v1/responses" -m mock-model "delegate"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "a sub-agent inherited a session"
import json
import sys

outputs = []
for line in open(sys.argv[1]):
    for item in json.loads(line)["body"].get("input", []):
        if item.get("type") == "function_call_output":
            if item["output"] not in outputs:
                outputs.append(item["output"])

joined = "\n".join(outputs)
if "already open" in joined:
    sys.stderr.write("the name of the parent was taken: %r\n" % joined)
    sys.exit(1)
if "could not name the terminal session" in joined:
    sys.stderr.write("the sub-agent could not open its shell: %r\n" % joined)
    sys.exit(1)
# What it read is its own program, and never the program of the parent.
if not any("CHILD-SESSION" in o for o in outputs):
    sys.stderr.write("the sub-agent never read its own shell: %r\n" % outputs)
    sys.exit(1)
for o in outputs:
    if "PARENT-SESSION" in o and "started" not in o:
        sys.stderr.write("a sub-agent read the shell of the parent: %r\n" % o)
        sys.exit(1)
PYEOF

pass
