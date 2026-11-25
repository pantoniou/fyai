#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai decide on a decision model (System One API, such as TypeSafe's Jev):
# one request of typed questions for each input; the options are a choice
# and the typed arguments are questions of their own; an answer that is
# too unsure goes to the fallback, a chat model; an option that needs text
# without a fallback is an error that carries the choice; decisions-api.dev
# wraps the result; too many questions take a second request.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start decide_systemone.yaml

# run_decide NAME OUT [GLOBAL-OPTION...] [-- DECIDE-OPTION...]
run_decide() {
	local name=$1 out=$2 globals=()
	shift 2
	while [ $# -gt 0 ] && [ "$1" != -- ]; do
		globals+=("$1")
		shift
	done
	[ $# -gt 0 ] && shift
	"$FYAI_BIN" -k test-key --color off --set api=chat-completions \
		--set display/stream=false \
		--set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model \
		"${globals[@]}" decide "$name" "$@" \
		>"$TEST_DIR/$out" 2>"$TEST_DIR/$out.err"
}

OPTS="options: {billing: {description: Payments and refunds, schema: {type: object, required: [urgent, tier], properties: {urgent: {type: boolean, description: Is it urgent?}, tier: {enum: [low, high]}}}}, other: {description: Anything else}}"

# A typed decision, and one too unsure that the fallback takes.
printf '%s\n' '{"text": "charged twice A1"}' '{"text": "unsure B2"}' |
	TYPESAFE_API_KEY=ts-key run_decide jev out1 \
	--set "decisions/jev={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', instructions: Which team handles it?, min_probability: 0.8, fallback: {}, $OPTS}" ||
	fail "decide jev failed: $(cat "$TEST_DIR/out1.err")"

"$PYTHON" - "$TEST_DIR/out1" "$TEST_DIR/requests.jsonl" <<'PY' || fail "the decision model"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
assert out[0] == {"decision": "billing", "probability": 0.9, "confidence": 0.85,
                  "probabilities": {"billing": 0.9, "other": 0.1},
                  "arguments": {"urgent": True, "tier": "high"}}, out[0]
assert out[1]["decision"] == "other" and out[1]["arguments"] == {}, out[1]
assert out[1]["classifier"]["decision"] == "billing", out[1]
assert out[1]["classifier"]["probability"] == 0.6, out[1]
reqs = [json.loads(l) for l in open(sys.argv[2])]
so = [r for r in reqs if r["path"] == "/v1/systemone"]
first = so[0]
assert first["auth"] == "Bearer ts-key", first["auth"]
body = first["body"]
assert body["model"] == "jev-latest", body
# The input is the state, as it is.
assert body["state"] == {"text": "charged twice A1"}, body["state"]
q = body["questions"]
assert sorted(q) == ["a1", "a2", "decision"], q
assert q["decision"] == {"type": "choice", "instructions": "Which team handles it?",
                         "criteria": {"billing": "Payments and refunds",
                                      "other": "Anything else"}}, q["decision"]
assert q["a1"]["type"] == "noul" and "Is it urgent?" in q["a1"]["instructions"], q["a1"]
assert "'billing'" in q["a1"]["instructions"], q["a1"]
assert q["a2"]["type"] == "choice" and q["a2"]["criteria"] == {"low": "low", "high": "high"}, q["a2"]
# The fallback is told what the decision model chose.
chat = [r for r in reqs if r["path"] == "/v1/chat/completions"]
assert len(chat) == 1, chat
user = chat[0]["body"]["messages"][1]["content"]
assert "unsure B2" in user and "leans to 'billing'" in user and "0.60" in user, user
params = chat[0]["body"]["tools"][0]["function"]["parameters"]
assert params["properties"]["decision"]["enum"] == ["billing", "other"], params
PY

# Triage on a decision model, with no fallback: wake needs a summary.
TYPESAFE_API_KEY=ts-key run_decide jev-triage out2 \
	--set "decisions/jev-triage={use: triage, model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone'}" \
	-- --input '{"text": "build C3"}' </dev/null ||
	fail "decide jev-triage failed: $(cat "$TEST_DIR/out2.err")"
"$PYTHON" - "$TEST_DIR/out2" <<'PY' || fail "an option that needs text"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
assert len(out) == 1 and out[0]["decision"] == "wake", out
assert "cannot write its arguments" in out[0]["error"], out
assert "arguments" not in out[0], out
PY

# decisions-api.dev: the model is VENDOR/ID, and the result is wrapped.
printf '%s\n' '{"text": "D4"}' '{"text": "E5"}' |
	DECISIONS_API_KEY=da-key run_decide da out3 \
	--set "decisions/da={model: decisions-api/typesafe/jev-1.13, url: '$MOCK_URL/v1/systemone', $OPTS}" ||
	fail "decide da failed: $(cat "$TEST_DIR/out3.err")"
"$PYTHON" - "$TEST_DIR/out3" "$TEST_DIR/requests.jsonl" <<'PY' || fail "decisions-api"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
assert out[0] == {"decision": "other", "probability": 0.85, "confidence": 0.7,
                  "probabilities": {"billing": 0.15, "other": 0.85},
                  "arguments": {}}, out[0]
assert out[1] == {"error": "The decision model failed: the provider failed: "
                           "insufficient credits."}, out[1]
reqs = [json.loads(l) for l in open(sys.argv[2])]
d4 = [r for r in reqs if "D4" in json.dumps(r["body"])][0]
assert d4["auth"] == "Bearer da-key" and d4["body"]["model"] == "typesafe/jev-1.13", d4
PY

# Nine questions: the choice, then the arguments of the option chosen.
WIDE="many: {description: Many flags, schema: {type: object, properties: {"
for i in 1 2 3 4 5 6 7 8; do
	WIDE="$WIDE f$i: {type: boolean},"
done
WIDE="$WIDE}}}, none: {description: No flags}"
TYPESAFE_API_KEY=ts-key run_decide wide out4 \
	--set "decisions/wide={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', options: {$WIDE}}" \
	-- --input '{"text": "F6"}' </dev/null ||
	fail "decide wide failed: $(cat "$TEST_DIR/out4.err")"
"$PYTHON" - "$TEST_DIR/out4" "$TEST_DIR/requests.jsonl" <<'PY' || fail "a second request"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
assert out[0]["decision"] == "many", out
assert out[0]["arguments"] == {"f%d" % i: i % 2 == 1 for i in range(1, 9)}, out
reqs = [json.loads(l)["body"] for l in open(sys.argv[2])]
f6 = [r for r in reqs if "F6" in json.dumps(r)]
assert len(f6) == 2, f6
assert list(f6[0]["questions"]) == ["decision"], f6[0]["questions"]
assert sorted(f6[1]["questions"]) == ["a%d" % i for i in range(1, 9)], f6[1]["questions"]
PY

# No key: the decider says which variable to set.
env -u TYPESAFE_API_KEY "$FYAI_BIN" -k test-key --color off \
	--set "decisions/nokey={model: typesafe/jev-latest, $OPTS}" \
	decide nokey --input '{"text": "x"}' </dev/null \
	>"$TEST_DIR/out5" 2>&1 && fail "decide without a key succeeded"
grep -q "no key for typesafe: set TYPESAFE_API_KEY" "$TEST_DIR/out5" ||
	fail "the missing key: $(cat "$TEST_DIR/out5")"

mock_stop 8
pass
