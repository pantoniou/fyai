#!/bin/bash
# SPDX-License-Identifier: MIT
# The browser of a split session shows the end of the selected session beside
# the list. The worker has the turns and the client has the renderer: it asks
# for the last turns of the branch and renders them itself.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_basic.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=responses --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/responses" --set model=mock-model \
	--set display/markdown=true --set display/branch_preview=right \
	--set display/branch_preview_width=48 \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$FYAI_BIN" branch create split/p >/dev/null 2>&1

# A session that has a line of its own.
pty_run "$TEST_DIR/first.out" "$FYAI_BIN" --split -b split/q --theme dark <<'PTY'
rows: 40
cols: 140
steps:
  - wait-screen: mock-model
  - type: unique-question-zzz
  - wait-screen: Hello from the mock responses provider.
  - type: /quit
PTY

# Another session selects it in the browser: its line is on the screen only in
# the preview, which the client renders from the turns that the worker gave.
pty_run "$TEST_DIR/preview.out" "$FYAI_BIN" --split -b split/p --theme dark <<'PTY'
rows: 40
cols: 140
steps:
  - wait-screen: mock-model
  - type: "/branches"
  - wait-screen: "- q"
  - write: "j"
  - wait-screen: unique-question-zzz
  - write: "\e"
  - wait-gone: Esc close
  - type: /quit
PTY
mock_stop 1
pass
