# SPDX-License-Identifier: MIT
# The session of the request cache cases: four lines of the user in one process,
# with tool calls, sent with the cache and without it. A case sets $api, $path
# (the endpoint after /v1/) and $splits (yes when the grammar keeps text), sources
# this file after fyai_test_setup, and ends with pass.
#
# The two runs must send the same requests, whatever the grammar. A grammar that
# splits its input also uses the kept text for the second request of a line,
# and emits only the new items there.
printf 'hello\n' > notes.txt

"$PYTHON" - "$TEST_DIR/cache.json" "$api" <<'PYEOF'
import json, sys

api = sys.argv[2]

def chat(i, content, calls):
    message = {"role": "assistant", "content": content or None}
    if calls:
        message["tool_calls"] = [{"id": "call_cache_%d" % i, "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": "{\"path\":\"notes.txt\"}"}}]
    return {"id": "chatcmpl-cache-%d" % i, "object": "chat.completion",
            "created": 1735689600 + i, "model": "mock-model",
            "choices": [{"index": 0, "message": message,
                         "finish_reason": "tool_calls" if calls else "stop"}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5,
                      "total_tokens": 15}}

def messages(i, content, calls):
    blocks = [{"type": "text", "text": content}] if content else []
    if calls:
        blocks.append({"type": "tool_use", "id": "call_cache_%d" % i,
                       "name": "read_file", "input": {"path": "notes.txt"}})
    return {"id": "msg_cache_%d" % i, "type": "message", "role": "assistant",
            "model": "mock-model", "content": blocks,
            "stop_reason": "tool_use" if calls else "end_turn",
            "usage": {"input_tokens": 10, "output_tokens": 5}}

def responses(i, content, calls):
    output = []
    if content:
        output.append({"type": "message", "id": "msg_cache_%d" % i,
                       "role": "assistant", "status": "completed",
                       "content": [{"type": "output_text", "text": content,
                                    "annotations": []}]})
    if calls:
        output.append({"type": "function_call", "id": "fc_cache_%d" % i,
                       "call_id": "call_cache_%d" % i, "name": "read_file",
                       "arguments": "{\"path\":\"notes.txt\"}",
                       "status": "completed"})
    return {"id": "resp_cache_%d" % i, "object": "response",
            "model": "mock-model", "output": output,
            "usage": {"input_tokens": 10, "output_tokens": 5,
                      "total_tokens": 15}}

make = {"chat-completions": chat, "messages": messages,
        "responses": responses}[api]

def text(i, content):
    return {"response": make(i, content, False)}

def call(i):
    return {"response": make(i, "", True)}

# Four lines of the user: a call and its answer, an answer, a call, an answer.
steps = [call(1), text(2, "The notes say hello."), text(3, "Here is more."),
         call(4), text(5, "Still hello."), text(6, "That is all.")]
json.dump({"steps": steps}, open(sys.argv[1], "w"))
PYEOF

export MOCK_KEY=test-key
export FYAI_TRACE="$TEST_DIR/trace.log"

# run_session LABEL: four lines of the user in one process; the requests of the
# mock go to requests.LABEL.jsonl.
run_session() {
	local label="$1"

	mock_start "$TEST_DIR/cache.json"
	"$FYAI_BIN" --set "api=$api" --set display/stream=false \
		--set "api_url=$MOCK_URL/v1/$path" --set model=mock-model \
		--set 'api_key={type: env, value: MOCK_KEY}' --set tools=true \
		--set "request_cache=$2" >/dev/null 2>&1 ||
		fail "configure the project for $label"
	"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PYEOF' ||
import json, os, select, subprocess, sys, time

binary, cwd, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
proc = subprocess.Popen([binary, "--output-format", "stream-json", "--verbose",
                         "--system-prompt", "", "--input-format", "stream-json"],
                        cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, bufsize=0)

def send(frame):
    proc.stdin.write((json.dumps(frame) + "\n").encode())
    proc.stdin.flush()

def recv():
    deadline = time.monotonic() + 30 * scale
    data = bytearray()
    while time.monotonic() < deadline:
        if not select.select([proc.stdout], [], [],
                             max(0, deadline - time.monotonic()))[0]:
            break
        ch = os.read(proc.stdout.fileno(), 1)
        if not ch:
            raise AssertionError("the process closed: " + proc.stderr.read().decode())
        if ch == b"\n":
            return json.loads(data)
        data.extend(ch)
    raise AssertionError("timed out waiting for a frame")

def until_result():
    while True:
        frame = recv()
        if frame["type"] == "result":
            return frame

send({"type": "control_request", "request_id": "init",
      "request": {"subtype": "initialize", "hooks": None}})
assert recv()["type"] == "control_response"
for line in ("read the notes", "tell me more", "read them again", "and now?"):
    send({"type": "user", "session_id": "", "parent_tool_use_id": None,
          "message": {"role": "user", "content": line}})
    result = until_result()
    assert result["subtype"] == "success" and not result["is_error"], result
proc.stdin.close()
proc.wait(timeout=20 * scale)
assert proc.returncode == 0, proc.stderr.read().decode()
PYEOF
		fail "the session with request_cache=$2"
	mock_stop_quiet
	cp "$TEST_DIR/requests.jsonl" "$TEST_DIR/requests.$label.jsonl"
}

run_session on true
cp "$TEST_DIR/trace.log" "$TEST_DIR/trace.on"
: >"$TEST_DIR/trace.log"
run_session off false

hits_on="$(grep -c 'request-cache: hit' "$TEST_DIR/trace.on" || true)"
misses_on="$(grep -c 'request-cache: miss' "$TEST_DIR/trace.on" || true)"
used_off="$(grep -c 'request-cache:' "$TEST_DIR/trace.log" || true)"
if [ "$splits" = yes ]; then
	[ "$hits_on" -eq 2 ] || fail "expected 2 requests from the kept text, got $hits_on"
	[ "$misses_on" -eq 4 ] || fail "expected 4 requests from the whole conversation, got $misses_on"
	# A request that extends the kept text makes the text of the new items
	# only: the emitter does not write the kept text or the tools again.
	grep 'request-cache: hit' "$TEST_DIR/trace.on" |
		sed -n 's/.*hit: \([0-9]*\) bytes, [0-9]* stable, \([0-9]*\) emitted.*/\1 \2/p' |
		while read -r body emitted; do
			[ "$((emitted * 4))" -lt "$body" ] ||
				fail "a hit made $emitted of $body bytes again"
		done
else
	# The grammar chains requests and has no input to keep.
	[ "$((hits_on + misses_on))" -eq 0 ] ||
		fail "a grammar that does not split used the cache"
fi
[ "$used_off" -eq 0 ] || fail "request_cache=false still used the cache"

"$PYTHON" - "$TEST_DIR/requests.on.jsonl" "$TEST_DIR/requests.off.jsonl" "$api" <<'PYEOF' ||
import json, sys

def bodies(path):
    return [json.dumps(json.loads(l)["body"]) for l in open(path) if l.strip()]

on, off = bodies(sys.argv[1]), bodies(sys.argv[2])
assert len(on) == 6 and len(off) == 6, (len(on), len(off))
for i, (a, b) in enumerate(zip(on, off)):
    assert a == b, "request %d differs:\n%s\n%s" % (i + 1, a, b)
if sys.argv[3] == "chat-completions":
    # Each request holds the conversation of the one before, as it sent it.
    # Messages moves its cache breakpoint with each request, so it does not.
    for i in range(1, len(on)):
        prev = json.loads(on[i - 1])["messages"]
        cur = json.loads(on[i])["messages"]
        assert cur[:len(prev)] == prev, "request %d lost the start of the last" % (i + 1)
PYEOF
	fail "the requests differ with the cache"
