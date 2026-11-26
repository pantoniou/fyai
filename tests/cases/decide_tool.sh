#!/bin/bash
# SPDX-License-Identifier: MIT
# The decide tool: a session with deciders configured offers `decide`, with
# the deciders as an enum; a call runs `fyai decide` as a child, here on a
# decision model, and its line is the result that the model goes on with.
# A session without deciders offers no such tool.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start decide_tool.yaml

CHAT=(-k test-key --color off --set api=chat-completions
	--set display/stream=false --set "api_url=$MOCK_URL/v1/chat/completions"
	-m mock-model)

# --transient: the decider is not stored, so the next session has none.
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" "${CHAT[@]}" --transient \
	--set "decisions/gate={description: Is a shell command safe to run?, model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', options: {safe: {description: Safe}, risky: {description: Risky}}}" \
	"Check K1 before you run it" </dev/null >"$TEST_DIR/out1" 2>"$TEST_DIR/err1" ||
	fail "the session failed: $(cat "$TEST_DIR/err1")"
grep -q "The gate says safe." "$TEST_DIR/out1" || fail "the answer: $(cat "$TEST_DIR/out1")"

"$FYAI_BIN" "${CHAT[@]}" "K2 hello" </dev/null >"$TEST_DIR/out2" 2>"$TEST_DIR/err2" ||
	fail "the plain session failed: $(cat "$TEST_DIR/err2")"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the decide tool"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
chat = [r["body"] for r in reqs if r["path"] == "/v1/chat/completions"]
first = chat[0]
tools = {t["function"]["name"]: t["function"] for t in first["tools"]}
assert "decide" in tools, sorted(tools)
d = tools["decide"]
assert d["parameters"]["properties"]["decider"]["enum"] == ["gate"], d
assert "- gate: Is a shell command safe to run?" in d["description"], d
# The decider asked the decision model, on the input given.
so = [r for r in reqs if r["path"] == "/v1/systemone"]
assert len(so) == 1 and so[0]["body"]["state"] == "rm -rf build K1", so
assert so[0]["auth"] == "Bearer ts-key", so[0]["auth"]
# The model got the decision line as the result of its call.
result = [m for m in chat[1]["messages"] if m.get("role") == "tool"][0]
line = json.loads(result["content"])
assert line["decision"] == "safe" and line["probability"] == 0.93, line
# No deciders, no tool.
plain = [b for b in chat if "K2" in json.dumps(b)][0]
assert "decide" not in [t["function"]["name"] for t in plain.get("tools", [])], plain
PY

mock_stop 4
pass
