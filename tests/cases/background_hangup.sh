#!/bin/bash
# SPDX-License-Identifier: MIT
# The terminal client dies while a tool runs: the worker gives itself to a
# server and goes on, and the server stores the end of the turn.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start background_tool.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set tools=true \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
mkfifo tool.fifo

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  signal: KILL
steps:
  - type: Run the held tool
  - wait-screen: "tool.fifo"
    timeout: session
  - signal: KILL
PTY

# The worker gives itself to a server when it sees its client go: wait until
# the record of the server has the session, running.
"$PYTHON" - "$TEST_DIR/.fyai/unix/remote.sock.live" "$FYAI_TIMEOUT_SCALE" <<'PY' || fail "the session did not reach a server"
import json, select, sys, time
record, scale = sys.argv[1], int(sys.argv[2])
def rows():
    try:
        return [(r['branch'], r['busy']) for r in json.load(open(record))['sessions']]
    except (OSError, ValueError, KeyError):
        return []
deadline = time.monotonic() + 30 * scale
while rows() != [('main', True)]:
    assert time.monotonic() < deadline, rows()
    select.select([], [], [], 0.05)
PY
run_fyai session list --output json
assert_status 0
grep -q '"state": "running"' "$TEST_DIR/stdout" || fail "session list: $(cat "$TEST_DIR/stdout")"
exec 9<>tool.fifo
exec 9>&-
log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop with the turn"
mock_stop 2
"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export"
grep -q 'Tool done at the server.' saved.md || fail "the turn did not end at the server"
pass
