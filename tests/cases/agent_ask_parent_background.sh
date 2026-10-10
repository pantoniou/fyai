#!/bin/bash
# SPDX-License-Identifier: MIT
# A background child asks its parent, waits, and receives a correlated reply.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_ask_parent_background.json
run_fyai --set display/stream=false --set api=responses \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model \
	"start parent question background test"
assert_status 0
assert_stdout_contains "Parent received green report."
assert_request 2 '"ask_parent" in [t.get("name") for t in r["body"]["tools"]] and "ask_user" not in [t.get("name") for t in r["body"]["tools"]]'
assert_request 6 '"[ask_parent '\''choice'\'' answered]" in json.dumps(r["body"]) and "Use green." in json.dumps(r["body"])'
mock_stop 8
pass
