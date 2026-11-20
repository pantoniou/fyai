#!/bin/bash
# SPDX-License-Identifier: MIT
# A server `--once` of the root serves its own project only and refuses the
# directory of a member. A message to a stored member then starts a server of
# the member project.
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
mock_watch
(cd a && "$FYAI_BIN" "${set_common[@]}" -b fleet/work "Do the work" >"$TEST_DIR/m.out" 2>&1) ||
	fail "cannot make the member: $(cat "$TEST_DIR/m.out")"
wait_requests 1
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true; (cd a && "$FYAI_BIN" serve stop >/dev/null 2>&1) || true'
"$FYAI_BIN" serve --daemon --once >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the server of the root did not start: $(cat "$TEST_DIR/serve.out")"
run_fyai session send a:fleet/work Member line
assert_status 0
wait_requests 2
assert_request 1 '"Member line" in json.dumps(r["body"])'
pass
