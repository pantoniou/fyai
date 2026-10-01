#!/bin/bash
# SPDX-License-Identifier: MIT
# A 401 to a login request is retried one time at the transport with the
# stored token; the agent sees the second reply only.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_login_retry.json

set +e
PYTHONPATH="$TESTS_DIR" "$PYTHON" "$TESTS_DIR/transport_login_retry_driver.py" \
	"$FYAI_BIN" "$MOCK_URL" "$TEST_DIR/.fyai/arena" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
status=$?
set -e
[ "$status" -eq 0 ] || fail "retry driver: $(cat "$TEST_DIR/stdout") $(cat "$TEST_DIR/stderr") $(cat "$TEST_DIR/transport.stderr" 2>/dev/null)"
grep -q '^OK$' "$TEST_DIR/stdout" || fail "retry driver did not finish"
mock_stop 2
