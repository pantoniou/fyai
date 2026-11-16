#!/bin/bash
# SPDX-License-Identifier: MIT
# A split session with no branch starts as a local one does: on a `session/`
# branch, with the configuration of the branch last used in this directory. A
# worker of a server keeps the `sdk/` name for such a session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start claude_sdk_branch.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set display/screen=fullscreen --set display/markdown=true \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
# The branch last used in this directory has a configuration of its own.
"$FYAI_BIN" branch create split/last >/dev/null 2>&1
"$FYAI_BIN" -b split/last --set temperature=0.5 >/dev/null 2>&1

pty_run "$TEST_DIR/session.out" "$FYAI_BIN" --split <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: "fyai: session/"
  - wait-screen: mock-model
  - wait-screen: "temp 0.5"
  - type: First split question.
  - wait-screen: First answer.
  - type: /quit
PTY
mock_stop 1

# The session is stored under its name, and the branch that it started from is
# not changed.
run_fyai branch list --output json
assert_status 0
grep -q '"session/' "$TEST_DIR/stdout" || fail "no session/ branch: $(cat "$TEST_DIR/stdout")"
! grep -q '"sdk/' "$TEST_DIR/stdout" || fail "an sdk/ branch: $(cat "$TEST_DIR/stdout")"
pass
