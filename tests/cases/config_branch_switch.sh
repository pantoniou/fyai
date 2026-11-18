#!/bin/bash
# SPDX-License-Identifier: MIT
# A session that moves to another branch takes the configuration of that
# branch, and keeps what the command line set for this run.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create one
assert_status 0
run_fyai branch create two
assert_status 0
run_fyai -b one config set temperature 0.25
assert_status 0
run_fyai -b two config set temperature 0.75
assert_status 0

# The model comes from -m, which no branch stores.
pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key -m mock-model -b one -i <<'PTY'
rows: 60
steps:
  - type: "/config effective"
  - wait-screen: "temperature: 0.25"
  - type: "/checkout two"
  - wait-screen: "switched to branch two"
  - type: "/config effective"
  - wait-screen: "temperature: 0.75"
  - wait-screen: "model: mock-model"
PTY
pass
