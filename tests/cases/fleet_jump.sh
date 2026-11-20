#!/bin/bash
# SPDX-License-Identifier: MIT
# The root session is a switchboard: /jump goes into a session of the fleet,
# which the terminal controls, and /root goes back to the root session, which
# went on at its server meanwhile. A project alone names its main session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_jump.json
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
mock_watch
run_fyai fleet start --branch fleet/work a Do the work
assert_status 0
wait_requests 1

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i 8<&- <<'PTY'
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - type: /jump a:fleet/work
  - wait-screen: "Work done."
    timeout: session
  - wait-screen: "jumped from .:main"
    timeout: session
  - type: Member line
  - wait-screen: "Member reply."
    timeout: session
  - type: /root
  - wait-screen: "All done."
    timeout: session
  - type: Root again
  - wait-screen: "Root reply."
    timeout: session
  - type: /jump a
  - wait-screen: "jumped from .:main"
    timeout: session
PTY
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the turns ran in the wrong sessions"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
def users(r):
    return [m["content"] for m in r["body"]["messages"] if m["role"] == "user"]
member = [r for r in reqs if "Member line" in users(r)]
root = [r for r in reqs if "Root again" in users(r)]
assert len(member) == 1 and users(member[0]) == ["Do the work", "Member line"], users(member[0])
assert len(root) == 1 and users(root[0]) == ["Say done", "Root again"], users(root[0])
PY
pass
