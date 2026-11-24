#!/bin/bash
# SPDX-License-Identifier: MIT
# The skills of the branch serve the Claude Code CLI: the skill tool lists
# them, a loaded skill returns its instructions with its arguments, and a
# forked skill runs as a sub-agent on a CLI of its own. The skills of Claude
# Code itself stay off.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mkdir -p "$TEST_DIR/skills/greet" "$TEST_DIR/skills/review"
cat >"$TEST_DIR/skills/greet/SKILL.md" <<'MD'
---
name: greet
description: Greet a person by name.
---
Say hello to $0 warmly. Family: $ARGUMENTS[1].
MD
cat >"$TEST_DIR/skills/review/SKILL.md" <<'MD'
---
description: Review a change in a sub-agent.
context: fork
---
Review $ARGUMENTS.
MD
run_fyai skill import "$TEST_DIR/skills"
assert_status 0

fake_claude_install claude_code_skill.json
run_fyai --set api=claude-code --set "claude_code/command=$FAKE_CLAUDE" \
	"greet Ada and review main"
assert_status 0
assert_stdout_contains "Skills work through the CLI."

"$PYTHON" - "$FAKE_CLAUDE_LOG" <<'PYEOF' || fail "the CLI and the skills"
import json, sys
log = [json.loads(l) for l in open(sys.argv[1])]
starts = [e for e in log if e["event"] == "start"]
assert len(starts) == 2, starts
for s in starts:
    assert "--disable-slash-commands" in s["argv"], s["argv"]
assert "skill" in [e for e in log if e["event"] == "tools"][0]["names"]
calls = {e["id"]: e for e in log if e["event"] == "call"}
greet = calls["toolu_greet"]["result"]["content"][0]["text"]
assert "Say hello to Ada warmly. Family: Lovelace." in greet, greet
review = calls["toolu_review"]["result"]["content"][0]["text"]
assert "CHILD_REPORT reviewed main" in review, review
# The sub-agent was given the skill as its task.
child = [e for e in log if e["event"] == "user"][1]
assert "Review main." in json.dumps(child["content"]), child
PYEOF
pass
