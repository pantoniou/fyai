#!/bin/bash
# SPDX-License-Identifier: MIT
# The claude-code backend behind the Claude SDK front of fyai: the SDK
# decides on each shell call that the CLI asks for. An approved call runs
# once in the working directory and its output reaches the CLI; a denied call
# never starts and the CLI gets the denial; an interrupt during a call stops
# the command and the CLI, and the completed steps stay. One CLI serves the
# exchanges of the conversation; an interrupt or another conversation starts
# a new one.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
fake_claude_install claude_code_sdk.json
"$FYAI_BIN" --set api=claude-code --set "claude_code/command=$FAKE_CLAUDE" \
	>/dev/null 2>&1 || fail "configure the project"

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" "$FAKE_CLAUDE_LOG" \
	"$TESTS_DIR/wait_exit.py" <<'PYEOF' || fail "SDK permissions"
import json, os, select, subprocess, sys, time

binary, cwd, scale, clog, wait_exit = sys.argv[1:6]
scale = int(scale)
proc = subprocess.Popen([binary, "--output-format", "stream-json", "--verbose",
                         "--input-format", "stream-json",
                         "--permission-prompt-tool", "stdio"], cwd=cwd,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, bufsize=0)
asked = []

def send(frame):
    proc.stdin.write((json.dumps(frame) + "\n").encode())
    proc.stdin.flush()

def recv(timeout=20):
    deadline = time.monotonic() + timeout * scale
    data = bytearray()
    while time.monotonic() < deadline:
        if not select.select([proc.stdout], [], [],
                             max(0, deadline - time.monotonic()))[0]:
            break
        ch = os.read(proc.stdout.fileno(), 1)
        if not ch:
            raise AssertionError("the SDK process closed: " +
                                 proc.stderr.read().decode())
        if ch == b"\n":
            return json.loads(data)
        data.extend(ch)
    raise AssertionError("timed out waiting for a frame: " + repr(data))

def serve(frame):
    if frame["type"] != "control_request":
        return False
    request = frame["request"]
    assert request["subtype"] == "can_use_tool", frame
    command = request["input"]["command"]
    asked.append(command)
    if "denied.txt" in command:
        response = {"behavior": "deny", "message": "the user said no"}
    else:
        response = {"behavior": "allow", "updatedInput": request["input"]}
    send({"type": "control_response",
          "response": {"subtype": "success", "request_id": frame["request_id"],
                       "response": response}})
    return True

def until(pred):
    while True:
        frame = recv()
        if serve(frame):
            continue
        if pred(frame):
            return frame

def prompt(text):
    send({"type": "user", "session_id": "", "parent_tool_use_id": None,
          "message": {"role": "user", "content": text}})

def gone(pid):
    rc = subprocess.call([sys.executable, wait_exit, str(pid), str(10 * scale)])
    assert rc == 0, "process %d still runs" % pid

def log():
    return [json.loads(l) for l in open(clog)]

prompt("approve it")
result = until(lambda f: f["type"] == "result")
assert result["result"] == "first done" and not result["is_error"], result
assert open(os.path.join(cwd, "ran.txt")).read() == cwd + "\n"

prompt("deny it")
result = until(lambda f: f["type"] == "result")
assert result["result"] == "second done", result
assert [d["tool_use_id"] for d in result["permission_denials"]] == ["toolu_n"], result
assert not os.path.exists(os.path.join(cwd, "denied.txt"))

calls = {e["id"]: e for e in log() if e["event"] == "call"}
assert "approved-ok" in calls["toolu_y"]["result"]["content"][0]["text"], calls
assert "the user said no" in calls["toolu_n"]["result"]["content"][0]["text"], calls

# Interrupt during the call: the command and the CLI end.
prompt("start it")
started = os.path.join(cwd, "started.txt")
deadline = time.monotonic() + 20 * scale
while not os.path.exists(started):
    # A frame of the SDK process is the event that moves the run on.
    if not select.select([proc.stdout], [], [], 0.05)[0]:
        assert time.monotonic() < deadline, "the call did not start"
        continue
    serve(recv())
sleeper = int(open(started).read())
send({"type": "control_request", "request_id": "int-1",
      "request": {"subtype": "interrupt"}})
result = until(lambda f: f["type"] == "result")
assert result["is_error"] and result["subtype"] == "error_during_execution", result
gone(sleeper)
for e in log():
    if e["event"] == "start":
        gone(e["pid"])
assert "toolu_s" not in {e["id"] for e in log() if e["event"] == "call"}

assert asked == ["pwd >> ran.txt; printf approved-ok", "echo no >> denied.txt",
                 "echo $$ > started.tmp; mv started.tmp started.txt; exec sleep 300"], asked

# One CLI served the three exchanges: the later ones sent the new message alone.
users = [e for e in log() if e["event"] == "user"]
assert [u.get("turn") for u in users] == [0, 1, 2], users
assert json.dumps(users[1]["content"]) == json.dumps([{"type": "text", "text": "deny it"}]), users[1]

# An interrupted exchange leaves no CLI to resume: a new one reads the branch.
prompt("again")
result = until(lambda f: f["type"] == "result")
assert result["result"] == "after the interrupt", result
starts = [e for e in log() if e["event"] == "start"]
assert len(starts) == 2, starts
assert "conversation_history" in json.dumps(
    [e for e in log() if e["event"] == "user"][-1]["content"])

# Another conversation is not one that the CLI holds.
prompt("/clear")
until(lambda f: f["type"] == "result")
prompt("fresh")
result = until(lambda f: f["type"] == "result")
assert result["result"] == "after clear", result
starts = [e for e in log() if e["event"] == "start"]
assert len(starts) == 3, starts
last = [e for e in log() if e["event"] == "user"][-1]
assert "conversation_history" not in json.dumps(last["content"]), last
gone(starts[1]["pid"])

proc.stdin.close()
proc.wait(timeout=10 * scale)
assert proc.returncode == 0, proc.stderr.read().decode()
gone(starts[2]["pid"])
PYEOF

# The stored session keeps both completed exchanges and the started call.
run_fyai branch list --output json
assert_status 0
branch=$("$PYTHON" -c 'import json,sys; print([b["branch"] for b in json.load(open(sys.argv[1])) if b["branch"].startswith("sdk/")][0])' "$TEST_DIR/stdout") ||
	fail "no SDK branch"
run_fyai -b "$branch" history --output yaml
assert_status 0
assert_stdout_contains "first done"
assert_stdout_contains "call_id: toolu_n"
assert_stdout_contains "call_id: toolu_s"
