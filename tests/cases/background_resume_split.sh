#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that `fyai resume` on a branch matching HEAD attaches to the existing
# live server session instead of spawning a second worker.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_park.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - type: /background
  - wait: "is parked at a server"
    timeout: session
PTY

pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" resume <<'PTY'
ready: toggle foreign sessions
steps:
  - wait-screen: "[live · idle]"
    timeout: session
  - write: "\r"
  - wait-gone: "toggle foreign sessions"
  - wait-count: "\x1b[>1u"
    count: 2
    timeout: session
PTY
grep -q 'client 2: attached to the worker' .fyai/logs/serve.log ||
    fail "the picker did not join the live session"

# `resume --last` and `resume NAME` must also attach without spawning a worker.
for words in "resume --last" "resume main"; do
	pty_run "$TEST_DIR/pty3.out" "$FYAI_BIN" $words <<'PTY'
steps:
  - wait-count: "\x1b[>1u"
    count: 1
    timeout: session
PTY
done
[ "$(grep -c 'attached to the worker' .fyai/logs/serve.log)" -eq 3 ] ||
    fail "resume --last or resume main did not join: $(cat .fyai/logs/serve.log)"
[ "$(grep -c 'started the worker' .fyai/logs/serve.log)" -eq 0 ] ||
    fail "a second worker started"
"$FYAI_BIN" serve stop >/dev/null 2>&1 || true
pass
