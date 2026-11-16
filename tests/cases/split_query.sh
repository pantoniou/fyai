#!/bin/bash
# SPDX-License-Identifier: MIT
# A split session asks its worker for rows of the arena that the client does
# not have. `/query` runs in the client, sends `fyai_query`, and the command
# ends with the answer of the worker: a page of rows, or the error that the
# worker gave for a cursor that it did not make.
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
for n in alpha bravo charlie; do
	"$FYAI_BIN" branch create "split/$n" >/dev/null 2>&1
done
"$FYAI_BIN" branch create split/ui >/dev/null 2>&1

pty_run "$TEST_DIR/query.out" "$FYAI_BIN" --split -b split/ui <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: /query branches --all --limit 1
  - wait-screen: "what: branches"
  - wait-screen: "branch: split/ui"
  - type: /query branches --after garbage
  - wait-screen: "bad_request"
  - type: /quit
PTY
pass
