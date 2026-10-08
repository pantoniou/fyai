#!/bin/bash
# SPDX-License-Identifier: MIT
# A background agent and a wait for it in one response.
#
# The calls run in the order the model wrote them: the agent starts, and the
# wait that follows it holds the turn until the report arrives. The wait must
# not run before the agent exists, nor after the agent ended.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_wait_same_response.json

FYAI_PTY_INPUT="start and wait" FYAI_PTY_NEEDLE="Wait returned the report." \
FYAI_PTY_TIMEOUT=30 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the wait did not follow the agent"
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
msgs = reqs[-1]["body"]["messages"]
tools = {m["tool_call_id"]: m["content"] for m in msgs if m.get("role") == "tool"}

if "started in the background" not in tools.get("c_a", ""):
    raise SystemExit("the agent did not start in the background: %r" % tools)
if "SAMEREPORT" not in tools.get("c_w", ""):
    raise SystemExit("the wait did not return the report: %r" % tools)
PYEOF

mock_stop 3
pass
