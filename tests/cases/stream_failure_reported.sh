#!/bin/bash
# SPDX-License-Identifier: MIT
# A 2xx stream that never completes must say why. The provider puts the reason
# on the failure event; dropping it left the engine reporting a bare "request
# failed", which is what these three shapes pin down.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# response.incomplete: the reason names the limit that stopped it.
mock_start stream_incomplete.json
run_fyai -u "$MOCK_URL/v1/responses" -m mock-model "hi"
assert_status 1
assert_stderr_contains "max_output_tokens"
assert_stderr_contains "response.incomplete"
# the cause is reported, not the engine's generic fallback behind it
assert_stderr_not_contains "request failed"
mock_stop 1

# response.failed: the provider's own error message.
mock_start stream_failed.json
run_fyai -u "$MOCK_URL/v1/responses" -m mock-model "hi"
assert_status 1
assert_stderr_contains "the model stumbled"
assert_stderr_not_contains "request failed"
mock_stop 1

# A stream that just stops: no event to explain it, so fyai must.
mock_start stream_cut.json
run_fyai -u "$MOCK_URL/v1/responses" -m mock-model "hi"
assert_status 1
assert_stderr_contains "ended before it completed"
assert_stderr_not_contains "request failed"
mock_stop 1

pass
