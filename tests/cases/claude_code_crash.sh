#!/bin/bash
# SPDX-License-Identifier: MIT
# A Claude Code CLI that crashes is started again. One that crashes while fyai
# runs its calls is replaced at the next step, with the calls and their results;
# the calls run once. One that dies between exchanges is replaced at the next
# exchange, which reads the branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
fake_claude_install claude_code_crash.json
"$FYAI_BIN" --set api=claude-code --set "claude_code/command=$FAKE_CLAUDE" \
	>/dev/null 2>&1 || fail "configure the project"

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" "$FAKE_CLAUDE_LOG" \
	"$TESTS_DIR/wait_exit.py" <<'PYEOF' || fail "a crashed CLI"
import json, os, select, signal, subprocess, sys, time

binary, cwd, scale, clog, wait_exit = sys.argv[1:6]
scale = int(scale)
proc = subprocess.Popen([binary, "--output-format", "stream-json", "--verbose",
                         "--input-format", "stream-json",
                         "--permission-mode", "bypassPermissions"], cwd=cwd,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, bufsize=0)

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

def result():
    while True:
        frame = recv()
        if frame["type"] == "result":
            return frame

def prompt(text):
    send({"type": "user", "session_id": "", "parent_tool_use_id": None,
          "message": {"role": "user", "content": text}})

def log():
    return [json.loads(l) for l in open(clog)]

def gone(pid):
    rc = subprocess.call([sys.executable, wait_exit, str(pid), str(10 * scale)])
    assert rc == 0, "process %d still runs" % pid

prompt("run it")
r = result()
assert r["result"] == "recovered" and not r["is_error"], r
assert open(os.path.join(cwd, "ran.txt")).read() == "once\n"
starts = [e for e in log() if e["event"] == "start"]
assert len(starts) == 2, starts
users = [e for e in log() if e["event"] == "user"]
second = json.dumps(users[1]["content"])
assert "progress" in second and "tool-ok" in second and "run it" in second, second

# A kept CLI that dies between exchanges.
os.kill(starts[1]["pid"], signal.SIGKILL)
gone(starts[1]["pid"])
prompt("again")
r = result()
assert r["result"] == "relaunched" and not r["is_error"], r
starts = [e for e in log() if e["event"] == "start"]
assert len(starts) == 3, starts
last = json.dumps([e for e in log() if e["event"] == "user"][-1]["content"])
assert "conversation_history" in last and "again" in last, last
assert open(os.path.join(cwd, "ran.txt")).read() == "once\n"

proc.stdin.close()
proc.wait(timeout=10 * scale)
assert proc.returncode == 0, proc.stderr.read().decode()
gone(starts[2]["pid"])
PYEOF
