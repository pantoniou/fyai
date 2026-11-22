#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that sub-agents with no room, as tiles of the work pane or as blocks
# of the transcript, collapse to one line each in the Agents panel, and that
# the input and the status keep the bottom rows of a short terminal.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/agents_panel.json" <<'PY'
import json
import sys

out = sys.argv[1]
P = "/v1/chat/completions"


def reply(message, reason):
    return {"response": {"id": "chatcmpl-panel", "object": "chat.completion",
                         "model": "mock-model",
                         "choices": [{"index": 0, "finish_reason": reason,
                                      "message": message}]}}


def call(i, name, args):
    return {"id": "call_%s_%d" % (name, i), "type": "function",
            "function": {"name": name, "arguments": json.dumps(args)}}


agents = [call(i, "agent", {"name": "p%d" % (i + 1),
                            "description": "panel agent %d" % (i + 1),
                            "context": "fresh",
                            "task": "PANEL-AGENT p%d" % (i + 1)})
          for i in range(8)]
steps = [
    # Each agent ends when the case releases it; the session then ends.
    dict(match={"path": P, "last_role": "tool", "last_contains": "held-line"},
         repeat=True, wait_for="release",
         **reply({"role": "assistant", "content": "AGENT-DONE"}, "stop")),
    dict(match={"path": P, "last_role": "tool"},
         **reply({"role": "assistant", "content": "SESSION-DONE"}, "stop")),
    dict(match={"path": P, "last_contains": "PANEL-AGENT"}, repeat=True,
         **reply({"role": "assistant", "content": None, "tool_calls": [
             call(0, "exec_command", {
                 "command": "echo held-line",
                 "description": "hold"})]}, "tool_calls")),
    dict(match={"path": P, "last_contains": "start the agents"},
         **reply({"role": "assistant", "content": None,
                  "tool_calls": agents}, "tool_calls")),
]
json.dump({"steps": steps}, open(out, "w"), indent=1)
PY
# The agents of a pane leave the grid; those of the transcript leave their
# blocks. Both stand in the panel.
panel_case() {
	rm -f "$TEST_DIR/release"
	mock_start "$TEST_DIR/agents_panel.json"

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set "display/tool_display=$1" \
    --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<PTY
rows: 24
cols: 80
steps:
  - type: start the agents
  - wait-screen: "… 3 more"
    timeout: session
  - checkpoint: "$TEST_DIR/panel.txt"
  - touch: "$TEST_DIR/release"
  - wait-screen: SESSION-DONE
    timeout: session
  - wait-gone: Agents
  - checkpoint: "$TEST_DIR/after.txt"
PTY

"$PYTHON" - "$TEST_DIR/panel.txt" "$TEST_DIR/after.txt" <<'EOF' || \
    fail "the agents panel is wrong with $1 tool display"
import sys

def window(path):
    """The rows of a checkpoint, without its style lines."""
    return [line[2:] for line in open(path).read().split("\n")
            if line.startswith("R|")]


rows = window(sys.argv[1])
text = "\n".join(rows)
# One line for each agent that left the grid, under the heading; a quarter
# of the rows of the page holds them, and the last counts the rest.
head = [i for i, r in enumerate(rows) if r.strip() == "Agents"]
if len(head) != 1:
    raise SystemExit("no Agents heading:\n" + text)
lines = rows[head[0] + 1:head[0] + 7]
if sum("panel agent" in r for r in lines) != 5:
    raise SystemExit("not five agent lines:\n" + text)
if "… 3 more" not in lines[5]:
    raise SystemExit("no count of the agents left out:\n" + text)
# The prompt and the status keep the bottom of the window.
tail = [r for r in rows if r.strip()][-2:]
if "mock-model" not in tail[-1]:
    raise SystemExit("the status is not the last row:\n" + text)
after = "\n".join(window(sys.argv[2]))
if "Agents" in after:
    raise SystemExit("the panel stayed after the agents ended:\n" + after)
EOF

	mock_stop
}

panel_case pane
panel_case inline
pass
