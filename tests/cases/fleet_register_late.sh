#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify late server registration and re-registration across server restarts:
# a session started with no server registers once one starts, survives server
# shutdown, and registers again when a new server starts.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
# Late registration watches the socket directory via Linux inotify.
[ "$(uname -s)" = Linux ] || skip "no directory watch on this host"
mock_start remote_park.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/idle" "$SYNC/listed" "$SYNC/alive"
LOG1="$SYNC/serve1.log"
LOG2="$SYNC/serve2.log"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - touch: "$SYNC/idle"
    timeout: session
  - wait-file: "$LOG1"
    contains: registered the worker
    timeout: session
  - touch: "$SYNC/listed"
    timeout: session
  - wait-file: "$SYNC/stopped"
    equals: ok
    timeout: session
  # Split the token across args so it appears only in command output, not the
  # echoed input line; Esc must fire after the command tile opens.
  - type: "!printf '%s%s' still- $$-here"
  - wait-screen: "still-$$-here"
    timeout: session
  - write: "\e"
  - wait-gone: "Esc closes"
  - wait-file: "$LOG2"
    contains: registered the worker
    timeout: session
  - touch: "$SYNC/alive"
    timeout: session
  - wait-file: "$SYNC/checked"
    equals: ok
    timeout: session
  - type: /exit
PTY
driver=$!

# serve_pid LOG: extract the server PID from LOG.
serve_pid() {
	sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$1" | head -1
}
# listed: assert `serve status` lists `main` as locally driven.
listed() {
	"$FYAI_BIN" serve status --output json >"$TEST_DIR/status.json" ||
		fail "serve status failed"
	"$PYTHON" - "$TEST_DIR/status.json" <<'PY' ||
import json, sys
s = json.load(open(sys.argv[1]))['sessions']
assert [(x['branch'], x['local']) for x in s] == [('main', True)], s
PY
		fail "the server does not list the session: $(cat "$TEST_DIR/status.json")"
}

# Session starts standalone; start a server and confirm late registration.
read -r _ <"$SYNC/idle" || true
[ ! -e .fyai/unix/remote.sock ] || fail "a server runs before the case starts one"
"$FYAI_BIN" serve --daemon --log "$LOG1" >"$TEST_DIR/serve1.out" 2>&1 ||
	fail "the server did not start: $(cat "$TEST_DIR/serve1.out")"
read -r _ <"$SYNC/listed" || true
listed

# Stop the server; session continues running and registers with the next server.
pid=$(serve_pid "$LOG1")
"$FYAI_BIN" serve stop >/dev/null 2>&1 || fail "serve stop failed"
"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$pid" $((100 * FYAI_TIMEOUT_SCALE)) ||
	fail "the server did not stop"
echo ok >"$SYNC/stopped"
"$FYAI_BIN" serve --daemon --log "$LOG2" >"$TEST_DIR/serve2.out" 2>&1 ||
	fail "the second server did not start: $(cat "$TEST_DIR/serve2.out")"
read -r _ <"$SYNC/alive" || true
listed
echo ok >"$SYNC/checked"
wait "$driver" || fail "the session failed: $(tail -c 2000 "$TEST_DIR/pty.out")"
pass
