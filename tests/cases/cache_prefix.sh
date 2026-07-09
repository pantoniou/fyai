#!/bin/bash
# SPDX-License-Identifier: MIT
# Prompt-cache hit path: providers cache on an exact prefix match of the
# request, so the history fyai reconstructs from the arena must be
# byte-stable - every request's message list must extend the previous
# request's list without altering a single element, across the tool loop and
# across separate invocations. Verified for all three APIs, along with the
# provider-reported cache counters flowing into --stats.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# check_prefix <prev-idx> <next-idx> <body key>: request next's message array
# must be request prev's array plus a non-empty appended tail.
check_prefix() {
	"$PYTHON" - "$TEST_DIR/requests.jsonl" "$1" "$2" "$3" <<'EOF' || fail "cache prefix broken: request $1 -> $2 ($3)"
import json, sys
path, prev, nxt, key = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
reqs = [json.loads(l) for l in open(path)]

def strip(v):
    # Anthropic ignores cache_control markers when matching the cached
    # prefix; the moving breakpoint must not count as a prefix change.
    if isinstance(v, dict):
        return {k: strip(x) for k, x in v.items() if k != "cache_control"}
    if isinstance(v, list):
        return [strip(x) for x in v]
    return v

a, b = strip(reqs[prev]["body"][key]), strip(reqs[nxt]["body"][key])
if b[:len(a)] != a:
    sys.exit("prefix mismatch:\nprev %s\nnext %s" %
             (json.dumps(a, indent=1), json.dumps(b[:len(a)], indent=1)))
if len(b) <= len(a):
    sys.exit("no appended tail")
EOF
}

printf 'mock data payload\n' > data.txt

# --- Chat Completions ---
mock_start cache_chat.json
run_fyai --set api=chat-completions --no-stream -t \
	 -u "$MOCK_URL/v1/chat/completions" -m mock-model --stats "read data.txt"
assert_status 0
run_fyai --set api=chat-completions --no-stream -t \
	 -u "$MOCK_URL/v1/chat/completions" -m mock-model --stats "and again"
assert_status 0
check_prefix 0 1 messages	# within the tool loop
check_prefix 1 2 messages	# across invocations (arena reconstruction)
assert_stderr_contains "cached=64"
mock_stop 3

# --- Responses ---
mock_start cache_responses.json
run_fyai --set api=responses --no-stream -t -u "$MOCK_URL/v1/responses" \
	 -m mock-model --new --stats "read data.txt"
assert_status 0
run_fyai --set api=responses --no-stream -t -u "$MOCK_URL/v1/responses" \
	 -m mock-model --stats "and again"
assert_status 0
check_prefix 0 1 input
check_prefix 1 2 input
# the instructions field must be stable across invocations too
assert_request 2 'r["body"]["instructions"] == "You are a concise assistant."'
assert_stderr_contains "cached=64"
mock_stop 3

# --- Anthropic Messages ---
mock_start cache_messages.json
run_fyai --set api=messages --no-stream -t -u "$MOCK_URL/v1/messages" \
	 -m mock-model --new --stats "read data.txt"
assert_status 0
run_fyai --set api=messages --no-stream -t -u "$MOCK_URL/v1/messages" \
	 -m mock-model --stats "and again"
assert_status 0
check_prefix 0 1 messages
check_prefix 1 2 messages
assert_request 2 'r["body"]["system"][0]["text"] == "You are a concise assistant."'
# exactly two breakpoints: the system block and the history's last block
assert_request 2 'r["body"]["system"][0]["cache_control"] == {"type": "ephemeral"}'
assert_request 2 'r["body"]["messages"][-1]["content"][-1]["cache_control"] == {"type": "ephemeral"}'
assert_request 2 'sum(1 for m in r["body"]["messages"] for b in m["content"] if isinstance(b, dict) and "cache_control" in b) == 1'
assert_stderr_contains "cached=64"
mock_stop 3

pass
