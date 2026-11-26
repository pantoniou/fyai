#!/bin/bash
# SPDX-License-Identifier: MIT
# Run a decision model from the session before the first chat turn.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start decide_session.yaml
TYPESAFE_API_KEY=ts-key pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -i -k test-key -m mock-model \
	--set display/stream=false --set api=chat-completions --set "api_url=$MOCK_URL/v1/chat/completions" \
	--set "decisions/gate={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', options: {safe: {description: Safe}, risky: {description: Risky}}}" <<'PTY'
steps:
  - type: /decide --list
  - wait-screen: gate
  - type: /decide gate
  - wait-screen: give the text to decide on
  - type: /decide yes-no Is 17 a prime number?
  - wait-screen: only two divisors
  - type: /decide gate --input ]
  - wait-screen: invalid JSON input
  - type: /decide gate --input '{"text":"K1"}'
  - wait-screen: '0.93'
  - type: K2 hello
  - wait-screen: No deciders here.
  - type: /quit
PTY
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'CHECK' || fail "session requests"
import json, sys
canned, decision, chat = map(json.loads, open(sys.argv[1]))
assert canned["path"] == "/v1/chat/completions"
assert "Is 17 a prime number?" in canned["body"]["messages"][-1]["content"]
assert decision["path"] == "/v1/systemone"
assert decision["body"]["state"] == {"text": "K1"}
assert [m["content"] for m in chat["body"]["messages"] if m["role"] == "user"] == ["K2 hello"]
CHECK
mock_stop 3
pass
