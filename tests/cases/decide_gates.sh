#!/bin/bash
# SPDX-License-Identifier: MIT
# The canned gates of fyai decide: `decide list` and `--list` name every
# decider with its model and need no key; shell-gate and triage-fast run on
# Jev, and a configured decider extends one with `use` to name its endpoint;
# the name of a decider completes, with `list`.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start decide_gates.yaml

# No key, no model: a listing sends no request.
env -u TYPESAFE_API_KEY "$FYAI_BIN" --color off decide list --output json \
	</dev/null >"$TEST_DIR/list1" 2>"$TEST_DIR/list1.err" ||
	fail "decide list: $(cat "$TEST_DIR/list1.err")"
env -u TYPESAFE_API_KEY "$FYAI_BIN" --color off decide --list --output json \
	</dev/null >"$TEST_DIR/list2" 2>"$TEST_DIR/list2.err" ||
	fail "decide --list: $(cat "$TEST_DIR/list2.err")"
cmp -s "$TEST_DIR/list1" "$TEST_DIR/list2" || fail "list and --list differ"
"$PYTHON" - "$TEST_DIR/list1" <<'PY' || fail "the list of deciders"
import json, sys
rows = {r["name"]: r for r in json.load(open(sys.argv[1]))}
assert rows["shell-gate"]["model"] == "typesafe/jev-latest", rows["shell-gate"]
assert rows["shell-gate"]["options"] == ["safe", "risky"], rows["shell-gate"]
assert rows["triage-fast"]["model"] == "typesafe/jev-latest", rows["triage-fast"]
assert rows["triage-fast"]["options"] == ["drop", "wake", "defer"], rows["triage-fast"]
assert rows["yes-no"]["model"] == "session", rows["yes-no"]
PY

# The decision models are not in the list of models, but in their own.
"$FYAI_BIN" --color off list models --output json </dev/null >"$TEST_DIR/models" 2>&1 ||
	fail "list models: $(cat "$TEST_DIR/models")"
"$FYAI_BIN" --color off list decider-models --output json </dev/null >"$TEST_DIR/dmodels" 2>&1 ||
	fail "list decider-models: $(cat "$TEST_DIR/dmodels")"
"$FYAI_BIN" --color off list deciders --output json </dev/null >"$TEST_DIR/deciders" 2>&1 ||
	fail "list deciders: $(cat "$TEST_DIR/deciders")"
"$PYTHON" - "$TEST_DIR/models" "$TEST_DIR/dmodels" "$TEST_DIR/deciders" <<'PY' || fail "the lists of models"
import json, sys
chat = [r["name"] for r in json.load(open(sys.argv[1]))]
dec = [r["name"] for r in json.load(open(sys.argv[2]))]
assert chat and dec, (chat, dec)
assert "jev-latest" in dec and "jev-latest" not in chat, (chat, dec)
assert not set(chat) & set(dec), set(chat) & set(dec)
rows = {r["name"] for r in json.load(open(sys.argv[3]))}
assert {"shell-gate", "triage-fast", "yes-no"} <= rows, rows
PY

# The name completes, and so does list.
"$FYAI_BIN" __complete -- decide "" </dev/null >"$TEST_DIR/words" 2>&1
for w in list shell-gate triage-fast yes-no; do
	grep -q "^$w" "$TEST_DIR/words" || fail "completion lacks $w: $(cat "$TEST_DIR/words")"
done

# A canned gate on the endpoint of the mock, by a decider that extends it.
GATE="{use: shell-gate, url: '$MOCK_URL/v1/systemone'}"
TRIAGE="{use: triage-fast, url: '$MOCK_URL/v1/systemone'}"
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" -k test-key --color off \
	--set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model \
	--set "decisions/shell-gate=$GATE" --set "decisions/triage-fast=$TRIAGE" \
	decide shell-gate --input '{"command": "rm -rf / G1"}' \
	>"$TEST_DIR/out1" 2>"$TEST_DIR/out1.err" ||
	fail "shell-gate: $(cat "$TEST_DIR/out1.err")"
TYPESAFE_API_KEY=ts-key "$FYAI_BIN" -k test-key --color off \
	--set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model \
	--set "decisions/triage-fast=$TRIAGE" \
	decide triage-fast --input '{"text": "newsletter T1"}' \
	>"$TEST_DIR/out2" 2>"$TEST_DIR/out2.err" ||
	fail "triage-fast: $(cat "$TEST_DIR/out2.err")"

"$PYTHON" - "$TEST_DIR/out1" "$TEST_DIR/out2" "$TEST_DIR/requests.jsonl" <<'PY' || fail "the gates"
import json, sys
g = json.loads(open(sys.argv[1]).read())
assert g["decision"] == "risky" and g["probability"] == 0.96, g
t = json.loads(open(sys.argv[2]).read())
assert t["decision"] == "drop" and t["probability"] == 0.91, t
reqs = [json.loads(l) for l in open(sys.argv[3])]
so = [r for r in reqs if r["path"] == "/v1/systemone"]
assert len(so) == 2, so
assert all(r["auth"] == "Bearer ts-key" for r in so), so
assert all(r["body"]["model"] == "jev-latest" for r in so), so
PY

mock_stop 2
pass
