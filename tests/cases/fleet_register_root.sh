#!/bin/bash
# SPDX-License-Identifier: MIT
# A session in a project with no server of its own registers with the server
# of the root above it, which starts the server of the project. That server
# keeps the record of the project, so `session list` and `fleet list` show the
# session, and `session kill` in the project reaches it through the root.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_park.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"

"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
(cd a && pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i) <<PTY &
exit: {status: 1}
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait: "the session was ended from outside it"
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true

[ ! -S a/.fyai/unix/remote.sock ] || fail "the project has a socket of its own"
(cd a && "$FYAI_BIN" session list --output json) >"$TEST_DIR/list.json"
"$PYTHON" - "$TEST_DIR/list.json" <<'PY' || fail "session list: $(cat "$TEST_DIR/list.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['branch'], r['state'], r['attached']) for r in rows] == [('main', 'idle', True)], rows
PY
"$FYAI_BIN" fleet list --output json >"$TEST_DIR/fleet.json"
"$PYTHON" - "$TEST_DIR/fleet.json" <<'PY' || fail "fleet list: $(cat "$TEST_DIR/fleet.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
got = [(r['state'], r['attached']) for r in rows if r['project'] == 'a' and r['branch'] == 'main']
assert got == [('idle', True)], rows
PY

(cd a && "$FYAI_BIN" session kill main) >"$TEST_DIR/kill.out" 2>&1 ||
	fail "session kill did not reach the session: $(cat "$TEST_DIR/kill.out")"
wait "$driver" || fail "the client did not see its session end: $(tail -c 2000 "$TEST_DIR/pty.out")"
grep -q "registered the worker" .fyai/logs/serve.log ||
	fail "the server of the project did not register the session: $(cat .fyai/logs/serve.log)"
pass
