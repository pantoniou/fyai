#!/bin/bash
# SPDX-License-Identifier: MIT
# A delegated agent cannot call ask_user, even if a provider sends that name.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_asks_user.json

run_fyai --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 "ask a sub-agent to ask me"
assert_status 0

assert_stderr_not_contains "WHICH-COLOUR?"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the sub-agent called ask_user"
import json
import sys

reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
child = [r for r in reqs if "fyai sub-agent" in json.dumps(r)]
if len(child) < 2:
    raise SystemExit("the sub-agent never called the tool")
tools = {t.get("name") for t in child[0].get("tools", [])}
if "ask_user" in tools or "ask_parent" not in tools:
    raise SystemExit("the delegated tool set is wrong: %r" % tools)
text = json.dumps(child[-1]["input"])
if "ask_user is available only in the root session" not in text:
    raise SystemExit("the sub-agent was not refused: %r" % text)
PYEOF

mock_stop 4
pass
