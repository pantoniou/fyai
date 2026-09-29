#!/bin/bash
# SPDX-License-Identifier: MIT
# The log verb knows the transport log: it shows it, turns it on and off,
# clears it, and takes the setting from the configuration.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai log show
assert_status 0
assert_stdout_contains "transport off"

run_fyai log transport start
assert_status 0
assert_stdout_contains "transport on"
assert_stdout_contains "wire off"

run_fyai log all start
assert_status 0
assert_stdout_contains "transport on"

run_fyai --set logging/transport true log show
assert_status 0
assert_stdout_contains "transport on"

mkdir -p .fyai/logs
printf -- '---\nkind: transport\n' > .fyai/logs/transport.yaml
run_fyai log transport clear
assert_status 0
test -e .fyai/logs/transport.yaml || fail "the transport log is gone"
test ! -s .fyai/logs/transport.yaml || fail "the transport log was not emptied"

run_fyai log nothing start
[ "$FYAI_STATUS" -ne 0 ] || fail "an unknown target was accepted"
