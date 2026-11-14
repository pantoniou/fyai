#!/bin/bash
# SPDX-License-Identifier: MIT
# A client registration that completes while a sibling holds the store lock
# is saved when the lock is free, and the login goes on.
set -eu
. "$(dirname "$0")/../harness.sh"
command -v flock >/dev/null || exit 77

fyai_test_setup
mock_start mcp_oauth_login.json
export PATH="$TESTS_DIR/mock:$PATH"

state="$XDG_STATE_HOME/fyai"
mkdir -p "$state"
chmod 700 "$state"
: > "$TEST_DIR/held"

# The sibling holds the lock until the registration is recorded.
(
	exec 9>"$state/auth.lock"
	flock 9
	echo held > "$TEST_DIR/held"
	while [ ! -e "$TEST_DIR/release" ]; do
		sleep 0.05
	done
) &
holder=$!
while [ ! -s "$TEST_DIR/held" ]; do
	sleep 0.05
done

"$FYAI_BIN" -k test-key --color off --set api=chat-completions \
	--set display/stream=false --set mcp/enabled=true \
	--set "mcp/servers/default={endpoint: '$MOCK_URL/mcp', auth: \
{type: oauth, allow_browser: true}}" \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "connect MCP" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null &
agent=$!

tries=$((400 * FYAI_TIMEOUT_SCALE))
i=0
until grep -q '/oauth/register' "$TEST_DIR/requests.jsonl" 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -lt "$tries" ] || fail "the client was not registered"
	sleep 0.05
done
touch "$TEST_DIR/release"
wait "$holder"
set +e
wait "$agent"
FYAI_STATUS=$?
set -e

assert_status 0
assert_stdout_contains "OAuth connected."
assert_any_request \
	'r["path"] == "/mcp" and r["auth"] == "Bearer mcp-access-token"'

mock_stop 10
pass
