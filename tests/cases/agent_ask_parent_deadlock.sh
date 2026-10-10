#!/bin/bash
# SPDX-License-Identifier: MIT
# A foreground parent cannot answer a child it is waiting to finish.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_ask_parent_deadlock.json
run_fyai --set display/stream=false --set api=responses \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model \
	"start parent question deadlock test"
assert_status 0
assert_stdout_contains "Deadlock handled."
assert_stderr_not_contains "Which colour should I use?"
assert_request 1 '"ask_parent" in [t.get("name") for t in r["body"]["tools"]] and "ask_user" not in [t.get("name") for t in r["body"]["tools"]]'
assert_request 3 '"deadlock: the parent is waiting" in json.dumps(r["body"])'
mock_stop 5
pass
