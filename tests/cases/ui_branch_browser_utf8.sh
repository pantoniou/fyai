#!/bin/bash
# SPDX-License-Identifier: MIT
# The prompt editor preserves Unicode branch names across deletion and submit.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create existing
assert_status 0

pty_run "$TEST_DIR/browser.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true -m mock-model -i <<'PTY'
steps:
  - type: "/branches"
  - wait: Branches
    timeout: 1.5
  - write: "nδοκιμή/日本語界\x7f\r\r"
  - wait: Branches
    timeout: 1.5
  - wait: "created branch δοκιμή/日本語"
    timeout: session
  # The keys go back to the prompt: it takes what is typed next. The browser
  # stays on the screen, so the case does not wait for it to leave.
  - write: "\x1d"
  - write: "probe"
  - wait-input: probe
  - write: "\x15"
  - wait-gone: probe
PTY

run_fyai branch show δοκιμή/日本語
assert_status 0
assert_stdout_contains "δοκιμή/日本語"
pass
