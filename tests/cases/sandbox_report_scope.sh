#!/bin/bash
# SPDX-License-Identifier: MIT
# The report that a sandbox profile cannot restrict egress is for a run that
# starts tools. A command that starts none, such as the completion script that a
# shell sources, a listing or a configuration command, says nothing about it.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unset FYAI_TEST_NO_HOST_WARNINGS

# A run that starts tools reports, on a kernel that cannot restrict egress. On a
# kernel that can, there is nothing to compare with.
run_fyai --set sandbox_profile=dev tool shell '{"command": "true"}'
grep -q 'egress remains open' "$TEST_DIR/stderr" || exit 77

for verb in "completion bash" "config get sandbox_profile" "branch list" "help"; do
	run_fyai --set sandbox_profile=dev $verb
	if grep -q 'egress remains open' "$TEST_DIR/stderr"; then
		fail "fyai $verb reported the sandbox"
	fi
done

pass
