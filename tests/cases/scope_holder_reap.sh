#!/bin/bash
# SPDX-License-Identifier: MIT
# The holder that a tool call started ends after its grace time: the worker
# reaps it then, and an idle worker keeps no zombie child.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
mock_start scope_holder_reap.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false --set tools=true
	    --set sandbox_profile=dev --set sandbox_cover=auto --set sandbox_scope=exchange
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mock_watch
pty_run "$TEST_DIR/pty0.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Hello root
  - wait-screen: "Hi."
    timeout: session
PTY
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" --split resume --last <<PTY &
steps:
  - type: Run it
  - wait-screen: "Finished."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-file: "$SYNC/go"
    contains: go
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true
fyai_test_on_exit 'echo go >"$SYNC/go"'
# worker_pid: the worker of the session, a child of the terminal client.
client="$(pgrep -f -n -- "$FYAI_BIN --split resume --last")"
worker="$(pgrep -n -P "$client")"
[ -n "$worker" ] || fail "no worker"
"$PYTHON" -I - "$worker" "$((30 * FYAI_TIMEOUT_SCALE))" <<'PY' ||
import subprocess, sys, time

worker, deadline = sys.argv[1], time.monotonic() + float(sys.argv[2])
while time.monotonic() < deadline:
    out = subprocess.run(["ps", "-o", "stat=", "--ppid", worker],
                         capture_output=True, text=True).stdout
    if not out.strip():
        sys.exit(0)
    time.sleep(0.05)
sys.exit(1)
PY
	fail "the worker keeps a child: $(ps -o pid,stat,cmd --ppid "$worker")"
echo go >"$SYNC/go"
wait "$driver" || fail "the session failed"
pass
