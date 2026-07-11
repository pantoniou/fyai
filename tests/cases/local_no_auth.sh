#!/bin/bash
# SPDX-License-Identifier: MIT
# Local no-auth model servers (Ollama, llama.cpp's llama-server, vLLM, ...):
# api_url set explicitly (no catalogue entry needed) against the Chat
# Completions grammar. Without no_auth, a run with no key anywhere still
# hard-errors as usual; with no_auth=true, the request goes out with neither
# an Authorization nor an x-api-key header.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json	# defines MOCK_URL / MOCK_PORT

# An explicit, deliberately absent env reference disables the automatic
# provider environment/keyring fallback, making the missing-key case
# independent of the host running the test.
"$FYAI_BIN" config set api_key \
	'{ type: env, value: FYAI_TEST_MISSING_API_KEY }' >/dev/null 2>&1 || \
	fail "set absent API-key environment reference"
LOCAL="--set api=chat-completions --set auth=api-key --set api_url=$MOCK_URL/v1/chat/completions -m llama3"

run_bare() {
	local flags="$1"; shift
	set +e
	# No -k anywhere, and no <PROVIDER>_API_KEY env (scrubbed by
	# fyai_test_setup): the key must come from no_auth, not a fallback.
	# shellcheck disable=SC2086
	"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false \
		$flags "$@" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

# Without no_auth, the usual missing-key gate remains in force.
unset FYAI_TEST_MISSING_API_KEY || true
run_bare "$LOCAL" "hello"
assert_status_nonzero
assert_stderr_contains "no API key"

# With no_auth: request goes out with no auth headers at all.
run_bare "$LOCAL --set no_auth=true" "hello"
assert_status 0
assert_stdout_contains "Hello from the mock provider."
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == ""'
assert_request 0 'r["x_api_key"] == ""'
mock_stop 1

pass
