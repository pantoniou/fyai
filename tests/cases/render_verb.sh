#!/bin/bash
# SPDX-License-Identifier: MIT
# `fyai render` writes Markdown from a file or standard input through the sink.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup_bare

printf '# Heading\n\n| a | b |\n|---|---|\n| 1 | 2 |\n' >"$TEST_DIR/in.md"

run_fyai render "$TEST_DIR/in.md"
assert_status 0
assert_stdout_contains "Heading"
assert_stdout_not_contains "|---|"

# run_fyai closes standard input, so the pipe case runs the binary itself.
"$FYAI_BIN" -k test-key --color off render <"$TEST_DIR/in.md" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
assert_stdout_contains "Heading"

run_fyai render "$TEST_DIR/missing.md"
assert_status_nonzero
assert_stderr_contains "cannot read"

pass
