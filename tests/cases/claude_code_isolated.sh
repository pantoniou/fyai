#!/bin/bash
# SPDX-License-Identifier: MIT
# The claude-code backend under credential isolation: the transport runs the
# CLI, so the CLI and its login are in no agent and no tool. Two exchanges of
# one session each start a CLI there, and the end of an exchange stops it.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
fake_claude_install claude_code_backend.json
"$FYAI_BIN" --set api=claude-code --set "claude_code/command=$FAKE_CLAUDE" \
	>/dev/null 2>&1 || fail "configure the project"

export FYAI_TRANSPORT_ISOLATION=level-b
run_fyai "run printf fyai-bridge-ok"
assert_status 0
assert_stdout_contains "It printed fyai-bridge-ok."
[ "$(cat ran.txt)" = "$TEST_DIR" ] || fail "the call ran as: $(cat ran.txt)"
run_fyai "what did it print?"
assert_status 0
assert_stdout_contains "fyai-bridge-ok"

"$PYTHON" - "$FAKE_CLAUDE_LOG" "$TESTS_DIR/wait_exit.py" "$FYAI_TIMEOUT_SCALE" <<'PYEOF' || fail "the CLI ran outside the transport"
import json, subprocess, sys
log = [json.loads(l) for l in open(sys.argv[1])]
starts = [e for e in log if e["event"] == "start"]
assert len(starts) == 2, starts
for s in starts:
    assert "transport" in s["parent"], s["parent"]
    assert not s["api_key"], s
calls = [e for e in log if e["event"] == "call"]
assert len(calls) == 1 and "fyai-bridge-ok" in calls[0]["result"]["content"][0]["text"], calls
assert "conversation_history" in json.dumps([e for e in log if e["event"] == "user"][1])
for s in starts:
    rc = subprocess.call([sys.executable, sys.argv[2], str(s["pid"]),
                          str(10 * int(sys.argv[3]))])
    assert rc == 0, "CLI %d still runs" % s["pid"]
PYEOF
