#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that `fleet start` reaches a member project that no server serves:
# a root session on a terminal has no server, and the server of a session
# in the background serves once and only its own project. The member gets a
# server of its own, and the session runs its task there.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
# The member takes its key from the environment of this case. Under credential
# isolation no worker holds that value, and the member server has no key.
[ -z "${FYAI_TEST_TRANSPORT:-}" ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
printf '%s\n' '{"steps": [{"repeat": true, "match": {"path": "/v1/chat/completions"}, "response": {"id": "x", "object": "chat.completion", "model": "mock-model", "choices": [{"index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": "Said hello."}}]}}]}' \
	>"$TEST_DIR/reply.json"
mock_start "$TEST_DIR/reply.json"
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1 ||
	fail "cannot configure the root"
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
mkdir -p py-hello
echo 'print("hello")' >py-hello/main.py
"$FYAI_BIN" fleet create py-hello >"$TEST_DIR/create.out" 2>&1 ||
	fail "fleet create failed: $(cat "$TEST_DIR/create.out")"
[ -d py-hello/.fyai/arena ] || fail "fleet create made no project"
fyai_test_on_exit '(cd py-hello && "$FYAI_BIN" serve stop >/dev/null 2>&1) || true'
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

# A root session on a terminal runs at a worker with no server: the member
# gets one.
mock_watch
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" --set display/markdown=true -i <<'PTY'
steps:
  - type: /fleet start py-hello say hello
  - wait-screen: "started py-hello:"
    timeout: session
PTY
[ -S py-hello/.fyai/unix/remote.sock ] ||
	fail "the session gave the member no server"
wait_requests 1
(cd py-hello && "$FYAI_BIN" serve stop >/dev/null 2>&1) ||
	fail "cannot stop the server of the member"

# The root runs a server that serves once, as a session that went to the
# background does.
"$FYAI_BIN" serve --daemon --once >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"

mock_watch
"$FYAI_BIN" fleet start py-hello say hello >"$TEST_DIR/start.out" 2>&1 ||
	fail "fleet start failed: $(cat "$TEST_DIR/start.out")"
grep -q "started py-hello:" "$TEST_DIR/start.out" ||
	fail "fleet start said: $(cat "$TEST_DIR/start.out")"
[ -S py-hello/.fyai/unix/remote.sock ] ||
	fail "the member has no server of its own"
wait_requests 1
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the member did not run its task"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
assert any(m.get("role") == "user" and m.get("content") == "say hello"
           for r in reqs for m in r["body"]["messages"]), reqs
PY
pass
