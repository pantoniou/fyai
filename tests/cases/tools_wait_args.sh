#!/bin/bash
# SPDX-License-Identifier: MIT
# What the wait tool accepts.
#
# A model that fills in every property of the schema sends `until: ""` beside
# the seconds it means. An empty argument is not an argument: refusing that as
# "both" refuses a call that asked for one thing. A call that really gives both
# is still refused, and the refusal says what it was given.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The call a model actually made: seconds, and an empty until beside it.
run_fyai tool wait '{"seconds":1,"until":"","name":"later","reason":"a note"}'
assert_status 0
grep -q "wait 'later' started" "$TEST_DIR/stdout" ||
	{ cat "$TEST_DIR/stdout" >&2; fail "an empty until was read as a request"; }

# A wait that holds the turn, with the same empty argument beside it.
run_fyai tool wait '{"seconds":0,"until":""}'
assert_status 0
grep -q "waited" "$TEST_DIR/stdout" || fail "the wait did not run"

# Both, really given: refused, and the refusal names them.
run_fyai tool wait '{"seconds":5,"until":"23:59"}'
grep -q "not both" "$TEST_DIR/stdout" || fail "two requests were accepted"
grep -q "23:59" "$TEST_DIR/stdout" ||
	{ cat "$TEST_DIR/stdout" >&2; fail "the refusal does not say what it was given"; }

# Neither: refused with what to supply.
run_fyai tool wait '{"name":"nothing"}'
grep -q "seconds or until" "$TEST_DIR/stdout" || fail "a wait with no time was accepted"

# An unreadable time says what it could not read.
run_fyai tool wait '{"until":"soon"}'
grep -q "HH:MM" "$TEST_DIR/stdout" || fail "an unreadable time was not explained"
grep -q "soon" "$TEST_DIR/stdout" || fail "the refusal does not quote the time"

# A valid prefix is not the complete documented format.
run_fyai tool wait '{"until":"00:00junk"}'
grep -q "HH:MM" "$TEST_DIR/stdout" || fail "trailing time text was accepted"

# mktime normalizes this into March; the tool must not change the request.
run_fyai tool wait '{"until":"2099-02-30T12:00:00"}'
grep -q "HH:MM" "$TEST_DIR/stdout" || fail "an impossible date was normalized"

pass
