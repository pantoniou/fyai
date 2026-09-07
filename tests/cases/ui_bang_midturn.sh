#!/bin/bash
# SPDX-License-Identifier: MIT
# A bang line typed while a model turn is in flight starts its user shell
# beside the turn: the turnieres stalled answer still arrives on the first
# provider, and interrupting the turn afterwards stops the turn without
# taking the user shell down with it. The follow-up prompt then runs
# normally, proving the session survived both.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start bang_midturn.json

cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: foo
    provider_model_id: bar
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret

# The bang shell announces itself while the first turn stalls; one Ctrl-T
# hands the keys back to the prompt, and Ctrl-C then interrupts the turn.
# /sessions must still list the user shell afterwards.
# The needle is the expanded home path: the typed echo holds only the
# literal $HOME, so it cannot trip the wait before the shell runs.
FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="shell-home-is-/" \
FYAI_PTY_DURING_INPUT="!sh -c 'echo shell-home-is-\$HOME; sleep 30'" \
FYAI_PTY_DURING_DELAY=0.5 \
FYAI_PTY_AFTER="raw:14|drain:.5|raw:03|wait:interrupted|send:/sessions|wait:bang-1|send:second question|wait:The follow-up answer." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" --color off \
    --set display/markdown=false --set display/stream=false \
    --set tools=false --set builtin_shell=false -i -m foo

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "the mid-turn bang disturbed the turn or died with it"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# The shell ran beside the running turn, before the interrupt.
ready = plain.find(b"shell-home-is-/")
landed = plain.find(b"interrupted")
if ready < 0:
    raise SystemExit("the user shell never ran")
if landed < 0:
    raise SystemExit("the interrupt never landed on the turn")
if ready > landed:
    raise SystemExit("the bang waited behind the turn")
# The interrupt stopped the turn, not the user shell: /sessions still
# lists it afterwards.
if plain.find(b"bang-1", landed) < 0:
    raise SystemExit("the user shell did not survive the interrupt")
if b"The follow-up answer." not in plain:
    raise SystemExit("the session did not survive the bang and interrupt")
EOF

# The interrupted turn reached the first provider; the follow-up
# after the interrupt ran there too.
mock_stop 2
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == "Bearer mock-secret"'
assert_request 1 'r["path"] == "/v1/chat/completions"'
assert_request 1 'any(m.get("role") == "user" and m.get("content") == "second question" for m in r["body"]["messages"])'

pass
