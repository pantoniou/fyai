#!/bin/bash
# SPDX-License-Identifier: MIT
# The client of a root session shows the fleet panel from the start, `fleet
# panel off` hides it and `fleet panel on` shows it again.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_panel.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - wait-screen: "no members"
    timeout: session
  - type: /fleet panel off
  - wait-screen: "panel: off"
  - wait-gone: "no members"
  - type: /fleet panel on
  - wait-screen: "panel: on"
  - wait-screen: "no members"
PTY

pass
