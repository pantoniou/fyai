#!/bin/bash
# SPDX-License-Identifier: MIT
# Shell output becomes prompt text, so it is bounded like a read_file result.
# The end of the output is kept, because a command reports its conclusion
# last, and standard error is served first so the reason a command failed
# survives a noisy standard output. shell/max_output_tokens sets the limit;
# 0 removes it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# 200k lines, far past any sane budget. Bytes follow tokens at 4 bytes each,
# so the default 16384 tokens is a 64 KiB budget.
big='{"command":"seq 1 200000"}'

run_fyai tool shell "$big"
assert_status 0
assert_stdout_contains "bytes elided; the end of the output follows"
# The end is what survives: the last line is intact.
assert_stdout_contains "200000"
bytes=$(wc -c < "$TEST_DIR/stdout")
[ "$bytes" -lt 200000 ] || fail "default bound not applied: $bytes bytes"

# The bounded text must be clean: the arena formatter truncates past its
# local-op limit and leaves trailing garbage, which this catches.
if LC_ALL=C grep -q '[^[:print:][:space:]]' "$TEST_DIR/stdout"; then
	fail "bounded output carries non-printable bytes"
fi

# A model-supplied budget is applied.
run_fyai tool shell '{"command":"seq 1 200000","max_output_tokens":100}'
assert_status 0
small=$(wc -c < "$TEST_DIR/stdout")
[ "$small" -lt 2000 ] || fail "asked-for bound not applied: $small bytes"

# Standard error is served first, so a failure reason survives a noisy stdout.
run_fyai tool shell '{"command":"yes pad | head -c 300000; echo BOOM-ERROR >&2; exit 3"}'
assert_stdout_contains "BOOM-ERROR"

# Output under the budget is untouched, with no elision note.
run_fyai tool shell '{"command":"echo hi"}'
assert_status 0
assert_stdout_contains "hi"
if grep -q "bytes elided" "$TEST_DIR/stdout"; then
	fail "small output was bounded"
fi

# 0 removes the bound.
run_fyai --set shell/max_output_tokens=0 tool shell "$big"
assert_status 0
full=$(wc -c < "$TEST_DIR/stdout")
[ "$full" -gt 1000000 ] || fail "bound not removed: $full bytes"
if grep -q "bytes elided" "$TEST_DIR/stdout"; then
	fail "unbounded output carries an elision note"
fi

pass
