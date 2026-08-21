#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent is reached again by its name.
#
# The name of a sub-agent is the name of its branch, and its conversation is
# committed there. A second call under that name is not a clash: it is that
# sub-agent, asked something else, carrying on from what it already knows.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_revive.json

run_fyai --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 "ask the same sub-agent twice"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the sub-agent did not carry on"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1])]
# The sub-agent's requests are the ones that carry its own instructions.
child = [r for r in reqs
         if "fyai sub-agent" in json.dumps(r["body"])]
if len(child) != 2:
    raise SystemExit("expected two sub-agent requests, got %d" % len(child))

first, second = child
text = json.dumps(first["body"]["input"])
if "ORANGE" not in text:
    raise SystemExit("the first call did not reach the sub-agent")
if "what word did you remember" in text:
    raise SystemExit("the calls were served in the wrong order")

# The second call is the same conversation: what it was told the first time,
# what it answered, and only then the new question.
text = json.dumps(second["body"]["input"])
for want in ("remember the word ORANGE", "the word is ORANGE",
             "what word did you remember"):
    if want not in text:
        raise SystemExit("the sub-agent lost %r" % want)
# One system turn, not one for each call.
if text.count("You are a fyai sub-agent") > 1:
    raise SystemExit("the sub-agent was started again instead of carried on")
PYEOF

# One sub-agent, with both exchanges on its branch.
run_fyai branch --all
assert_status 0
assert_stdout_contains "main/agent:greeter"
[ "$(grep -c 'agent:greeter' "$TEST_DIR/stdout")" -eq 1 ] ||
	fail "expected exactly one sub-agent branch"

run_fyai --branch main/agent:greeter display
assert_status 0
assert_stdout_contains "ORANGE"
assert_stdout_contains "what word did you remember"

mock_stop
pass
