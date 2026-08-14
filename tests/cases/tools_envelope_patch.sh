#!/bin/bash
# SPDX-License-Identifier: MIT
# A Codex-style envelope carries no line numbers. It is resolved against the
# file before it is applied, so the rendered hunk has the same gutter as a
# unified diff, live and in the stored history alike.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_envelope_patch.json

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "patch it"
assert_status 0
assert_file_content hello.c 'int main(void)
{
	puts("new");
	return 0;
}'
assert_stdout_contains "@@ -1,5 +1,5 @@"

"$FYAI_BIN" --color off history --last 1 >"$TEST_DIR/history.out" 2>&1 ||
	fail "history replay of the patch call failed"
grep -qF "@@ -1,5 +1,5 @@" "$TEST_DIR/history.out" ||
	fail "stored patch lost its resolved hunk header"

# One changed row, not a whole-block replacement.
"$PYTHON" - "$TEST_DIR/stdout" <<'PY' || fail "hunk is not minimal"
import re
import sys

data = open(sys.argv[1], "rb").read()
if len(re.findall(rb"\n[^\n]*\xe2\x94\x82 \+", data)) != 1:
    raise SystemExit("expected exactly one added row")
if len(re.findall(rb"\n[^\n]*\xe2\x94\x82 -", data)) != 1:
    raise SystemExit("expected exactly one removed row")
PY

mock_stop 2
pass
