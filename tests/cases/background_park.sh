#!/bin/bash
# SPDX-License-Identifier: MIT
# /background with no turn in flight parks the session at a server, which
# keeps it until `serve stop`.
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

record=.fyai/unix/remote.sock.live
"$PYTHON" - "$record" <<'PY' || fail "the parked session is not in the record"
import json, sys
s = json.load(open(sys.argv[1]))['sessions']
assert [(x['branch'], x['busy'], x['attached']) for x in s] == [('main', False, False)], s
PY
log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
kill -0 "$pid" || fail "the server stopped with no turn"
"$FYAI_BIN" serve stop >/dev/null 2>&1 || fail "serve stop failed"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop"
pass
