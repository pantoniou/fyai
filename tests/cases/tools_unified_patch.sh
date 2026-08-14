#!/bin/bash
# SPDX-License-Identifier: MIT
# A unified-diff apply_patch call must apply and must present the hunk as a
# marked, frameless diff whose gutter carries the new-side line numbers.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_unified_patch.json

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "patch it"
assert_status 0
assert_file_content hello.c 'int main(void)
{
	puts("new");
	puts("extra");
	return 0;
}'

# The title row names the updated file, and the body is marked, not framed.
assert_stdout_contains "update"
assert_stdout_contains "⎿"
assert_stdout_not_contains "────────"
# The hunk header supplies the gutter, so a new-side line number is rendered.
"$PYTHON" - "$TEST_DIR/stdout" <<'PY' || fail "diff gutter has no line numbers"
import re
import sys

data = open(sys.argv[1], "rb").read()
if not re.search(rb"\n\s*1\xe2\x94\x82", data):
    raise SystemExit("no new-side line number in the diff gutter")
PY

mock_stop 2
pass
