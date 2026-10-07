#!/bin/bash
# SPDX-License-Identifier: MIT
# config undo returns to the configuration the branch had N changes ago, and
# an undo is a change itself. config reset stores an empty configuration, so
# every key takes its built-in default, and an undo brings back what it
# replaced.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai config set display/theme dark
assert_status 0
run_fyai config set display/theme light
assert_status 0
run_fyai config set display/theme_ground terminal
assert_status 0

# One back: the ground is gone and the theme stays light.
run_fyai config undo
assert_status 0
run_fyai config get display/theme
assert_status 0
assert_stdout_contains "light"
run_fyai config get display/theme_ground
assert_status 1

# The undo was a change: one back from it returns the ground.
run_fyai config undo 1
assert_status 0
run_fyai config get display/theme_ground
assert_status 0
assert_stdout_contains "terminal"

# Each change, newest first: the second undo (ground), the first undo
# (light), the ground, light, dark. Four back is the first theme.
run_fyai config undo 4
assert_status 0
run_fyai config get display/theme
assert_status 0
assert_stdout_contains "dark"

# Further back than the ref log goes is refused and changes nothing.
run_fyai config undo 1000
assert_status 1
assert_stderr_contains "earlier configuration"
run_fyai config get display/theme
assert_status 0
assert_stdout_contains "dark"

# reset: no key is stored; undo brings the theme back.
run_fyai config reset
assert_status 0
assert_stdout_contains "reset to the built-in defaults"
run_fyai config show
assert_status 0
assert_stdout_contains "{}"
run_fyai config undo
assert_status 0
run_fyai config get display/theme
assert_status 0
assert_stdout_contains "dark"

pass
