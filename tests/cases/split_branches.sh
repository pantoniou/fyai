#!/bin/bash
# SPDX-License-Identifier: MIT
# The branch browser of a split session has no arena: it asks its worker for
# the tree, and runs its actions as commands of the worker. A branch that the
# form creates is in the arena afterwards, and Enter switches to a branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create existing
assert_status 0
run_fyai branch create other
assert_status 0

# A branch that the form makes is made by the worker, and the tree is read
# again: the list has it, and the session stays where it was.
pty_run "$TEST_DIR/create.out" \
	"$FYAI_BIN" --split -b main -k test-key --theme catppuccin:dark \
	--set display/markdown=true -m mock-model -i <<'PTY'
steps:
  - type: "/branches"
  - wait-screen: Branches
  - wait-screen: existing
  - write: "nreview\r\r"
  - wait-screen: created branch review
  - wait-screen: "- review"
  - write: "i"
  - wait-screen: Field
  - write: "\e"
  - wait-gone: Field
  - write: "\e"
  - wait-gone: Branches
  - type: /quit
PTY

# Enter switches to the selected branch, in the arena of the worker.
pty_run "$TEST_DIR/switch.out" \
	"$FYAI_BIN" --split -b main -k test-key --theme catppuccin:dark \
	--set display/markdown=true -m mock-model -i <<'PTY'
steps:
  - type: "/branches"
  - wait-screen: other
  - write: "j"
  - write: "\r"
  - wait-screen: switched to branch
  - type: /quit
PTY

run_fyai branch list
assert_status 0
assert_stdout_contains "review"
pass
