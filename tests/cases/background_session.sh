#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that /background hands off an in-flight model request to a temporary
# server, which commits the completed turn and exits.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_background.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
printf 'one line\n' >data.txt

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Start the long job
  - wait-file: "${TEST_DIR}/requests.jsonl"
    contains: Start the long job
  - type: /background
  - wait: "continues in the background"
    timeout: session
  - touch: "${TEST_DIR}/release-background"
PTY

# Wait for the server to finish the second request and shut down.
log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
[ -n "$pid" ] || fail "no server started: $(cat "$log" 2>/dev/null)"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop with the turn"
[ ! -e .fyai/unix/remote.sock ] || fail "the server left its socket"
mock_stop 2
"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export"
grep -q 'Finished by the server.' saved.md || fail "the server did not finish the turn"
pass
