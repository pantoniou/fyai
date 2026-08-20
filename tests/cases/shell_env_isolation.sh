#!/bin/bash
# SPDX-License-Identifier: MIT
# A model-issued shell command runs without the provider credentials. This is
# the sequential path: the tool call runs in the parent process, so the shell
# child is the first place the environment can be cut down.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_env_isolation.json

export MY_SECRET=topsecret-xyz
export OPENAI_API_KEY=sk-should-not-leak

run_fyai --set api=responses --set display/stream=false --set builtin_shell=true \
	 --set parallel_tool_calls=false \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model "read the environment"
assert_status 0
assert_stdout_contains "Shell environment checked."
assert_stdout_not_contains "topsecret-xyz"
assert_stdout_not_contains "sk-should-not-leak"

# The provider receives the environment data that the command reports.
assert_request 1 'all("topsecret-xyz" not in o.get("stdout", "") for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'
assert_request 1 'any("secret=[] key=[]" in o.get("stdout", "") for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'

mock_stop 2
pass
