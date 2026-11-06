#!/bin/bash
# SPDX-License-Identifier: MIT
# A background agent accepts its original name and the listed handle.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_message_live.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
timeout: 30
steps:
  - type: start message test
  - wait-file: "${TEST_DIR}/served"
    equals: "3"
  - touch: "${TEST_DIR}/child-started"
  - wait: MESSAGE SUBMITTED
    timeout: session
  - wait-screen: agent_message ping1 queued
  - touch: "${TEST_DIR}/release-child"
  - wait-screen: REPORT RECEIVED
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "agent message did not submit"
import json
import sys

reqs = [json.loads(line) for line in open(sys.argv[1])]
outputs = [m["content"] for req in reqs for m in req["body"].get("messages", [])
           if m.get("role") == "tool"]
if not any("message sent; wait for 'ping1'" in text for text in outputs):
    raise SystemExit("message submission failed: %r" % outputs)
if not any("[agent 'Beta' finished]" in text for text in outputs):
    raise SystemExit("wait for listed handle beta failed: %r" % outputs)
users = [m["content"] for req in reqs for m in req["body"].get("messages", [])
         if m.get("role") == "user"]
if not any("[agent_message 'ping1' queued]" in text for text in users):
    raise SystemExit("message was not queued: %r" % users)
if not any("[Message from agent" in text and "hello" in text for text in users):
    raise SystemExit("child did not receive the message: %r" % users)
PYEOF

[ ! -e "$TEST_DIR/stream-wait-failed" ] || fail "child release marker was not reached"
mock_stop 7
pass
