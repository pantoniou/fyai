#!/bin/bash
# SPDX-License-Identifier: MIT
# Pathological context shape: the whole context is consumed by a couple of
# turns - a multi-megabyte read_file tool result plus ~1MB assistant replies -
# instead of many small ones. Exercises JSON escaping of huge strings, the
# arena commit, and next-turn reconstruction of a fat-but-short history, for
# all three APIs. Scale with:
#   FYAI_PERF_FILE_MB=<n>   size of the file the tool reads (default 4)
#   FYAI_PERF_REPLY_KB=<n>  size of each canned assistant reply (default 1024)
set -eu
. "$(dirname "$0")/../harness.sh"

FILE_MB="${FYAI_PERF_FILE_MB:-4}"
REPLY_KB="${FYAI_PERF_REPLY_KB:-1024}"

fyai_test_setup

# A big but JSON-hostile file: newlines, quotes and backslashes throughout,
# so escaping is really exercised (not one flat run of 'a').
"$PYTHON" - "$TEST_DIR/big.txt" "$FILE_MB" <<'EOF'
import sys
line = 'x" \\ y\t' + "payload " * 12 + "\n"
with open(sys.argv[1], "w") as f:
    need = int(sys.argv[2]) * 1024 * 1024
    f.write(line * (need // len(line)))
EOF

# Per-mode scenario: tool call -> big-reply final -> follow-up final.
gen_scenario() {
	"$PYTHON" - "$TEST_DIR/patho.json" "$1" "$REPLY_KB" <<'EOF'
import json, sys
path, mode, kb = sys.argv[1], sys.argv[2], int(sys.argv[3])
pad = ("lorem ipsum dolor sit amet " * 64)[:kb * 1024]
args = '{"path":"big.txt"}'
def final(i, text):
    if mode == "chat-completions":
        return {"response": {"id": "cc%d" % i, "object": "chat.completion",
            "created": 1, "model": "mock-model",
            "choices": [{"index": 0,
                         "message": {"role": "assistant", "content": text},
                         "finish_reason": "stop"}]}}
    if mode == "responses":
        return {"response": {"id": "r%d" % i, "object": "response",
            "model": "mock-model", "status": "completed",
            "output": [{"type": "message", "id": "m%d" % i,
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": text}]}]}}
    return {"response": {"id": "am%d" % i, "type": "message",
        "role": "assistant", "model": "mock-model",
        "content": [{"type": "text", "text": text}],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 10, "output_tokens": 10}}}
def toolcall():
    if mode == "chat-completions":
        return {"response": {"id": "cc0", "object": "chat.completion",
            "created": 1, "model": "mock-model",
            "choices": [{"index": 0, "message": {"role": "assistant",
                "content": None,
                "tool_calls": [{"id": "call_big", "type": "function",
                    "function": {"name": "read_file", "arguments": args}}]},
                "finish_reason": "tool_calls"}]}}
    if mode == "responses":
        return {"response": {"id": "r0", "object": "response",
            "model": "mock-model", "status": "completed",
            "output": [{"type": "function_call", "id": "fc0",
                "call_id": "call_big", "name": "read_file",
                "arguments": args, "status": "completed"}]}}
    return {"response": {"id": "am0", "type": "message", "role": "assistant",
        "model": "mock-model",
        "content": [{"type": "tool_use", "id": "call_big",
                     "name": "read_file", "input": {"path": "big.txt"}}],
        "stop_reason": "tool_use",
        "usage": {"input_tokens": 10, "output_tokens": 10}}}
steps = [toolcall(),
         final(1, "digested the big file. " + pad),
         final(2, "follow-up done. " + pad)]
json.dump({"steps": steps}, open(path, "w"))
EOF
}

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

echo "file=${FILE_MB}MB replies=${REPLY_KB}KB"
echo "mode              turn1_ms turn2_ms req2_bytes arena_kb"
for mode in chat-completions responses messages; do
	gen_scenario "$mode"
	mock_start "$TEST_DIR/patho.json"

	case "$mode" in
	chat-completions) args=(--chat-completions -u "$MOCK_URL/v1/chat/completions") ;;
	responses)        args=(--responses -u "$MOCK_URL/v1/responses") ;;
	messages)         args=(--messages -u "$MOCK_URL/v1/messages") ;;
	esac

	t0=$(now_ms)
	run_fyai --no-stream --new -t "${args[@]}" -m mock-model "read big.txt"
	t1=$(now_ms)
	assert_status 0
	assert_stdout_contains "digested the big file."
	turn1=$(( t1 - t0 ))

	t0=$(now_ms)
	run_fyai --no-stream "${args[@]}" -m mock-model "follow up"
	t1=$(now_ms)
	assert_status 0
	assert_stdout_contains "follow-up done."
	turn2=$(( t1 - t0 ))

	# the follow-up request must replay the full multi-MB tool result
	req_bytes=$(tail -n 1 "$TEST_DIR/requests.jsonl" | wc -c)
	min_bytes=$(( FILE_MB * 1024 * 1024 ))
	[ "$req_bytes" -ge "$min_bytes" ] || \
		fail "$mode: follow-up request $req_bytes bytes, tool result lost"

	arena_kb=$(du -sk "$TEST_DIR/.fyai/arena" | cut -f1)
	printf '%-18s %7d %8d %10d %8d\n' \
		"$mode" "$turn1" "$turn2" "$req_bytes" "$arena_kb"

	# soft guard: a couple of fat turns must stay interactive
	[ "$turn1" -le 15000 ] && [ "$turn2" -le 15000 ] || \
		fail "$mode: pathological turn exceeded 15s"

	mock_stop 3
done

t0=$(now_ms); "$FYAI_BIN" dump state >/dev/null 2>&1; t1=$(now_ms)
echo "dump state: $(( t1 - t0 )) ms"

pass
