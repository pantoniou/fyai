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
# The case and the sibling signal each other through FIFOs, which wake a read
# and need no polling. The FIFOs are outside the project.
held="$MOCK_NOTIFY_DIR/held"
release="$MOCK_NOTIFY_DIR/release"
mkfifo "$held" "$release" || fail "cannot make the FIFOs"

# The sibling holds the lock until the registration is recorded.
(
	exec 9>"$state/auth.lock"
	flock 9
	echo held >"$held"
	read -r _ <"$release" || true
) &
holder=$!
read -r _ <"$held" || fail "the sibling did not take the lock"

mock_watch
"$FYAI_BIN" -k test-key --color off --set api=chat-completions \
	--set display/stream=false --set mcp/enabled=true \
	--set "mcp/servers/default={endpoint: '$MOCK_URL/mcp', auth: \
{type: oauth, allow_browser: true}}" \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "connect MCP" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null 8<&- &
agent=$!

# Each request wakes the read; the registration is one of them.
until grep -q '/oauth/register' "$TEST_DIR/requests.jsonl" 2>/dev/null; do
	read -r -t $((300 * FYAI_TIMEOUT_SCALE)) -u 8 _ ||
		fail "the client was not registered"
done
echo release >"$release"
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
