#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent asks the user through its parent.
#
# A sub-agent has a terminal of its own but nobody at it. The question goes up
# to the parent, which has the person, and comes back as the answer to the
# call: the sub-agent waits for it exactly as it would wait for anything else.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_asks_user.json

run_fyai --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 --answer "green" "ask a sub-agent to ask me"
assert_status 0

# The person was asked, and told which sub-agent is asking.
assert_stderr_contains "the sub-agent 'asker' asks"
assert_stderr_contains "WHICH-COLOUR?"
# The options of the sub-agent are the options put to the user.
assert_stderr_contains "1) red"
assert_stderr_contains "2) green"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the answer never came back"
import json
import sys

reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
child = [r for r in reqs if "fyai sub-agent" in json.dumps(r)]
if len(child) < 2:
    raise SystemExit("the sub-agent never asked and answered")
# The answer of the person is the result of the sub-agent's own call.
text = json.dumps(child[-1]["input"])
if "green" not in text:
    raise SystemExit("the sub-agent was never given the answer: %r" % text)
PYEOF

mock_stop 4
pass
