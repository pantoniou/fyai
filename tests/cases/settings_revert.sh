#!/bin/bash
# SPDX-License-Identifier: MIT
# config revert takes back one change of the model, key by key, as a change
# of its own: a key that changed since is a conflict, and --force takes back
# only the others. The model can revert its own changes of write keys. A
# parent that a change made and the revert leaves empty goes too. config
# keep stores a trial change in the branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
# fyai applies a change of write keys unasked only in this state.
run_fyai config set sandbox_profile dev
assert_status 0
run_fyai config set model_access write
assert_status 0

change() {
	run_fyai tool settings "{\"action\":\"change\",\"changes\":$1,\"explanation\":\"x\"}"
	assert_status 0
	assert_stdout_contains '"status": "applied"'
}

change '[{"path":"temperature","value":0.25},{"path":"reasoning/effort","value":"high"}]'
assert_stdout_contains '"id": "c1"'
change '[{"path":"shell/timeout_ms","value":5000}]'
assert_stdout_contains '"id": "c2"'

# The user changed a key of c1 since: nothing is taken back.
run_fyai config set temperature 1
run_fyai config revert c1
assert_status_nonzero
assert_stderr_contains 'changed since the change, so nothing is taken back: temperature'
run_fyai config get reasoning
assert_stdout_contains 'high'

# --force takes back the key that did not change, and leaves the other.
run_fyai config revert c1 --force --output json
assert_status 0
assert_stdout_contains '"reverted"'
assert_stdout_contains '"temperature"'
run_fyai config get reasoning
assert_status_nonzero
run_fyai config get temperature
assert_stdout_contains '1'

# The model takes back its own change of write keys.
run_fyai tool settings '{"action":"revert","id":"c2"}'
assert_status 0
assert_stdout_contains '"status": "applied"'
run_fyai config get shell
assert_stdout_not_contains 'timeout_ms'

# A change cannot be taken back twice.
run_fyai config revert c2
assert_status_nonzero
assert_stderr_contains "change 'c2' is reverted"

# A trial changes this session only; keep stores it.
run_fyai tool settings '{"action":"change","changes":[{"path":"temperature","value":0.75}],"trial":true,"explanation":"t"}'
assert_stdout_contains '"id": "c5"'
run_fyai config get temperature
assert_stdout_not_contains '0.75'
run_fyai config keep c5
assert_status 0
assert_stdout_contains 'change c5 is kept'
run_fyai config get temperature
assert_stdout_contains '0.75'
run_fyai config keep c4
assert_status_nonzero
assert_stderr_contains 'is not a trial'

# Keep can persist a trial of a key that session commands treat as temporary.
run_fyai tool settings '{"action":"change","changes":[{"path":"display/theme_ground","value":"theme"}],"trial":true,"explanation":"t"}'
assert_status 0
assert_stdout_contains '"id": "c7"'
run_fyai config keep c7
assert_status 0
run_fyai config get display/theme_ground
assert_stdout_contains 'theme'

# A normal model change persists even for a session-scoped setting command.
change '[{"path":"display/table_border","value":"grid"}]'
run_fyai config get display/table_border
assert_stdout_contains 'grid'

run_fyai config changes --output json
assert_status 0
assert_stdout_contains '"kept"'
assert_stdout_contains '"reverted"'

pass
