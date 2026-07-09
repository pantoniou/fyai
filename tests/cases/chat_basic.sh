#!/bin/bash
# SPDX-License-Identifier: MIT
# Chat Completions, buffered: canned reply, request wire shape, auth header.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic_twice.json

run_fyai --set logging/wire true --set logging/conversation true \
	 --set api=chat-completions --set display/stream=false -u "$MOCK_URL/v1/chat/completions" \
	 --set "system_prompt=You are a test assistant." -m mock-model "hello mock"
assert_status 0
assert_stdout_contains "Hello from the mock provider."

assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == "Bearer test-key"'
assert_request 0 'r["body"]["model"] == "mock-model"'
assert_request 0 'r["body"]["messages"][0]["role"] == "system"'
assert_request 0 '"You are a test assistant." in r["body"]["messages"][0]["content"]'
assert_request 0 'r["body"]["messages"][-1] == {"role": "user", "content": "hello mock"}'
assert_request 0 '"stream" not in r["body"]'
assert_request 0 '"temperature" in r["body"]'
test -s .fyai/logs/wire.yaml || fail "missing wire log"
test -s .fyai/logs/conversation.yaml || fail "missing conversation log"
grep -q '^---$' .fyai/logs/wire.yaml || fail "wire log missing YAML separator"
grep -q '\[redacted\]' .fyai/logs/wire.yaml || fail "wire log missing redacted key"
! grep -q 'test-key' .fyai/logs/wire.yaml || fail "wire log leaked api key"
grep -q '^---$' .fyai/logs/conversation.yaml || fail "conversation log missing YAML separator"
grep -q 'kind: request' .fyai/logs/conversation.yaml || fail "conversation log missing request"
grep -q 'kind: response' .fyai/logs/conversation.yaml || fail "conversation log missing response"

run_fyai log wire clear
assert_status 0
test ! -s .fyai/logs/wire.yaml || fail "wire log not cleared"

run_fyai --set whitewash_api_keys=false --set logging/wire true \
	 --set api=chat-completions --set display/stream=false -u "$MOCK_URL/v1/chat/completions" \
	 --set "system_prompt=You are a test assistant." -m mock-model "hello again"
assert_status 0
grep -q 'test-key' .fyai/logs/wire.yaml || fail "wire log did not honor --set whitewash_api_keys=false"

cat > editor.sh <<'EOF'
#!/bin/sh
[ "$1" = "-R" ] || exit 2
[ -f "$2" ] || exit 3
printf '%s\n' "$2" > viewed
EOF
chmod +x editor.sh
EDITOR="$PWD/editor.sh" run_fyai log conversation view
assert_status 0
grep -q 'conversation.yaml' viewed || fail "log view did not open conversation log"

mock_stop 2
pass
