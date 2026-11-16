#!/bin/bash
# SPDX-License-Identifier: MIT
# A split session completes the branches of its worker. The client has no
# arena: the word that is typed asks the worker, and the answer is there for
# the keys that follow, which the client filters.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	--set model=mock-model --set display/completion=auto \
	--set display/screen=fullscreen --set display/markdown=true \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
for n in alpha bravo; do
	"$FYAI_BIN" branch create "split/$n" >/dev/null 2>&1
done
"$FYAI_BIN" branch create split/ui >/dev/null 2>&1

pty_run "$TEST_DIR/complete.out" "$FYAI_BIN" --split -b split/ui <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: mock-model
  - write: "/checkout split/"
  - wait-screen: "/checkout split/"
  - write: "a"
  - wait-screen: "/checkout split/a"
  - write: "\x7f"
  - wait-gone: "/checkout split/a"
  - write: "a"
  - wait-screen: "split/alpha"
  - write: "\x7f"
  - write: "b"
  - wait-gone: "split/alpha"
  - wait-screen: "split/bravo"
  - write: "\e"
  - write: "\x15"
PTY
pass
