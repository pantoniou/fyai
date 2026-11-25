#!/bin/bash
# SPDX-License-Identifier: MIT
# A persona on a decision model: tool_choice required and a result_schema.
# The sub-agent is forced to call a tool, is offered structured_output beside
# its own tools, is asked again when it ends in text, is answered with the
# problems of a result that fails the schema, and returns the result as JSON.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_result.json

run_fyai --set display/stream=false --set tools=true --set api=chat-completions \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 --set 'agent/personas/judge={description: Judge a change., system_prompt: You judge changes., tool_choice: required, tools: [read_file], result_schema: {type: object, required: [verdict, reason], properties: {verdict: {enum: [pass, fail]}, reason: {type: string}}}}' \
	 "judge the change"
assert_status 0
assert_stdout_contains "Judged."

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the typed result of the persona"
import json, sys
reqs = [json.loads(l)["body"] for l in open(sys.argv[1])]
child = reqs[1]
names = [t["function"]["name"] for t in child["tools"]]
assert sorted(names) == ["read_file", "structured_output"], names
assert child["tool_choice"] == "required", child["tool_choice"]
params = [t for t in child["tools"]
          if t["function"]["name"] == "structured_output"][0]["function"]["parameters"]
assert params["required"] == ["verdict", "reason"], params
assert "call structured_output with the result" in child["messages"][0]["content"]
# The text answer is asked for the result again.
assert "Give the result now" in json.dumps(reqs[2]["messages"][-1]), reqs[2]["messages"][-1]
# The result that fails the schema is answered with the problem.
bad = [m for m in reqs[3]["messages"] if m.get("tool_call_id") == "call_bad"][0]
assert "reason" in bad["content"] and "verdict" in bad["content"], bad
# The parent gets the result as JSON.
report = [m for m in reqs[4]["messages"] if m.get("tool_call_id") == "call_agent"][0]
assert json.loads(report["content"]) == {"verdict": "pass", "reason": "fine"}, report
PY

mock_stop 5
pass
