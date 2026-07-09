#!/bin/bash
# SPDX-License-Identifier: MIT
# token_extents fail-soft: a provider that 400s the injected logprobs params
# latches the session off and the step retries once without them; the retried
# stream still records chunk extents.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tokens_failsoft.json

run_fyai --set api=chat-completions -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model --token-extents "stream please"
assert_status 0
assert_stdout_contains "Fallback answer."

# first request injected logprobs, the retry dropped them
assert_request 0 'r["body"]["logprobs"] is True'
assert_request 1 '"logprobs" not in r["body"]'

# the retried stream carried no logprob entries -> chunk extents
assert_state_contains "tokens:" dump anchors
assert_state_contains "text: 'Fallback '" dump anchors
assert_state_contains "pos: 9" dump anchors

mock_stop 2
pass
