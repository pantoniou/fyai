#!/bin/bash
# SPDX-License-Identifier: MIT
# AGENTS.md and CLAUDE.md are commonly the same file, symlinked or copied. The
# instruction text must carry it one time, and must still carry two files that
# differ.
set -eu
. "$(dirname "$0")/../harness.sh"

sys_prompt_count() {
	python3 - "$TEST_DIR/requests.jsonl" "$1" <<'PY'
import json,sys
b = json.loads(open(sys.argv[1]).readline())["body"]
s = [m for m in b["messages"] if m["role"] == "system"][0]["content"]
print(s.count(sys.argv[2]))
PY
}

# A symlink: one file under two names.
fyai_test_setup
mock_start chat_basic.json
printf 'Repo rules: be terse.\n' > CLAUDE.md
ln -s CLAUDE.md AGENTS.md
run_fyai --set api=chat-completions --set display/stream=false --set tools=false \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
[ "$(sys_prompt_count 'Repo rules')" = "1" ] || fail "symlinked instructions sent twice"
mock_stop 1

# A copy: two files, same contents. Compared by content, so also one.
fyai_test_setup
mock_start chat_basic.json
printf 'Repo rules: be terse.\n' > CLAUDE.md
cp CLAUDE.md AGENTS.md
run_fyai --set api=chat-completions --set display/stream=false --set tools=false \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
[ "$(sys_prompt_count 'Repo rules')" = "1" ] || fail "copied instructions sent twice"
mock_stop 1

# Two files that differ: both are guidance, both are sent.
fyai_test_setup
mock_start chat_basic.json
printf 'Repo rules: be terse.\n' > CLAUDE.md
printf 'Agent rules: something else.\n' > AGENTS.md
run_fyai --set api=chat-completions --set display/stream=false --set tools=false \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
[ "$(sys_prompt_count 'Repo rules')" = "1" ] || fail "CLAUDE.md missing"
[ "$(sys_prompt_count 'Agent rules')" = "1" ] || fail "AGENTS.md missing"
mock_stop 1

pass
