#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai decide: an option with `tool` runs the tool as a tool call of a turn
# runs, and the decider decides again on its result; a tool that the
# configuration does not offer fails before any input. A decision model
# answers an integer range with a score question. Images go with each
# decision: as TypeSafe assets, as image_url parts of the state, and to the
# chat fallback; an input names its images only as data URLs.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start decide_steps.yaml

# A chat model that takes images, served by the mock, for the fallback.
cat >"$TEST_DIR/cat.yaml" <<EOF
models:
- name: eye-model
  modalities: [text, image]
  capabilities: [tool_calling]
providers:
- name: mock
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: eye-model
    provider_model_id: eye-model
EOF
"$FYAI_BIN" catalog import "$TEST_DIR/cat.yaml" </dev/null >"$TEST_DIR/import" 2>&1 ||
	fail "catalog import: $(cat "$TEST_DIR/import")"

CHAT=(-k test-key --color off --set api=chat-completions
	--set display/stream=false --set "api_url=$MOCK_URL/v1/chat/completions"
	-m mock-model)

# A tool step: the decider runs a command, then decides on what it printed.
"$FYAI_BIN" "${CHAT[@]}" \
	--set "decisions/act={max_steps: 4, options: {run: {description: Run a command, tool: exec_command}, done: {description: Enough}}}" \
	decide act --input '{"text": "T1"}' </dev/null \
	>"$TEST_DIR/out1" 2>"$TEST_DIR/err1" ||
	fail "decide act failed: $(cat "$TEST_DIR/err1")"
"$PYTHON" - "$TEST_DIR/out1" "$TEST_DIR/requests.jsonl" <<'PY' || fail "the tool step"
import json, sys
out = [json.loads(l) for l in open(sys.argv[1])]
assert len(out) == 1 and out[0]["decision"] == "done", out
step = out[0]["steps"][0]
assert step["decision"] == "run", step
assert step["arguments"] == {"command": "echo TOOLRAN-$((6*7))"}, step
assert step["tool"]["name"] == "exec_command" and step["tool"]["ok"], step
assert "TOOLRAN-42" in json.dumps(step["tool"]["result"]), step
chat = [r["body"] for r in map(json.loads, open(sys.argv[2]))
        if r["path"] == "/v1/chat/completions"]
assert len(chat) == 2, chat
# The option takes the schema of the tool.
p = chat[0]["tools"][0]["function"]["parameters"]
run = [r["then"]["properties"]["arguments"] for r in p["allOf"]
       if r["if"]["properties"]["decision"]["const"] == "run"][0]
assert "command" in run["properties"], run
# Only structured_output is offered: the tool runs as a step, not a call.
assert [t["function"]["name"] for t in chat[0]["tools"]] == ["structured_output"]
# The second decision sees the result.
assert "TOOLRAN-42" in chat[1]["messages"][1]["content"], chat[1]["messages"][1]
PY

# A tool that the configuration does not offer fails at once.
"$FYAI_BIN" "${CHAT[@]}" \
	--set "decisions/bad={options: {a: {description: A, tool: no_such_tool}, b: {description: B}}}" \
	decide bad --input '{"text": "x"}' </dev/null >"$TEST_DIR/out2" 2>&1 &&
	fail "a tool step of no tool succeeded"
grep -q "runs the tool 'no_such_tool', which this configuration does not offer" \
	"$TEST_DIR/out2" || fail "the bad tool: $(cat "$TEST_DIR/out2")"

# A score: an integer of 1 to 3 is three levels; 1.6 is the level 2, so 3.
RATE="rate: {description: Rate it, schema: {type: object, required: [stars], properties: {stars: {type: integer, minimum: 1, maximum: 3, x-levels: [poor, fair, good]}}}}, skip: {description: Skip it}"
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" "${CHAT[@]}" \
	--set "decisions/rate={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', options: {$RATE}}" \
	decide rate --input '{"text": "S2"}' </dev/null \
	>"$TEST_DIR/out3" 2>"$TEST_DIR/err3" ||
	fail "decide rate failed: $(cat "$TEST_DIR/err3")"

# Images: a file as a TypeSafe asset.
# Jev takes 280 to 1568 pixels a side: pic.png is 300x300, tiny.png 16x16.
"$PYTHON" - "$TEST_DIR" <<'PY'
import struct, sys, zlib
def png(n):
    raw = b"".join(b"\0" + b"\xdc\x14\x14" * n for _ in range(n))
    c = lambda t, d: (struct.pack(">I", len(d)) + t + d +
                      struct.pack(">I", zlib.crc32(t + d) & 0xffffffff))
    return (b"\x89PNG\r\n\x1a\n" +
            c(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0)) +
            c(b"IDAT", zlib.compress(raw)) + c(b"IEND", b""))
open(sys.argv[1] + "/pic.png", "wb").write(png(300))
open(sys.argv[1] + "/tiny.png", "wb").write(png(16))
PY
PNG=$(base64 <"$TEST_DIR/pic.png" | tr -d "\n")
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" "${CHAT[@]}" \
	--set "decisions/rate={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', options: {$RATE}}" \
	decide rate --input '{"text": "P3"}' --image "$TEST_DIR/pic.png" </dev/null \
	>"$TEST_DIR/out4" 2>"$TEST_DIR/err4" ||
	fail "decide --image failed: $(cat "$TEST_DIR/err4")"

# An image that the model does not take is refused before any request.
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" "${CHAT[@]}" \
	--set "decisions/rate={model: typesafe/jev-latest, url: '$MOCK_URL/v1/systemone', options: {$RATE}}" \
	decide rate --input '{"text": "U6"}' --image "$TEST_DIR/tiny.png" </dev/null \
	>"$TEST_DIR/out6" 2>"$TEST_DIR/err6" ||
	fail "decide tiny failed: $(cat "$TEST_DIR/err6")"
grep -q 'image 1 is 16x16; the model takes 280 to 1568 pixels a side' "$TEST_DIR/out6" ||
	fail "the tiny image: $(cat "$TEST_DIR/out6")"
! grep -q U6 "$TEST_DIR/requests.jsonl" || fail "the tiny image was sent"

# Images of the input, as parts; too unsure, so the fallback sees them too.
printf '{"text": "Q4", "pic": "data:image/png;base64,%s"}\n{"text": "R5", "pic": "/etc/passwd"}\n' "$PNG" |
	DECISIONS_API_KEY=da-key "$FYAI_BIN" "${CHAT[@]}" \
	--set "decisions/look={model: decisions-api/typesafe/jev-1.14, url: '$MOCK_URL/v1/systemone', images: pic, min_probability: 0.8, fallback: {model: eye-model}, options: {$RATE}}" \
	decide look >"$TEST_DIR/out5" 2>"$TEST_DIR/err5" ||
	fail "decide look failed: $(cat "$TEST_DIR/err5")"

"$PYTHON" - "$TEST_DIR" "$PNG" <<'PY' || fail "scores and images"
import json, sys
d, png = sys.argv[1], sys.argv[2]
rows = lambda n: [json.loads(l) for l in open(f"{d}/{n}")]
out3 = rows("out3")
assert out3[0]["decision"] == "rate" and out3[0]["arguments"] == {"stars": 3}, out3
assert rows("out4")[0]["decision"] == "skip", rows("out4")
out5 = rows("out5")
assert out5[0]["decision"] == "skip" and out5[0]["classifier"]["decision"] == "rate", out5
# A path is not an image of the input: nothing is read.
assert out5[1] == {"error": "an image of the input is not a base64 data URL"}, out5
reqs = [json.loads(l) for l in open(f"{d}/requests.jsonl")]
body = lambda m: [r["body"] for r in reqs if m in json.dumps(r["body"])]
s2 = body("S2")[0]
assert s2["questions"]["a1"]["type"] == "score", s2["questions"]
assert s2["questions"]["a1"]["criteria"] == ["poor", "fair", "good"], s2["questions"]
p3 = body("P3")[0]
assert p3["state"] == {"input": {"text": "P3"}, "images": ["image1"]}, p3["state"]
assert p3["assets"] == {"image1": {"type": "image", "format": "png",
                                   "data_base64": png}}, p3["assets"]
q4 = [b for b in body("Q4") if "questions" in b][0]
# The data URL left the state and went as an image part.
assert q4["state"][0]["type"] == "text", q4["state"]
assert json.loads(q4["state"][0]["text"]) == {"text": "Q4"}, q4["state"]
assert q4["state"][1] == {"type": "image_url",
                          "image_url": {"url": "data:image/png;base64," + png}}, q4["state"]
chat = [b for b in body("Q4") if "messages" in b][0]
user = chat["messages"][1]["content"]
assert isinstance(user, list), user
assert png not in json.dumps(user[0]), user[0]
assert any(p.get("type") == "image_url" and png in p["image_url"]["url"]
           for p in user[1:]), user
PY

mock_stop 6
pass
