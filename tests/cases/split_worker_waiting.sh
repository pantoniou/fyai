#!/bin/bash
# SPDX-License-Identifier: MIT
# The worker of a split session ends with its client while the model has it
# wait. The model waits an hour with no name, the client is killed, and the
# worker, which nobody can answer now, ends.
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

if ! "$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$worker" $((30 * FYAI_TIMEOUT_SCALE)); then
	kill -KILL "$worker" 2>/dev/null || true
	printf go >"$TEST_DIR/go"
	wait "$DRIVER" || true
	fail "the worker did not end with its client"
fi
printf go >"$TEST_DIR/go"
wait "$DRIVER" || true
pass
