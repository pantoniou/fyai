#!/bin/bash
# SPDX-License-Identifier: MIT
# The effective isolation is visible: /status names the level that `auto`
# chose, what was asked for, and the transport; a run without isolation says
# none.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup

# The table wraps a long value: read the report as one line of words.
words() {
	tr '\n│' '  ' <"$TEST_DIR/stdout" | tr -s ' '
}

status_of() {
	# An interactive session over a pipe: /status, then end of input.
	printf '/status\n' | env "$@" "$FYAI_BIN" -k test-key --color off \
		--set display/markdown=false -i -m mock-model \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" || true
}

status_of FYAI_TRANSPORT_ISOLATION=none
words | grep -qi "Isolation none" || fail "an unisolated run does not say none: $(cat "$TEST_DIR/stdout")"

status_of FYAI_TRANSPORT_ISOLATION=level-b
words | grep -qi "Isolation level-b (transport pid [0-9]*; execution 1 of 1; [0-9]* profiles; 0 in flight)" ||
	fail "level-b is not shown: $(cat "$TEST_DIR/stdout")"
! words | grep -q "requested" || fail "a level that was asked for is reported as auto"

# `auto` is not an answer: the level that it chose is, and what was asked for.
status_of FYAI_TRANSPORT_ISOLATION=auto
words | grep -qi "Isolation level-b (requested auto; transport pid [0-9]*; execution 1 of 1" ||
	fail "auto is not resolved: $(cat "$TEST_DIR/stdout")"

pass
