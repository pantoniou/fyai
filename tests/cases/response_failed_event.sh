#!/bin/bash
# SPDX-License-Identifier: MIT
# A response.failed SSE event must fail the run cleanly. Retry is off: what is
# under test is that the run settles, not how often a passing condition is
# tried again. See the retry_* cases for that.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start response_failed.json
run_fyai --set api=responses --set retry/max_attempts=1 \
	--set api_url="$MOCK_URL/v1/responses" -m mock-model "fail please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
mock_stop 1

pass
