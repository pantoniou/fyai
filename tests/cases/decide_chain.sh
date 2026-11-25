#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai decide along a chain: an option with `then` hands the input on to
# another decider, which decides on {input, steps}; an option without it
# ends the chain, whose line adds the steps. A decision model screens and a
# chat model on a model of its own routes; a decider that hands on to
# itself stops at max_steps; a `then` that names no decider fails before
# any input.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start decide_chain.yaml

# The model of route is a model of the catalogue, served by the mock.
cat >"$TEST_DIR/cat.yaml" <<EOF
models:
- name: route-model
  capabilities: [tool_calling]
providers:
- name: mock
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: route-model
    provider_model_id: route-model
EOF
"$FYAI_BIN" catalog import "$TEST_DIR/cat.yaml" </dev/null >"$TEST_DIR/import" 2>&1 ||
	fail "catalog import: $(cat "$TEST_DIR/import")"

SCREEN="{model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', instructions: Is it worth a look?, options: {noise: {description: Nothing to do}, real: {description: Someone should act, then: route}}}"
ROUTE="{model: route-model, instructions: Which team handles it?, options: {billing: {description: Payments, schema: {type: object, required: [urgent], properties: {urgent: {type: boolean}}}}, other: {description: Anything else}}}"

printf '%s\n' '{"text": "I1 buy now"}' '{"text": "I2 charged twice"}' |
	TYPESAFE_API_KEY=ts-key "$FYAI_BIN" -k test-key --color off \
	--set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model \
	--set "decisions/screen=$SCREEN" --set "decisions/route=$ROUTE" \
	decide screen >"$TEST_DIR/out1" 2>"$TEST_DIR/err1" ||
	fail "decide screen failed: $(cat "$TEST_DIR/err1")"

"$PYTHON" - "$TEST_DIR/out1" "$TEST_DIR/requests.jsonl" <<'PY' || fail "the chain"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
# The screen ends it: no steps.
assert out[0]["decision"] == "noise" and "steps" not in out[0], out[0]
# The screen hands on; route decides; the steps say how it got there.
assert out[1]["decision"] == "billing", out[1]
assert out[1]["arguments"] == {"urgent": True}, out[1]
assert out[1]["steps"] == [{"decider": "screen", "decision": "real",
                            "arguments": {}, "probability": 0.8}], out[1]
reqs = [json.loads(l) for l in open(sys.argv[2])]
chat = [r["body"] for r in reqs if r["path"] == "/v1/chat/completions"]
assert len(chat) == 1, chat
# route runs on its own model, with its own instructions and options.
assert chat[0]["model"] == "route-model", chat[0]["model"]
assert chat[0]["messages"][0]["content"] == "Which team handles it?", chat[0]["messages"][0]
params = chat[0]["tools"][0]["function"]["parameters"]
assert params["properties"]["decision"]["enum"] == ["billing", "other"], params
# It decides on the input and the steps so far.
user = chat[0]["messages"][1]["content"]
state = json.loads(user.split("<external-data>\n", 1)[1].split("\n</external-data>", 1)[0])
assert state == {"input": {"text": "I2 charged twice"},
                 "steps": [{"decider": "screen", "decision": "real",
                            "arguments": {}, "probability": 0.8}]}, state
PY

# A decider that hands on to itself stops at max_steps.
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" -k test-key --color off \
	--set "decisions/again={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', max_steps: 3, options: {more: {description: Again, then: again}, done: {description: Enough}}}" \
	decide again --input '{"text": "L3"}' </dev/null \
	>"$TEST_DIR/out2" 2>"$TEST_DIR/err2" ||
	fail "decide again failed: $(cat "$TEST_DIR/err2")"
"$PYTHON" - "$TEST_DIR/out2" "$TEST_DIR/requests.jsonl" <<'PY' || fail "max_steps"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
assert out[0]["error"] == "the chain did not end in 3 steps", out
assert [s["decision"] for s in out[0]["steps"]] == ["more", "more", "more"], out
reqs = [json.loads(l)["body"] for l in open(sys.argv[2])]
loops = [r for r in reqs if "L3" in json.dumps(r)]
assert len(loops) == 3, loops
# The first step sees the input; the next ones the steps too.
assert loops[0]["state"] == {"text": "L3"}, loops[0]["state"]
assert len(loops[2]["state"]["steps"]) == 2, loops[2]["state"]
PY

# A `then` that names no decider fails before any input.
"$FYAI_BIN" -k test-key --color off \
	--set "decisions/bad={options: {a: {description: A, then: nope}, b: {description: B}}}" \
	decide bad --input '{"text": "x"}' </dev/null >"$TEST_DIR/out3" 2>&1 &&
	fail "a chain to no decider succeeded"
grep -q "the option 'a' hands on: no decider 'nope'" "$TEST_DIR/out3" ||
	fail "the bad chain: $(cat "$TEST_DIR/out3")"

# Switching to the model of a decider stores nothing.
"$FYAI_BIN" config get model </dev/null >"$TEST_DIR/model" 2>&1 || true
! grep -q route-model "$TEST_DIR/model" ||
	fail "the model of a decider was stored: $(cat "$TEST_DIR/model")"

mock_stop 6
pass
