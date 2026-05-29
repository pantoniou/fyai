#!/bin/bash
# SPDX-License-Identifier: MIT
# Performance on a very large conversation: run N turns against the mock,
# each turn a full fyai invocation (arena load, history reconstruction,
# request build, response commit). Reports per-turn wall time, request body
# size and arena size at checkpoints, so growth in the *last* turns of a
# long conversation is visible. Scale with:
#   FYAI_PERF_TURNS=<n>      turns (default 40 for CTest; use 200+ for figures)
#   FYAI_PERF_MODE=<mode>    chat-completions | responses | messages
#   FYAI_PERF_REPLY_KB=<kb>  size of each canned assistant reply (default 2)
set -eu
. "$(dirname "$0")/../harness.sh"

TURNS="${FYAI_PERF_TURNS:-40}"
MODE="${FYAI_PERF_MODE:-chat-completions}"
REPLY_KB="${FYAI_PERF_REPLY_KB:-2}"

fyai_test_setup

# Generate an N-step scenario with distinct, sizeable assistant replies.
"$PYTHON" - "$TEST_DIR/perf.json" "$TURNS" "$MODE" "$REPLY_KB" <<'EOF'
import json, sys
path, n, mode, kb = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
pad = ("lorem ipsum dolor sit amet " * 64)[:kb * 1024]
steps = []
for i in range(n):
    text = "answer %d: %s" % (i + 1, pad)
    if mode == "chat-completions":
        steps.append({"response": {
            "id": "chatcmpl-p%d" % i, "object": "chat.completion",
            "created": 1735689600 + i, "model": "mock-model",
            "choices": [{"index": 0,
                         "message": {"role": "assistant", "content": text},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 10 * i, "completion_tokens": 50,
                      "total_tokens": 10 * i + 50}}})
    elif mode == "responses":
        steps.append({"response": {
            "id": "resp_p%d" % i, "object": "response", "model": "mock-model",
            "status": "completed",
            "output": [{"type": "message", "id": "msg_p%d" % i,
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": text}]}],
            "usage": {"input_tokens": 10 * i, "output_tokens": 50,
                      "total_tokens": 10 * i + 50}}})
    else:
        steps.append({"response": {
            "id": "msg_p%d" % i, "type": "message", "role": "assistant",
            "model": "mock-model",
            "content": [{"type": "text", "text": text}],
            "stop_reason": "end_turn",
            "usage": {"input_tokens": 10 * i, "output_tokens": 50}}})
json.dump({"steps": steps}, open(path, "w"))
EOF

mock_start "$TEST_DIR/perf.json"

case "$MODE" in
chat-completions) MODE_ARGS=(--chat-completions -u "$MOCK_URL/v1/chat/completions") ;;
responses)        MODE_ARGS=(--responses -u "$MOCK_URL/v1/responses") ;;
messages)         MODE_ARGS=(--messages -u "$MOCK_URL/v1/messages") ;;
*) fail "unknown FYAI_PERF_MODE: $MODE" ;;
esac

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# Checkpoints: 1, then every ~tenth of the run, always including the last.
echo "mode=$MODE turns=$TURNS reply=${REPLY_KB}KB"
echo "turn  ms  req_bytes  arena_kb"
step=$(( TURNS / 10 )); [ "$step" -ge 1 ] || step=1
first_ms=0
last_ms=0
i=1
while [ "$i" -le "$TURNS" ]; do
	t0=$(now_ms)
	run_fyai --no-stream "${MODE_ARGS[@]}" -m mock-model "question $i"
	t1=$(now_ms)
	assert_status 0
	last_ms=$(( t1 - t0 ))
	[ "$i" -eq 1 ] && first_ms=$last_ms
	if [ "$i" -eq 1 ] || [ $(( i % step )) -eq 0 ] || [ "$i" -eq "$TURNS" ]; then
		req_bytes=$(tail -n 1 "$TEST_DIR/requests.jsonl" | wc -c)
		arena_kb=$(du -sk "$TEST_DIR/.fyai/arena" | cut -f1)
		printf '%4d %4d %9d %9d\n' "$i" "$last_ms" "$req_bytes" "$arena_kb"
	fi
	i=$(( i + 1 ))
done

assert_stdout_contains "answer $TURNS:"

# Read-side cost of the accumulated history.
t0=$(now_ms); "$FYAI_BIN" dump state >/dev/null 2>&1; t1=$(now_ms)
echo "dump state: $(( t1 - t0 )) ms"
t0=$(now_ms); "$FYAI_BIN" history --raw >/dev/null 2>&1; t1=$(now_ms)
echo "history --raw: $(( t1 - t0 )) ms"

# Soft regression guard: the last turn of a long conversation must not blow
# up versus the first (allow 10x + 200ms slack for noise on small runs).
[ "$last_ms" -le $(( first_ms * 10 + 200 )) ] || \
	fail "last turn ${last_ms}ms vs first ${first_ms}ms - superlinear growth"

mock_stop "$TURNS"
pass
