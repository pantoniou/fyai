#!/bin/bash
# SPDX-License-Identifier: MIT
# An explicit --config file is an ingestion point: invalid types are rejected
# there, as they already are for --set, instead of reaching the effective
# configuration unreported.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

BAD="$TEST_DIR/bad.yaml"
cat >"$BAD" <<'YAML'
tools: nope
max_tokens: nope
retry: nope
YAML

run_fyai --config "$BAD" --transient config effective
assert_status_nonzero
assert_stderr_contains "schema validation failed"
assert_stdout_not_contains "tools: nope"

# --set is refused the same way, and says which key is wrong.
run_fyai --transient --set tools=nope config effective
assert_status_nonzero
assert_stderr_contains "tools"

# A valid file is still accepted.
GOOD="$TEST_DIR/good.yaml"
printf 'tools: true\nmax_tokens: 256\n' >"$GOOD"
run_fyai --config "$GOOD" --transient config effective
assert_status 0
assert_stdout_contains "max_tokens: 256"

pass
