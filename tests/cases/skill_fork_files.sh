#!/bin/bash
# SPDX-License-Identifier: MIT
# A forked skill writes its files out in the sub-agent, under the $TMPDIR of
# the sub-agent, and runs its scripts from there. An isolated sub-agent has a
# /tmp of its own in its view, so the parent cannot write the files for it.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
# The scratch directory of a view must lie outside the project; this one is
# the case's own, so what is left in it is what this case left.
fyai_test_scratch

mkdir -p "$TEST_DIR/skills/review/scripts"
cat >"$TEST_DIR/skills/review/SKILL.md" <<'MD'
---
description: Review a change in a sub-agent.
context: fork
---
Review $ARGUMENTS. Run scripts/check.sh from the base directory.
MD
echo 'echo CHECK_OK $((3+4))' >"$TEST_DIR/skills/review/scripts/check.sh"
chmod +x "$TEST_DIR/skills/review/scripts/check.sh"

forked_run() {
	mock_start skill_fork_files.json
	run_fyai --set api=responses --set display/stream=false --set tools=true \
		 --set api_url="$MOCK_URL/v1/responses" -m mock-model "review main"
	assert_status 0
	assert_stdout_contains 'Reviewed.'
	# The sub-agent loads the skill, with the base directory it wrote.
	assert_request 1 "\"Base directory for this skill: $TMPDIR/fyai-skills-\" in json.dumps(r[\"body\"][\"input\"])"
	assert_request 1 '"Review main." in json.dumps(r["body"]["input"])'
	# Its script runs there.
	assert_request 2 '"CHECK_OK 7" in json.dumps(r["body"]["input"])'
	assert_request 3 '"CHILD_REPORT" in json.dumps(r["body"]["input"])'
	mock_stop 4
	ls "$TMPDIR" | grep -q fyai-skills- && fail "a skill directory was left in $TMPDIR"
	return 0
}

run_fyai skill import "$TEST_DIR/skills/review"
assert_status 0
forked_run

# In a view the sub-agent writes the files to the /tmp of the view.
unshare -Urnm true 2>/dev/null || pass
sed -i 's/^context: fork$/context: fork\nisolated: true/' "$TEST_DIR/skills/review/SKILL.md"
run_fyai skill import --force "$TEST_DIR/skills/review"
assert_status 0
forked_run
run_fyai view list
assert_status 0
assert_stdout_contains 'agent/skill-review-'
pass
