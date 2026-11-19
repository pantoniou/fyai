#!/bin/bash
# SPDX-License-Identifier: MIT
# A session that started before any server registers when a server starts. A
# server that stops leaves the session running, and the next server lists it
# again.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
# A session finds a server that starts later by watching the directory of the
# sockets, which this host does only on Linux (inotify).
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
  # The word is in two parts, so that only the output holds it, and not the line
  # that is typed: Esc must come after the tile that the command opens.
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

# serve_pid LOG: the server that writes LOG.
serve_pid() {
	sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$1" | head -1
}
# listed: the server lists main, driven by its own client.
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

# No server runs: the session stands alone. Then one starts.
read -r _ <"$SYNC/idle" || true
[ ! -e .fyai/unix/remote.sock ] || fail "a server runs before the case starts one"
"$FYAI_BIN" serve --daemon --log "$LOG1" >"$TEST_DIR/serve1.out" 2>&1 ||
	fail "the server did not start: $(cat "$TEST_DIR/serve1.out")"
read -r _ <"$SYNC/listed" || true
listed

# The server stops; the session goes on, and the next server lists it.
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
