#!/bin/bash
# SPDX-License-Identifier: MIT
# Slash commands typed while a model turn is in flight: a read-only slash
# (/status) runs beside the turn, while a mutating slash (/model baz) stays
# queued and runs only once the turn is done. The turn itself is unaffected
# in both cases: its stalled answer still arrives on the first provider.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start slash_midturn.json

# Two providers, both speaking chat_completions at the mock, told apart by
# endpoint path, wire id and api-key env var.
cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
- name: baz
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
- name: otherprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v2/chat/completions
  models:
  - canonical_id: baz
    provider_model_id: qux
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret
export OTHERPROV_API_KEY=other-secret

# Leg 1: /status typed while the first turn stalls runs beside it: its
# report reaches the screen before the stalled answer does.
FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="The stalled answer." \
FYAI_PTY_DURING_INPUT="/status" \
FYAI_PTY_DURING_DELAY=0.5 \
FYAI_PTY_AFTER="send:second question|wait:The follow-up answer." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/immediate.out" \
    "$FYAI_BIN" --color off \
    --set display/markdown=false --set display/stream=false \
    --set tools=false --set builtin_shell=false -i -m foo

# The status band holds the last rows of a long table, so the model rows
# scroll off it: read the usage row instead. calls 0 proves the report ran
# before any turn completed; its position before the answer proves it did
# not wait behind the turn.
"$PYTHON" - "$TEST_DIR/immediate.out" <<'EOF' || \
    fail "the mid-turn /status did not run beside the turn"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
status = plain.find(b"Usage / calls")
answer = plain.find(b"The stalled answer.")
if status < 0:
    raise SystemExit("the /status report never reached the screen")
if answer < 0:
    raise SystemExit("the stalled answer never arrived")
if status > answer:
    raise SystemExit("the /status report waited behind the turn")
if not re.search(rb"Usage / calls\s+\xe2\x94\x82\s+0", plain[:answer]):
    raise SystemExit("the /status report did not run mid-turn")
EOF

# Both turns ran on the first provider: the mid-turn read disturbed
# neither the running turn nor the session model. The mock stays up for
# the second leg, which consumes the remaining scenario steps.
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == "Bearer mock-secret"'
assert_request 1 'r["path"] == "/v1/chat/completions"'
assert_request 1 'r["auth"] == "Bearer mock-secret"'

# Leg 2: /model typed while the first turn stalls stays queued: the
# stalled answer arrives first, on the first provider, and only then
# does the switch run. The follow-up prompt afterwards uses it.
FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="The stalled answer." \
FYAI_PTY_DURING_INPUT="/model baz" \
FYAI_PTY_DURING_DELAY=0.5 \
FYAI_PTY_AFTER="send:second question|wait:The follow-up answer." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/queued.out" \
    "$FYAI_BIN" --color off \
    --set display/markdown=false --set display/stream=false \
    --set tools=false --set builtin_shell=false -i -m foo

"$PYTHON" - "$TEST_DIR/queued.out" <<'EOF' || \
    fail "the mid-turn /model did not wait behind the turn"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
answer = plain.find(b"The stalled answer.")
switched = plain.find(b"model: qux (provider otherprov")
if answer < 0:
    raise SystemExit("the stalled answer never arrived")
if switched < 0:
    raise SystemExit("the queued /model switch never ran")
if switched < answer:
    raise SystemExit("the /model switch disturbed the running turn")
EOF

# The first turn ran on the first provider; the follow-up after the
# queued switch ran on the second.
mock_stop 4
assert_request 2 'r["path"] == "/v1/chat/completions"'
assert_request 2 'r["auth"] == "Bearer mock-secret"'
assert_request 3 'r["path"] == "/v2/chat/completions"'
assert_request 3 'r["auth"] == "Bearer other-secret"'
assert_request 3 'any(m.get("role") == "user" and m.get("content") == "second question" for m in r["body"]["messages"])'

pass
