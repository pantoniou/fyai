#!/bin/bash
# SPDX-License-Identifier: MIT
# source_start: the model starts an event source, here `fyai source lines`,
# with a session that configures no MCP server. `wait` with `for` returns
# its event, `list` shows it as started by the model, `cancel` stops it and
# then it is gone; a second start under a taken name is refused; cancel
# does not stop a configured source.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
SRC="$FYAI_BIN source lines -- printf 'hello-src\\\\n'"

# The scenario names the binary: write it here.
call() {	# call ID NAME ARGS-JSON
	"$PYTHON" -c 'import json,sys; print(json.dumps({"choices": [{"message": {"role": "assistant", "content": None, "tool_calls": [{"id": sys.argv[1], "type": "function", "function": {"name": sys.argv[2], "arguments": sys.argv[3]}}]}, "finish_reason": "tool_calls"}], "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}}))' "$@"
}
START=$("$PYTHON" -c 'import json,sys; print(json.dumps({"name": "feed", "command": sys.argv[1]}))' "$SRC")
cat >"$TEST_DIR/scenario.json" <<EOF
{"steps": [
 {"response": $(call c_start source_start "$START")},
 {"response": $(call c_again source_start "$START")},
 {"response": $(call c_wait wait '{"for": "feed"}')},
 {"response": $(call c_list list '{"kind": "sources"}')},
 {"response": $(call c_cancel cancel '{"name": "feed"}')},
 {"response": $(call c_list2 list '{"kind": "sources"}')},
 {"response": $(call c_cfg cancel '{"name": "cfg", "kind": "source"}')},
 {"response": {"choices": [{"message": {"role": "assistant", "content": "Done."}, "finish_reason": "stop"}],
               "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}}}
]}
EOF
mock_start "$TEST_DIR/scenario.json"

"$FYAI_BIN" -k test-key --color off --set api=chat-completions \
	--set display/stream=false --set "api_url=$MOCK_URL/v1/chat/completions" \
	--set mcp/enabled=true --set "mcp/servers/cfg={transport: stdio, command: '$FYAI_BIN', args: [source, lines, --, 'true'], events: true}" \
	-m mock-model "start a source" </dev/null >"$TEST_DIR/out" 2>"$TEST_DIR/err" ||
	fail "the session failed: $(cat "$TEST_DIR/err")"

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "source_start"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
last = reqs[-1]["body"]["messages"]
res = {m["tool_call_id"]: m["content"] for m in last if m.get("role") == "tool"}
assert res["c_start"].startswith("[source 'feed' starting]"), res["c_start"]
assert "exists already" in res["c_again"], res["c_again"]
assert res["c_wait"].startswith("[event 'feed/line']"), res["c_wait"]
assert "hello-src" in res["c_wait"], res["c_wait"]
rows = {r["name"]: r for r in json.loads(res["c_list"])["sources"]}
assert rows["feed"]["by"] == "model" and rows["feed"]["state"] == "up", rows
# A configured source whose arguments are all short strings starts.
assert "by" not in rows["cfg"] and rows["cfg"]["state"] == "up", rows
assert res["c_cancel"] == "[source 'feed' stopped]", res["c_cancel"]
rows = [r["name"] for r in json.loads(res["c_list2"])["sources"]]
assert rows == ["cfg"], rows
assert "a configured source is not stopped by cancel" in res["c_cfg"], res["c_cfg"]
# The tool is offered, and cancel knows the kind.
tools = {t["function"]["name"]: t["function"] for t in reqs[0]["body"]["tools"]}
assert "source_start" in tools, sorted(tools)
assert "source" in tools["cancel"]["parameters"]["properties"]["kind"]["enum"]
PY

mock_stop 8
pass
