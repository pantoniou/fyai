#!/bin/bash
# SPDX-License-Identifier: MIT
# --split runs the interactive session at a worker that is a child of this
# process, with no server between: a question is answered, the worker commits
# the conversation, and a second session on the branch shows it and goes on.
# Without a terminal the option is refused.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start claude_sdk_branch.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	--set model=mock-model \
	--set display/screen=fullscreen --set display/markdown=true \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$FYAI_BIN" branch create split/ui >/dev/null 2>&1

run_fyai --split -b split/ui
[ "$FYAI_STATUS" -ne 0 ] || fail "--split without a terminal succeeded"
grep -q 'interactive session' "$TEST_DIR/stderr" ||
	fail "--split without a terminal did not say why"

pty_run "$TEST_DIR/first.out" "$FYAI_BIN" --split -b split/ui <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: First split question.
  - wait-screen: First answer.
  - type: /quit
PTY
pty_run "$TEST_DIR/second.out" "$FYAI_BIN" --split -b split/ui <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: First answer.
  - type: Continue the split conversation.
  - wait-screen: Second answer.
  - type: /quit
PTY
mock_stop 2
assert_request 0 'any("First split question." in str(m.get("content", "")) for m in r["body"]["messages"])'
pass
