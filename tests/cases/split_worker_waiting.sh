#!/bin/bash
# SPDX-License-Identifier: MIT
# A worker that the model has wait goes on at a server when its client is lost.
# The model waits an hour with no name, the client is killed, and the session
# is found at the server, where `session kill` ends it.
set -eu
# This test inspects Linux /proc.
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
rm -f "$TEST_DIR/go"
mock_start wait_plain_escape.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false --set tools=true \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set display/screen=fullscreen --set display/markdown=true \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$FYAI_BIN" branch create split/waiting >/dev/null 2>&1

mock_watch
# The session holds on the first reply, and on the file that this case makes.
(
	pty_run "$TEST_DIR/split.out" "$FYAI_BIN" --split -b split/waiting <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: mock-model
  - type: go to sleep
  - wait-file: "${TEST_DIR}/go"
    equals: "go"
PTY
) 8<&- &
DRIVER=$!

# The request is in flight once the mock has recorded it.
wait_requests 1

# The worker is the process of this run that has the binary of the worker and
# this directory; its parent is the client.
worker=
for p in $(pgrep -f 'claude-sdk --remote-worker'); do
	[ "$(readlink "/proc/$p/cwd" 2>/dev/null)" = "$TEST_DIR" ] && worker=$p
done
[ -n "$worker" ] || fail "no worker of this run"
client="$(awk '{print $4}' "/proc/$worker/stat")"
kill -KILL "$client"

# The worker outlives its client: it is at a server, in the state it was in.
"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$client" $((30 * FYAI_TIMEOUT_SCALE)) ||
	fail "the client did not end"
state=
for i in $(seq 1 600); do
	run_fyai session list --output json
	state="$("$PYTHON" -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r[0]["state"] if r else "")' "$TEST_DIR/stdout")"
	[ -z "$state" ] || break
	"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$worker" 0 && break
done
[ "$state" = running ] || {
	kill -KILL "$worker" 2>/dev/null || true
	printf go >"$TEST_DIR/go"
	wait "$DRIVER" || true
	fail "the session is not running at a server: '$state'"
}
run_fyai session kill split/waiting
assert_status 0
"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$worker" $((30 * FYAI_TIMEOUT_SCALE)) ||
	fail "session kill did not end the worker"
printf go >"$TEST_DIR/go"
wait "$DRIVER" || true
pass
