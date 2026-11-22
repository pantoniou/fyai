#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a kernel which cannot restrict an operation of the sandbox profile
# is reported when the process starts, not at each tool call, and that a
# sub-agent adds no report of its own.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

# The sandbox grants the system scratch trees whole: keep the project out of them.
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unset FYAI_TEST_NO_HOST_WARNINGS
printf baseline > file

# count_reports: the lines of the last run that say that egress stays open.
count_reports() {
	grep -c 'egress remains open' "$TEST_DIR/stderr" || true
}

# One start of the process with the profile: the report that this kernel gives, 0 or 1.
# A kernel that can restrict every operation gives none, and the case then checks
# that the later runs give none too.
run_fyai --set sandbox_profile=dev tool shell '{"command": "true"}'
assert_status 0
EXPECTED="$(count_reports)"
[ "$EXPECTED" -le 1 ] || fail "one start reports $EXPECTED times"

# A session of several tool calls, one of them in a sub-agent, reports as one start does.
mock_start agent_sandbox.json
run_fyai --set sandbox_profile=dev --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate an edit"
assert_status 0
assert_stdout_contains "Parent and sub-agent done."
[ "$(count_reports)" -eq "$EXPECTED" ] ||
	fail "the run reported $(count_reports) times, a start reports $EXPECTED"
mock_stop 4

# A sub-agent that is forked, not executed, inherits the diagnostics that its parent has
# not drained yet. It must not give them back as its own.
mock_start agent_sandbox.json
run_fyai --set sandbox_profile=dev --set agent/spawn=fork --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate an edit"
assert_status 0
assert_stdout_contains "Parent and sub-agent done."
[ "$(count_reports)" -eq "$EXPECTED" ] ||
	fail "the forked run reported $(count_reports) times, a start reports $EXPECTED"
mock_stop 4
pass
