#!/bin/bash
# SPDX-License-Identifier: MIT
# The resume picker opens on the recent sessions, resumes the one Enter
# selects, and stores nothing when it is cancelled.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai branch create older
assert_status 0
run_fyai branch create newer
assert_status 0
run_fyai branch describe newer "the newest session"
assert_status 0

# Make `newer` the most recently updated session, so the row the picker opens
# on is the one this case names.
run_fyai -b newer --set temperature=0.5
assert_status 0

# The picker holds the keys as soon as it opens, so the line the driver sends
# first is read as keys: Enter resumes, Escape cancels.

# --- Enter resumes the selected session ----------------------------------
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=90 \
FYAI_PTY_INPUT="" \
FYAI_PTY_READY_NEEDLE="Enter resume" \
FYAI_PTY_NEEDLE="Sessions" \
FYAI_PTY_AFTER="wait-frame:switched to branch|send:/help|wait:Settings|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pick.out" \
	"$FYAI_BIN" -k test-key -m mock-model resume

# The picker drew the sessions it offers, newest first.
grep -qF "Sessions" "$TEST_DIR/pick.out" || fail "the picker did not open"
grep -qF "recent" "$TEST_DIR/pick.out" || fail "the picker did not open on recent"
grep -qF "the newest session" "$TEST_DIR/pick.out" || \
	fail "the picker did not describe the sessions"
# Enter resumed the newest of them.
grep -qF "switched to branch newer" "$TEST_DIR/pick.out" || \
	fail "Enter did not resume the selected session"
"$PYTHON" - "$TEST_DIR/pick.out" <<'EOF' || \
	fail "resumed session lost automatic colour"
import sys

data = open(sys.argv[1], "rb").read()
start = data.rfind(b"switched to branch")
help_text = data.find(b"Settings", start)
if start < 0 or help_text < 0 or b"\x1b[" not in data[start:help_text]:
    raise SystemExit("no styling after branch configuration was adopted")
EOF

# Selecting a session does not move HEAD.
run_fyai root show
assert_status 0
assert_stdout_contains "main"

# --- Escape cancels and stores nothing -----------------------------------
run_fyai list reflog
assert_status 0
cp "$TEST_DIR/stdout" "$TEST_DIR/reflog.before"

FYAI_PTY_ROWS=24 FYAI_PTY_COLS=90 \
FYAI_PTY_INPUT=$'\x1b' \
FYAI_PTY_READY_NEEDLE="Enter resume" \
FYAI_PTY_NEEDLE="Sessions" \
FYAI_PTY_AFTER="drain:0.5" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/cancel.out" \
	"$FYAI_BIN" -k test-key --color off -m mock-model resume

grep -qF "Sessions" "$TEST_DIR/cancel.out" || fail "the picker did not open"
grep -qF "switched to branch" "$TEST_DIR/cancel.out" && \
	fail "Escape resumed a session" || true

# A cancelled picker publishes nothing.
run_fyai list reflog
assert_status 0
cmp -s "$TEST_DIR/reflog.before" "$TEST_DIR/stdout" || \
	fail "a cancelled picker changed the stored state"

pass
