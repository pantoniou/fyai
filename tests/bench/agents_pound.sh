#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Sixteen sub-agents at the parallel ceiling, each returning a large report.
# Reports the profile of the state commit: how often the compare-and-swap is
# lost, and how the commit time divides between building the durable state and
# the atomic publish itself.
#
#   tests/bench/agents_pound.sh ./build/fyai [runs]
set -eu
FYAI_BIN="${1:?usage: agents_pound.sh <fyai-binary> [runs]}"
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/harness.sh"

runs="${2:-3}"
export FYAI_PROFILE=1

for n in $(seq 1 "$runs"); do
	fyai_test_setup
	mock_start agents_pound_${PAYLOAD:-big}.json
	start=$(date +%s.%N)
	run_fyai --set api=chat-completions --set display/stream=false \
		 --set tools=true \
		 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
		 "delegate everything"
	end=$(date +%s.%N)
	echo "### run $n: $(awk -v a=$start -v b=$end 'BEGIN{printf "%.2f", b-a}')s status=$FYAI_STATUS"
	"$PYTHON" "$BENCH_DIR/agg.py" "$TEST_DIR/stderr"
	echo "arena on disk: $(du -h "$TEST_DIR/.fyai/arena" 2>/dev/null | cut -f1)"
	mock_stop_quiet
	rm -rf "$TEST_DIR"
done
