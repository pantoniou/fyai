#!/bin/bash
# SPDX-License-Identifier: MIT
# Shell-tool output capture, driven straight through the `tool` verb so no
# provider is involved. These pin the behaviour of the event-loop capture path
# in run_shell_command_capture_cb():
#
#   - a large stdout stream survives intact (many read wakeups, buffer growth)
#   - output still buffered when the child exits is not lost - the capture ends
#     on pipe EOF, never on child exit, so a process that writes and exits
#     immediately does not get truncated
#   - stdout and stderr are captured independently and completely
#   - the child's exit status propagates
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# --- large stdout ----------------------------------------------------------
# 20k lines is far past the 4 KiB read chunk and the initial buffer, so this
# only passes if every wakeup appends and nothing is dropped between them.
run_fyai tool shell '{"command":"seq 1 20000"}'
assert_status 0
lines=$(grep -c '^[0-9][0-9]*$' "$TEST_DIR/stdout" || true)
[ "$lines" -eq 20000 ] || fail "large stdout truncated: $lines lines, expected 20000"
assert_stdout_contains "20000"

# --- output buffered at exit ------------------------------------------------
# The command writes and exits at once, so the child is reapable while the pipe
# still holds data. Ending the capture on child exit rather than on EOF would
# truncate this non-deterministically.
run_fyai tool shell '{"command":"seq 1 5000; exit 0"}'
assert_status 0
lines=$(grep -c '^[0-9][0-9]*$' "$TEST_DIR/stdout" || true)
[ "$lines" -eq 5000 ] || fail "output buffered at exit lost: $lines lines, expected 5000"

# --- both streams -----------------------------------------------------------
run_fyai tool shell '{"command":"echo to-stdout; echo to-stderr >&2"}'
assert_status 0
assert_stdout_contains "to-stdout"
assert_stdout_contains "to-stderr"

# Interleaved and large on both pipes at once: neither may starve the other.
run_fyai tool shell '{"command":"seq 1 3000; seq 1 3000 >&2"}'
assert_status 0
assert_stdout_contains "3000"
count=$(grep -c '^3000$' "$TEST_DIR/stdout" || true)
[ "$count" -eq 2 ] || fail "expected the tail of both streams, got $count"

# --- exit status ------------------------------------------------------------
# The tool verb reports a failing command as tool output and still exits 0 -
# the status is the tool's result, not the process's.
run_fyai tool shell '{"command":"exit 7"}'
assert_status 0
assert_stdout_contains "command exited with status 7"

# A signalled child is reported as signalled, not as an exit code.
run_fyai tool shell '{"command":"kill -TERM $$"}'
assert_status 0
assert_stdout_contains "killed by signal 15"

# --- no output at all -------------------------------------------------------
# Both pipes hit EOF with zero bytes; the loop must still complete rather than
# wait for a read that never comes.
run_fyai tool shell '{"command":"true"}'
assert_status 0

pass
