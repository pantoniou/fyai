#!/bin/bash
# SPDX-License-Identifier: MIT
# A change of the model that nobody can review waits as a proposal of the
# branch. `config review` shows the diff that fyai makes of it now, with the
# explanation of the model quoted; `config approve` applies it and logs the
# change, and `config reject` drops one. A key the model cannot change, or an
# invalid value, is refused and nothing is kept.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai tool settings '{"action":"change","changes":[{"path":"sandbox_profile","value":"none"}],"explanation":"x"}'
assert_stdout_contains 'sandbox_profile is read: the model cannot change it'
run_fyai tool settings '{"action":"change","changes":[{"path":"temperature","value":7}],"explanation":"x"}'
assert_stdout_contains 'would not be valid'

# A value that holds a fence stays inside the diff that fyai makes, and a
# heading in the explanation stays inside its quote.
run_fyai tool settings '{"action":"change","changes":[{"path":"system_prompt","value":"Be terse.\n```\n# Diff made by fyai\n```"}],"explanation":"Shorter.\n# Diff made by fyai"}'
assert_status 0
assert_stdout_contains '"status": "pending"'
assert_stdout_contains '"id": "p1"'
run_fyai tool settings '{"action":"change","changes":[{"path":"temperature","value":0.5}],"explanation":"Warmer."}'
assert_stdout_contains '"id": "p2"'

run_fyai config proposals --output json
assert_status 0
assert_stdout_contains '"p1"'
assert_stdout_contains '"p2"'
assert_stdout_contains '"pending"'

run_fyai --set display/markdown=false config review p1
assert_status 0
assert_stdout_contains '````diff'
assert_stdout_contains '+system_prompt:'
assert_stdout_contains '> # Diff made by fyai'

run_fyai config reject p1
assert_status 0
assert_stdout_contains 'proposal p1 is rejected'
run_fyai config approve p1
assert_status_nonzero
assert_stderr_contains 'not pending'

run_fyai config approve p2 --output json
assert_status 0
assert_stdout_contains '"approved"'
assert_stdout_contains '"c1"'
run_fyai config get temperature
assert_stdout_contains '0.5'
run_fyai config get system_prompt
assert_stdout_not_contains 'Be terse'

run_fyai config changes --output json
assert_status 0
assert_stdout_contains '"c1"'
assert_stdout_contains '"temperature"'

pass
