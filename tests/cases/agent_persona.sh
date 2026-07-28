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

run_fyai --set tools=true --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
	 --set 'agent/personas/explore/description=Read-only search.' \
	 --set 'agent/personas/explore/system_prompt=You are SCOUT. Report what you find.' \
	 --set 'agent/personas/explore/model=mock-model-mini' \
	 --set 'agent/personas/explore/reasoning/effort=low' \
	 "delegate to the explore persona"
assert_status 0
assert_stdout_contains "Delegated and done."

# The parent is told which personas exist, or the model cannot choose one.
assert_request 0 '"explore" in json.dumps(r["body"]["tools"]) and "Read-only search." in json.dumps(r["body"]["tools"])'

# The sub-agent runs under the instructions, model and settings of the persona,
# and the parent keeps its own.
assert_request 1 '"You are SCOUT" in json.dumps(r["body"])'
assert_request 1 'r["body"]["model"] == "mock-model-mini"'
assert_request 2 'r["body"]["model"] == "mock-model"'

mock_stop 3
pass
