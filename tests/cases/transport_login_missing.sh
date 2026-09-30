#!/bin/bash
# SPDX-License-Identifier: MIT
# With isolation on and neither a key nor a ChatGPT login, the run stops at
# startup and says what is missing. The bootstrap asks the transport: it never
# reads the login store itself.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_chat.json

set +e
FYAI_TRANSPORT_ISOLATION=level-b env -u OPENAI_API_KEY \
	"$FYAI_BIN" --color off -m gpt-4o-mini "hello" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
status=$?
set -e
[ "$status" -ne 0 ] || fail "a run with no credential started"
grep -q "no ChatGPT login" "$TEST_DIR/stderr" ||
	fail "the cause is missing: $(cat "$TEST_DIR/stderr")"
[ ! -s "$TEST_DIR/requests.jsonl" ] || fail "a request reached the provider"
mock_stop 0
