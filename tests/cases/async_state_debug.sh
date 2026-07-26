#!/bin/bash
# SPDX-License-Identifier: MIT
# Debug mode reports every formal async state-machine transition.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_stream.json

run_fyai -d --set api=responses --set display/stream=true \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model "debug states"
assert_status 0
assert_stderr_contains "model step state new -> building"
assert_stderr_contains "model step state building -> request-pending"
assert_stderr_contains "response stream state new -> submitted"
assert_stderr_contains "model step state request-pending -> completed"

mock_stop 1

mock_start responses_stream.json

FYAI_PTY_INPUT="debug turn" \
FYAI_PTY_NEEDLE="turn state model -> done" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -d -k test-key --set api=responses \
	--set display/markdown=true --set display/stream=true \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
	fail "interactive turn transitions were not logged"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
for transition in (
    b"turn state new -> model",
    b"turn state model -> done",
):
    if transition not in plain:
        raise SystemExit("missing transition: %r" % transition)
EOF

mock_stop 1
pass
