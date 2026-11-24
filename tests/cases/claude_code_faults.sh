#!/bin/bash
# SPDX-License-Identifier: MIT
# The claude-code backend with a CLI that misbehaves: a call asked for twice
# runs once, a call that no reply made does not run, a line that is not JSON
# is dropped, a CLI that exits in the words of a reply is replaced and the
# reply goes on, a CLI that exits in a tool call or ends the turn with an
# error fails the turn with its cause, and a CLI that would bill an API key or has no login is
# stopped before it gets the conversation. No CLI outlives its exchange.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# use SCENARIO NAME: select a fake CLI for the next run.
use() {
	fake_claude_install "$1" "$2"
	"$FYAI_BIN" --set api=claude-code \
		--set "claude_code/command=$FAKE_CLAUDE" >/dev/null 2>&1 ||
		fail "configure $2"
}

# reaped: each CLI that the log of the last run names has exited.
reaped() {
	"$PYTHON" - "$FAKE_CLAUDE_LOG" "$TESTS_DIR/wait_exit.py" \
		"$FYAI_TIMEOUT_SCALE" <<'PYEOF' || fail "a CLI of $FAKE_CLAUDE_LOG runs on"
import json, subprocess, sys
for line in open(sys.argv[1]):
    e = json.loads(line)
    if e["event"] == "start":
        rc = subprocess.call([sys.executable, sys.argv[2], str(e["pid"]),
                              str(10 * int(sys.argv[3]))])
        assert rc == 0, e["pid"]
PYEOF
}

logged() {
	grep -qF -- "$1" "$FAKE_CLAUDE_LOG" || fail "the CLI log has no: $1"
}

use claude_code_fault_duplicate.json duplicate
run_fyai "go"
assert_status 0
assert_stdout_contains "Done duplicate."
[ "$(grep -c duplicate ran.txt)" = 1 ] || fail "the call ran $(grep -c duplicate ran.txt) times"
logged "fyai has no tool call 'toolu_f' to run"
reaped

use claude_code_fault_unknown.json unknown
run_fyai "go"
assert_status 0
assert_stdout_contains "Done unknown."
logged "fyai has no tool call 'toolu_unknown' to run"
reaped

use claude_code_fault_garbage.json garbage
run_fyai "go"
assert_status 0
assert_stdout_contains "Done garbage."
reaped

# A CLI that goes away in the words of a reply: they stay, and a new CLI
# continues from them.
use claude_code_fault_exit.json exit
run_fyai "go"
assert_status 0
assert_stdout_contains "Done exit."
[ "$(grep -c 'Running\.' stdout)" = 1 ] || fail "the words of the cut reply are not shown once"
grep -q exit ran.txt && fail "a reply that did not end ran its call"
"$PYTHON" - "$FAKE_CLAUDE_LOG" <<'PYEOF' || fail "the new CLI did not continue the cut reply"
import json, sys
users = [json.loads(l) for l in open(sys.argv[1]) if '"event": "user"' in l]
assert len(users) == 2, users
second = json.dumps(users[1]["content"])
assert "<progress>" in second and "Running." in second, second
PYEOF
reaped

# A CLI that goes away in a tool call: its input is cut, and the turn fails.
use claude_code_fault_exit_in_tool.json exit_in_tool
run_fyai "go"
assert_status_nonzero
assert_stderr_contains "the Claude Code CLI exited: fake-claude: lost the connection"
grep -q exit ran.txt && fail "a cut tool call ran"
reaped

use claude_code_fault_result.json result
run_fyai "go"
assert_status_nonzero
assert_stderr_contains "the Claude Code CLI ended the turn: rate limit reached"
reaped

use claude_code_api_key.json api_key
run_fyai "secret prompt"
assert_status_nonzero
assert_stderr_contains "would bill the API key of ANTHROPIC_API_KEY"
assert_stderr_contains "claude_code/api_billing"
grep '"event": "user"' "$FAKE_CLAUDE_LOG" && fail "the conversation went to a CLI that bills a key"
reaped

use claude_code_signed_out.json signed_out
run_fyai "secret prompt"
assert_status_nonzero
assert_stderr_contains "is not signed in to a Claude subscription"
grep '"event": "user"' "$FAKE_CLAUDE_LOG" && fail "the conversation went to a CLI with no login"
reaped

# With api_billing, the same CLI may take the conversation.
use claude_code_api_key.json billing
"$FYAI_BIN" --set claude_code/api_billing=true >/dev/null 2>&1 || fail "allow billing"
run_fyai "go"
assert_status 0
assert_stdout_contains "billed"
reaped
