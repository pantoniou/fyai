#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_group_partition.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set tools=true --answer=yes \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model "run the mixed calls"
assert_status 0
assert_stdout_contains "partition completed"
assert_request 1 \
	'[m.get("tool_call_id") for m in r["body"]["messages"] '\
'if m.get("role") == "tool"][-3:] == '\
'["call_partition_a", "call_partition_b", "call_partition_ask"]'

mock_stop 2
pass
