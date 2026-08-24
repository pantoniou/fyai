#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent gets no wait of the process that forked it.
#
# A wait is a timer on the loop of the process that started it, and the model
# that started it is the one waiting. A fork copies the records, so a sub-agent
# found the names of the parent taken for waits of its own. The sub-agent here
# asks for the name the parent is using, and must get its own wait.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_wait_names.json

run_fyai --set api=responses --set display/stream=false --set tools=true \
	 --set "api_url=$MOCK_URL/v1/responses" -m mock-model "delegate"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "a sub-agent inherited a wait"
import json
import sys

outputs = {}
for line in open(sys.argv[1]):
    for item in json.loads(line)["body"].get("input", []):
        if item.get("type") == "function_call_output":
            outputs[item["call_id"]] = item["output"]

child = outputs.get("call-child-wait", "")
if "already open" in child:
    sys.stderr.write("the name of the parent was taken: %r\n" % child)
    sys.exit(1)
if "started" not in child:
    sys.stderr.write("the sub-agent started no wait: %r\n" % child)
    sys.exit(1)
if "started" not in outputs.get("call-parent-wait", ""):
    sys.stderr.write("the parent started no wait: %r\n"
                     % outputs.get("call-parent-wait"))
    sys.exit(1)
PYEOF

pass
