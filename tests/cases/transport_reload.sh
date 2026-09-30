#!/bin/bash
# SPDX-License-Identifier: MIT
# A reload executes the program again. With credential isolation the transport
# must live through it, with its channels and the key that came from --api-key,
# so that the session can send requests after one reload and after a second.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_reload.json

FYAI_TRANSPORT_ISOLATION=level-b \
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="Reply one." \
FYAI_PTY_AFTER="wait-screen:Reply one.|"\
"send:/reload|wait-screen:↳ Reply one.|"\
"send:second question|wait-screen:Reply two.|"\
"send:/reload|wait-screen:↳ Reply two.|"\
"send:third question|wait-screen:Reply three." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark -b main --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false -m mock-model -i

# One process, three images of it (bootstrap, supervisor, and one for each
# reload), and one transport that outlived all of them.
"$PYTHON" - "$TEST_DIR/trace.log" <<'PY' || fail "the reloads did not keep one process and one transport"
import re
import sys

starts = re.findall(r"start: pid (\d+), parent (\d+)", open(sys.argv[1]).read())
main = starts[0][0]
if [p for p, _ in starts].count(main) < 4:
    raise SystemExit("expected the main process to start 4 times: %r" % starts)
transports = {p for p, parent in starts if parent == main}
if len(transports) != 1:
    raise SystemExit("expected one transport, saw %r" % transports)
PY
# The key came from --api-key, and it reached the provider after each reload.
assert_request 0 'r["auth"] == "Bearer test-key"'
assert_request 1 'r["auth"] == "Bearer test-key"'
assert_request 2 'r["auth"] == "Bearer test-key"'
mock_stop 3
pass
