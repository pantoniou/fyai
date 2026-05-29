#!/bin/bash
# SPDX-License-Identifier: MIT
# A response.failed SSE event must fail the run cleanly.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start response_failed.json
run_fyai --responses -u "$MOCK_URL/v1/responses" -m mock-model "fail please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
mock_stop 1

pass
