#!/bin/bash
# SPDX-License-Identifier: MIT
# A read_file result becomes prompt text, so it is bounded. A file over the
# limit is truncated, the result says how much it did not show, and the
# bounded text is what reaches the follow-up request. read/max_bytes sets the
# limit; 0 removes it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# 630 KB, well over the 256 KiB default bound.
"$PYTHON" - "$TEST_DIR/big.txt" <<'EOF'
import sys
with open(sys.argv[1], "w") as f:
    f.write(("payload line of text\n" * 30000))
EOF

# MOCK_URL is only known once the mock is running, so build the invocation
# per run rather than once up front.
read_big() {
	run_fyai --new "$@" --set api=chat-completions \
		--set display/stream=false --set tools=true \
		--set api_url="$MOCK_URL/v1/chat/completions" \
		-m mock-model "read the big file"
}

# The default bound truncates and reports the complete size.
mock_start tools_read_limit.json
read_big
assert_status 0
assert_stdout_contains "Read the bounded file."
assert_request 1 'any(m.get("tool_call_id") == "call_read_big" and "truncated" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_read_big" and "630000 bytes" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_read_big" and len(m.get("content", "")) < 300000 for m in r["body"]["messages"])'
mock_stop 2

# A smaller configured bound is applied.
mock_start tools_read_limit.json
read_big --set read/max_bytes=4096
assert_status 0
assert_request 1 'any(m.get("tool_call_id") == "call_read_big" and len(m.get("content", "")) < 5000 for m in r["body"]["messages"])'
mock_stop 2

# 0 removes the bound: the complete file reaches the request.
mock_start tools_read_limit.json
read_big --set read/max_bytes=0
assert_status 0
assert_request 1 'any(m.get("tool_call_id") == "call_read_big" and len(m.get("content", "")) >= 630000 for m in r["body"]["messages"])'
assert_request 1 'all("truncated" not in m.get("content", "") for m in r["body"]["messages"] if m.get("tool_call_id") == "call_read_big")'
mock_stop 2

pass
