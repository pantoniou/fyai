#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a named persona configures the sub-agent.
#
# A persona carries the instructions of the sub-agent and the settings of its
# model. A key that the persona does not set keeps the value in force.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_persona.json

export OPENROUTER_API_KEY=test-env-key
set +e
"$FYAI_BIN" --color off \
	 --set tools=true --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m openrouter/mock-model \
	 --set 'agent/personas/explore/description=Read-only search.' \
	 --set 'agent/personas/explore/system_prompt=You are SCOUT. Report what you find.' \
	 --set 'agent/personas/explore/model=mock-model-mini' \
	 --set 'agent/personas/explore/reasoning/effort=low' \
	 "delegate to the explore persona" \
	 >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
FYAI_STATUS=$?
set -e
assert_status 0
assert_stdout_contains "Delegated and done."

# The parent is told which personas exist, or the model cannot choose one.
assert_request 0 '"explore" in json.dumps(r["body"]["tools"]) and "Read-only search." in json.dumps(r["body"]["tools"])'

# The sub-agent runs under the instructions, model and settings of the persona,
# and the parent keeps its own. The persona model resolves its key after the
# agent process starts, while ordinary tool children do not inherit that key.
assert_request 1 '"You are SCOUT" in json.dumps(r["body"])'
assert_request 1 'r["body"]["model"] == "mock-model-mini"'
assert_request 1 'r["auth"] == "Bearer test-env-key"'
assert_request 2 'r["body"]["model"] == "mock-model"'
assert_request 2 'r["auth"] == "Bearer test-env-key"'

mock_stop 3
pass
