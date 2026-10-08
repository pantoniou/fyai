#!/bin/bash
# SPDX-License-Identifier: MIT
# write_file makes the directories above the file, as apply_patch does, and a
# write that fails says why.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai --set sandbox=false tool write_file \
	'{"path": "deep/er/new.txt", "content": "made\n"}'
assert_status 0
[ "$(cat deep/er/new.txt)" = made ] || fail "the file was not written in new directories"

# A file in a directory that exists is written as before.
run_fyai --set sandbox=false tool write_file \
	'{"path": "deep/er/new.txt", "content": "again\n"}'
assert_status 0
[ "$(cat deep/er/new.txt)" = again ] || fail "an existing file was not replaced"

# A parent that is a file cannot be made; the result names the path and cause.
run_fyai --set sandbox=false tool write_file \
	'{"path": "deep/er/new.txt/x.txt", "content": "no\n"}'
assert_stdout_contains "deep/er/new.txt/x.txt"
assert_stdout_contains "tool error"

pass
