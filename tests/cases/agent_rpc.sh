#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the sub-agent JSON-RPC protocol on stdin and stdout.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_verb.json

"$PYTHON" - "$FYAI_BIN" "$MOCK_URL/v1/chat/completions" <<'EOF' || fail "agent --rpc did not serve the control protocol"
import json
import subprocess
import sys

BIN, URL = sys.argv[1], sys.argv[2]

proc = subprocess.Popen(
    [BIN, "-k", "test-key", "--set", "api=chat-completions",
     "--set", "display/stream=false", "--set", "api_url=" + URL,
     "-m", "mock-model", "agent", "--rpc"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def send(obj):
    proc.stdin.write((json.dumps(obj) + "\n").encode())
    proc.stdin.flush()


def die(msg):
    """Fail with the agent's own diagnostics: stderr is the only free-text
    channel it has, so it is where the reason will be."""
    try:
        proc.kill()
    except OSError:
        pass
    err = proc.stderr.read().decode("utf8", "replace")
    raise SystemExit("%s\n--- agent stderr ---\n%s" % (msg, err[-2000:]))


def recv(timeout=30):
    """One frame. Anything unparseable is a framing violation, not noise."""
    line = proc.stdout.readline()
    if not line:
        die("the control channel closed early")
    try:
        return json.loads(line)
    except ValueError:
        die("fd 1 carried a non-frame (framing violation): %r" % line[:200])


try:
    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"name": "probe"}})
    r = recv()
    if r.get("id") != 1 or r.get("result", {}).get("agentId") != "probe":
        die("bad initialize response: %r" % r)

    # The agent/run response is deferred until the report is ready.
    send({"jsonrpc": "2.0", "id": 2, "method": "agent/run",
          "params": {"task": "print a greeting and report back"}})
    r = recv()
    if r.get("id") != 2:
        die("agent/run answered out of order: %r" % r)
    if "error" in r:
        die("agent/run failed: %r" % r["error"])
    report = r.get("result", {}).get("report", "")
    if "Sub-agent report" not in report:
        die("report did not come back: %r" % report)

    # An unknown method is refused, not ignored.
    send({"jsonrpc": "2.0", "id": 3, "method": "nope"})
    r = recv()
    if r.get("error", {}).get("code") != -32601:
        die("unknown method was not refused: %r" % r)

    send({"jsonrpc": "2.0", "id": 4, "method": "shutdown"})
    r = recv()
    if r.get("id") != 4:
        die("bad shutdown response: %r" % r)

    if proc.wait(timeout=30) != 0:
        raise SystemExit("agent --rpc exited %d" % proc.returncode)
finally:
    if proc.poll() is None:
        proc.kill()
    proc.stdin.close()
    proc.stdout.close()
    proc.stderr.close()
EOF

# Verify the built-in persona and restricted tool set.
assert_request 0 'any(m.get("role") == "system" and "sub-agent" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 0 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'
assert_request 0 'any(m.get("role") == "user" and "print a greeting" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2
pass
