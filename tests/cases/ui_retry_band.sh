#!/bin/bash
# SPDX-License-Identifier: MIT
# A retry is a step of the running turn, not loose commentary. In the terminal
# it must be drawn as a marked work band like the tool work beside it, and it
# must be repainted for each attempt rather than adding a line each time.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_retry_band.json

FYAI_PTY_INPUT="hello" \
FYAI_PTY_NEEDLE="recovered" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set api=responses \
    --set retry/max_attempts=4 \
    --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PYEOF' || fail "the retry was not drawn as a band"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# The band carries the mark and the title the producer opened it with.
if not re.search(rb"[\xe2\x97\x8f\xe2\x97\x8b\xe2\x97\x90] provider", plain):
    sys.stderr.write("no marked provider band in the pty capture\n")
    sys.stderr.write(plain.decode("utf-8", "replace")[-3000:])
    sys.exit(1)
if b"retrying in" not in plain:
    sys.stderr.write("the band never said what it was waiting for\n")
    sys.exit(1)
PYEOF

mock_stop 3
pass
