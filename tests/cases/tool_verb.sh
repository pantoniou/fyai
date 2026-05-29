#!/bin/bash
# SPDX-License-Identifier: MIT
# The `fyai tool <name>` verb runs each built-in tool as a standalone sandboxed
# sub-execution of self - no model round-trip. Covers the shared tool executor
# (read_file, write_file, shell), argument parsing from argv and stdin, and the
# error paths.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# write_file creates the file and reports ok
run_fyai tool write_file '{"path": "w.txt", "content": "hello tool\n"}'
assert_status 0
assert_stdout_contains "ok"
assert_file_content w.txt "hello tool"

# read_file returns the content
run_fyai tool read_file '{"path": "w.txt"}'
assert_status 0
assert_stdout_contains "hello tool"

# shell runs the command and returns its captured output
run_fyai tool shell '{"command": "echo shell-ran-42"}'
assert_status 0
assert_stdout_contains "shell-ran-42"

# arguments may come from stdin when omitted on the command line
"$FYAI_BIN" -k test-key --color off --no-markdown tool read_file \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'JSON'
{"path": "w.txt"}
JSON
assert_stdout_contains "hello tool"

# an unknown tool reports an error but the process still exits cleanly
run_fyai tool bogus '{}'
assert_status 0
assert_stdout_contains "unknown tool bogus"

# a missing tool name is a usage error
run_fyai tool
assert_status_nonzero

pass
