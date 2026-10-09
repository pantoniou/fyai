#!/bin/bash
# SPDX-License-Identifier: MIT
# ask_user with several questions: --answer values are consumed one for each
# question. A number chooses an option, a list of numbers chooses many for a
# question that allows it, and any other text is the answer as typed. A blank
# answer declines. Arguments that are not valid are a tool error with a cause.
set -eu
. "$(dirname "$0")/../harness.sh"

# The result of the tool that the model was given, as the Python object res.
tool_result()
{
	assert_request 1 "$1"
}

run()
{
	run_fyai --set api=chat-completions --set display/stream=false \
		 --set tools=true \
		 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "$@"
}

fyai_test_setup
mock_start ask_user_multi.json
run --answer 2 --answer "1, 3" "ask me two things"
assert_status 0
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["status"] == "answered"'
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][0]["id"] == "scope"'
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][0]["selected"] == ["medium"]'
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][1]["id"] == "q2"'
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][1]["selected"] == ["lint", "asan"]'
mock_stop 2

# Text in place of a number is the answer as typed, and one number only
# for a question that allows one choice.
fyai_test_setup
mock_start ask_user_multi.json
run --answer "1,2" --answer "none of them" "ask me two things"
assert_status 0
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][0] == {"id": "scope", "header": "Scope", "question": "How wide?", "selected": [], "other": "1,2"}'
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["answers"][1]["other"] == "none of them"'
mock_stop 2

# A blank answer declines every question.
fyai_test_setup
mock_start ask_user_multi.json
run --answer 1 --answer "" "ask me two things"
assert_status 0
tool_result 'json.loads([m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"])["status"] == "declined"'
mock_stop 2

# The old shape of the tool is refused, and the model is told why.
fyai_test_setup
mock_start ask_user_invalid.json
run "ask me the old way"
assert_status 0
tool_result '"tool error: ask_user: ask_user needs 1 to 4 questions" in [m for m in r["body"]["messages"] if m.get("role") == "tool"][-1]["content"]'
mock_stop 2

pass
