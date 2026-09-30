#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai transport, driven over its control channel as a supervisor does:
# init, profiles, admit, grant, reload, requests, events, shutdown.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_chat.json

set +e
"$PYTHON" "$TESTS_DIR/transport_driver.py" "$FYAI_BIN" "$MOCK_URL" \
	"$TEST_DIR/.fyai/arena" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
status=$?
set -e
[ "$status" -eq 0 ] || fail "transport driver: $(cat "$TEST_DIR/stdout") $(cat "$TEST_DIR/stderr") $(cat "$TEST_DIR/transport.stderr" 2>/dev/null)"
grep -q '^OK$' "$TEST_DIR/stdout" || fail "transport driver did not finish"
mock_stop 5
