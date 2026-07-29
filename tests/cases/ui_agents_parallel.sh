#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify separate work bands for two concurrent sub-agents.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agents_parallel.json

FYAI_PTY_INPUT="delegate both halves" \
FYAI_PTY_MID_NEEDLE="second half" \
FYAI_PTY_MID_TIMEOUT="3" \
FYAI_PTY_NEEDLE="Both sub-agents reported." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "parallel agent bands are wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# Both delegations are labelled, by name and description.
for needle in (b"[alpha] first half", b"[beta] second half"):
    if needle not in plain:
        raise SystemExit("missing agent label: %r" % needle)
# Require two pending agent rows in one frame.
pending = {}
pending_frames = {}
for m in re.finditer(rb"\[(alpha|beta)\]", data):
    margin = data[max(0, m.start() - 60):m.start()]
    if b"33m" in margin and b"32m" not in margin:
        pending.setdefault(m.group(1), []).append(m.start())
        frame = re.search(rb"\x1b\[33m(.*?)\x1b\[0m ", margin, re.S)
        if frame:
            pending_frames.setdefault(m.group(1), set()).add(frame.group(1))
if not any(abs(a - b) < 600
           for a in pending.get(b"alpha", [])
           for b in pending.get(b"beta", [])):
    raise SystemExit("agent bands never ran concurrently")
if not any(len(frames) > 1 for frames in pending_frames.values()):
    raise SystemExit("agent progress indicator did not animate")
# The slower agent keeps the first report visible in its work band.
if b"REPORT-ALPHA" not in plain:
    raise SystemExit("missing agent progress")
# Do not show the full delegated task.
for needle in (b"TASK-ALPHA", b"TASK-BETA"):
    if needle in plain:
        raise SystemExit("sub-agent task leaked: %r" % needle)
if b"Both sub-agents reported." not in plain:
    raise SystemExit("parent final answer missing")
EOF

# Each sub-agent ran its own restricted, agent-free tool loop.
assert_request 1 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'
assert_request 2 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'
# The parent folded both reports back as the two tool results.
assert_request 3 \
	'all(any(m.get("tool_call_id") == call and "REPORT" in m.get("content", "") '\
'for m in r["body"]["messages"]) for call in ("call_agent_alpha", "call_agent_beta"))'

mock_stop 4
pass
