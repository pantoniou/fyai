#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

"$PYTHON" - "$TEST_DIR/scenario.json" <<'EOF'
import json
import sys

calls = []
for i in range(18):
    calls.append({
        "id": "call_%02d" % i,
        "type": "function",
        "function": {
            "name": "shell",
            "arguments": json.dumps({
                "command": "sleep .05; echo fifo-%02d" % i,
            }),
        },
    })
steps = [
    {
        "response": {
            "id": "chatcmpl-fifo-1",
            "object": "chat.completion",
            "model": "mock-model",
            "choices": [{
                "index": 0,
                "finish_reason": "tool_calls",
                "message": {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": calls,
                },
            }],
        },
    },
    {
        "response": {
            "id": "chatcmpl-fifo-2",
            "object": "chat.completion",
            "model": "mock-model",
            "choices": [{
                "index": 0,
                "finish_reason": "stop",
                "message": {
                    "role": "assistant",
                    "content": "fifo group completed",
                },
            }],
        },
    },
]
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump({"steps": steps}, stream)
EOF

mock_start "$TEST_DIR/scenario.json"
run_fyai --set api=chat-completions --set display/stream=false \
	--set tools=true \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model "run the fifo group"
assert_status 0
assert_stdout_contains "fifo group completed"
assert_request 1 \
	'all(any(m.get("tool_call_id") == "call_%02d" % i '\
'and "fifo-%02d" % i in m.get("content", "") '\
'for m in r["body"]["messages"]) for i in range(18))'

mock_stop 2
pass
