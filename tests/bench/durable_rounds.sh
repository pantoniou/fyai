#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Benchmark a multi-round tool turn with `durable` enabled vs disabled.
# Each round reads a PAYLOAD-byte file so the conversation grows linearly.
# Reports total wall time, per-round overhead, and arena size.
#
#   tests/bench/durable_rounds.sh ./build/fyai [rounds] [payload-bytes] [runs]
set -eu
FYAI_BIN="${1:?usage: durable_rounds.sh <fyai-binary> [rounds] [payload-bytes] [runs]}"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/harness.sh"

rounds="${2:-50}"
payload="${3:-1024}"
runs="${4:-3}"
SCENARIO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scenarios"
scenario="durable_rounds_${rounds}.json"

"$PYTHON" - "$SCENARIO_DIR/$scenario" "$rounds" <<'PY'
import json, sys
path, rounds = sys.argv[1], int(sys.argv[2])
def call(i):
    return {"response": {"id": f"c{i}", "object": "chat.completion",
        "model": "mock-model", "choices": [{"index": 0,
        "finish_reason": "tool_calls", "message": {"role": "assistant",
        "content": None, "tool_calls": [{"id": f"call_{i}",
        "type": "function", "function": {"name": "read_file",
        "arguments": json.dumps({"path": "data.txt"})}}]}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 4,
                  "total_tokens": 14}}}
steps = [call(i) for i in range(rounds)]
steps.append({"response": {"id": "cend", "object": "chat.completion",
    "model": "mock-model", "choices": [{"index": 0, "finish_reason": "stop",
    "message": {"role": "assistant", "content": "Done."}}],
    "usage": {"prompt_tokens": 10, "completion_tokens": 4,
              "total_tokens": 14}}})
json.dump({"steps": steps}, open(path, "w"))
PY
trap 'rm -f "$SCENARIO_DIR/$scenario"' EXIT

for durable in false true; do
	best=
	for n in $(seq 1 "$runs"); do
		fyai_test_setup
		mock_start "$scenario"
		head -c "$payload" /dev/zero | tr '\0' 'x' >data.txt
		"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
		    --set tools=true --set "durable=$durable" --set max_tool_iterations=100000 \
		    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model \
		    -k test-key --set model=mock-model >/dev/null 2>&1
		start=$(date +%s.%N)
		run_fyai --set max_tool_iterations=100000 "go"
		end=$(date +%s.%N)
		t=$(awk -v a="$start" -v b="$end" 'BEGIN{printf "%.3f", b-a}')
		[ "$FYAI_STATUS" -eq 0 ] || echo "status $FYAI_STATUS: $(head -c 200 "$TEST_DIR/stderr")"
		size=$(du -sk "$TEST_DIR/.fyai/arena" 2>/dev/null | cut -f1)
		echo "durable=$durable rounds=$rounds payload=$payload run $n: ${t}s arena=${size}K"
		mock_stop_quiet
		rm -rf "$TEST_DIR"
	done
done
