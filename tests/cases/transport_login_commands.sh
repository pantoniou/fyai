#!/bin/bash
# SPDX-License-Identifier: MIT
# The login commands of an isolated session run at the transport: they report
# the login and list the accounts, and the configured method is the
# login, not an API key.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_chat.json

state="$XDG_STATE_HOME/fyai"
mkdir -p "$state"
chmod 700 "$state"

# The logout is not run here: it sends a revocation to the issuer. The
# transport driver case covers it with a record that needs none.
write_login() {
	cat >"$state/auth.json" <<EOF
{"type": "chatgpt", "client_id": "oaiapp_test",
 "ext_agent_host_id": "urn:uuid:00000000-0000-4000-8000-000000000000",
 "subject": "user-test", "email": "user@example.com",
 "scope": "openid offline_access resource.invoke chatgpt.tokens.use.direct",
 "access_token": "secret-access", "refresh_token": "$1",
 "id_token": "secret-id", "expires_at": $(( $(date +%s) + 3600 )),
 "registrations": {"oaiapp_test": {"email": "user@example.com",
                                    "subject": "user-test"}}}
EOF
	chmod 600 "$state/auth.json"
}

session() {
	set +e
	FYAI_TRANSPORT_ISOLATION=level-b env -u OPENAI_API_KEY \
		"$FYAI_BIN" --color off --set display/markdown=false \
		--set display/stream=false --set tools=false \
		--set builtin_shell=false -i -m gpt-4o-mini \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
	status=$?
	set -e
}

FYAI_TRANSPORT_ISOLATION=level-b "$FYAI_BIN" -k test-key \
	--color off --set api=chat-completions --set display/markdown=false \
	--set display/stream=false -i >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF'
/log wire start
/auth status
/auth info
/auth accounts
/auth logout
/exit
EOF
assert_stdout_contains "Authentication"
assert_stdout_contains "Signed out"
assert_stdout_contains "api-key"
assert_stdout_contains "auth: logged out"
assert_stderr_not_contains "fatal signal"

write_login secret-refresh
session <<'EOF'
/auth status
/auth info
/auth accounts
EOF
[ "$status" -eq 0 ] || fail "session failed: $(cat "$TEST_DIR/stderr")"
grep -q 'signed_in\|Signed in\|signed in' "$TEST_DIR/stdout" ||
	fail "status does not show the login: $(cat "$TEST_DIR/stdout")"
grep -q 'chatgpt' "$TEST_DIR/stdout" ||
	fail "the method is not the login: $(cat "$TEST_DIR/stdout")"
grep -q 'oaiapp_test' "$TEST_DIR/stdout" ||
	fail "accounts is empty: $(cat "$TEST_DIR/stdout")"
grep -q 'secret-' "$TEST_DIR/stdout" "$TEST_DIR/stderr" &&
	fail "a token reached the output"

[ ! -s "$TEST_DIR/requests.jsonl" ] || fail "a request reached the provider"
mock_stop 0
