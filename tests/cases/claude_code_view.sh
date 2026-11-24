#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent in a view runs the Claude Code CLI itself when there is no
# credential transport. The view leaves the home read-only; the grammar grants
# the configuration directory of the CLI, which the view mounts writable, and
# the CLI keeps its state there.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch of a view and the home lie outside the project, apart.
fyai_test_scratch
# The case is of a runtime that runs the CLI itself: no transport.
unset FYAI_TRANSPORT_ISOLATION
export TMPDIR="$TEST_SCRATCH/tmp"
export HOME="$TEST_SCRATCH/home"
mkdir -p "$TMPDIR" "$HOME/.claude"

fake_claude_install claude_code_view.json claude "$HOME/.claude/fake.log"
"$FYAI_BIN" --set api=claude-code --set "claude_code/command=$FAKE_CLAUDE" \
	--set agent/transport_isolation=none >/dev/null 2>&1 ||
	fail "configure the project"

run_fyai "delegate to an isolated sub-agent"
assert_status 0
assert_stdout_contains "parent done"

"$PYTHON" - "$FAKE_CLAUDE_LOG" "$HOME" <<'PYEOF' || fail "the CLI in the view"
import json, os, sys
log = [json.loads(l) for l in open(sys.argv[1])]
home = sys.argv[2]
starts = [e for e in log if e["event"] == "start"]
assert len(starts) == 2, starts
parent, child = starts
# Outside the view the CLI writes as it always does.
assert parent["writes"] == {"config": "ok", "home": "ok"}, parent
assert parent["config_dir"] is None, parent
# In the view the home is read-only and the granted directory is not.
assert child["writes"]["config"] == "ok", child
assert child["writes"]["home"] != "ok", child
assert child["config_dir"] == os.path.join(home, ".claude"), child
calls = [e for e in log if e["event"] == "call"]
assert len(calls) == 1 and "child done" in json.dumps(calls[0]["result"]), calls
PYEOF
