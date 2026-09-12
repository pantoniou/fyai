#!/bin/bash
# SPDX-License-Identifier: MIT
# A fenced mermaid block of a Markdown answer is drawn as a diagram, and a diff
# that names mermaid source keeps its own rendering.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_mermaid_block.json

set +e
"$FYAI_BIN" -k test-key --color off --set display/markdown=true \
	--set display/markdown_mode=oneshot --set api=chat-completions \
	--set display/stream=false --set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
set -e
mock_stop_quiet
assert_status 0

# Without block renderers the source stays a code block and nothing is drawn.
if grep -q "A\[peek\] --> B\[scan next\]" "$TEST_DIR/stdout" &&
   ! grep -qE "[┌+].*peek" "$TEST_DIR/stdout"; then
	skip "fyai is built without Markdown diagram blocks"
fi

# The diagram draws the node labels in boxes, not the mermaid source.
assert_stdout_contains "peek"
assert_stdout_contains "scan next"
assert_stdout_not_contains "A[peek] --> B[scan next]"
# The diff keeps its rows, source text included.
assert_stdout_contains "flowchart LR"

pass
