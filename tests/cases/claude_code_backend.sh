#!/bin/bash
# SPDX-License-Identifier: MIT
# The claude-code backend: a CLI asks for a shell call through the SDK MCP
# server of fyai, fyai runs it once in the working directory and gives the
# result back, the answer is stored with the call, and a follow-up starts a
# new CLI that reads the branch as history. The CLI gets no API key, no
# built-in tools and no session file, and no CLI outlives its exchange.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
fake_claude_install claude_code_backend.json
export ANTHROPIC_API_KEY=sk-ant-must-not-leak
"$FYAI_BIN" --set api=claude-code \
	--set "claude_code/command=$FAKE_CLAUDE" >/dev/null 2>&1 ||
	fail "configure the project"

run_fyai "run printf fyai-bridge-ok"
assert_status 0
assert_stdout_contains "It printed fyai-bridge-ok."

# The call ran one time, in the working directory.
[ "$(cat ran.txt)" = "$TEST_DIR" ] || fail "the call ran as: $(cat ran.txt)"

run_fyai "what did it print?"
assert_status 0
assert_stdout_contains "fyai-bridge-ok"

"$PYTHON" - "$FAKE_CLAUDE_LOG" "$TESTS_DIR/wait_exit.py" "$FYAI_TIMEOUT_SCALE" <<'PYEOF' || fail "the CLI log"
import json, subprocess, sys
log = [json.loads(l) for l in open(sys.argv[1])]
starts = [e for e in log if e["event"] == "start"]
assert len(starts) == 2, starts
for s in starts:
    argv = s["argv"]
    assert not s["api_key"], "the CLI got ANTHROPIC_API_KEY"
    assert argv[argv.index("--tools") + 1] == "", argv
    assert "--no-session-persistence" in argv, argv
    assert argv[argv.index("--allowedTools") + 1] == "mcp__fyai", argv
tools = [e for e in log if e["event"] == "tools"][0]["names"]
assert "exec_command" in tools, tools
calls = [e for e in log if e["event"] == "call"]
assert len(calls) == 1, calls
text = calls[0]["result"]["content"][0]["text"]
assert calls[0]["id"] == "toolu_a" and "fyai-bridge-ok" in text, calls
users = [e for e in log if e["event"] == "user"]
first = json.dumps(users[0]["content"])
assert "conversation_history" not in first, first
second = json.dumps(users[1]["content"])
assert "conversation_history" in second and "fyai-bridge-ok" in second, second
assert "what did it print?" in second, second
# Each CLI leaves when its exchange ends.
for s in starts:
    rc = subprocess.call([sys.executable, sys.argv[2], str(s["pid"]),
                          str(10 * int(sys.argv[3]))])
    assert rc == 0, "CLI %d still runs" % s["pid"]
PYEOF

run_fyai history --output yaml
assert_status 0
assert_stdout_contains "call_id: toolu_a"
assert_stdout_contains "name: exec_command"
assert_stdout_contains "output: fyai-bridge-ok"
assert_stdout_contains "It printed fyai-bridge-ok."
