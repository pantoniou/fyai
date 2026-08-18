#!/bin/bash
# SPDX-License-Identifier: MIT
# The default attempt budget is a promise to the user, not an accident. A
# token-per-minute limit can hold a request for most of a minute, so the
# default has to be generous enough to outlast one. Only the delays are made
# small here: the count is what is under test.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_default_attempts.json

run_fyai --set api=chat-completions --set tools=false \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=5 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status_nonzero

# Eight attempts: the first one and seven retries.
mock_stop 8
pass
