#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
mkdir project
printf 'baseline\n' > project/file
mkdir project/.fyai
printf 'reserved\n' > project/.fyai/private
ln -s file project/link

run_fyai view create demo "$TEST_DIR/project" --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] == v['root']
assert v['state'] == 'ready'
PY
run_fyai view create demo "$TEST_DIR/project"
assert_status 1

printf 'host changed\n' > project/file
run_fyai view enter demo --command 'test "$(cat file)" = baseline && test "$(cat link)" = baseline && test ! -e .fyai/private && ! touch .fyai/new && printf scratch > /tmp/scratch && test "$(cat /tmp/scratch)" = scratch && printf agent > file && mkdir child && printf nested > child/new' --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['exit_code'] == 0, v
assert not v['timed_out']
PY
[ "$(cat project/file)" = 'host changed' ] || fail 'view modified host'
[ ! -e project/child ] || fail 'view created host directory'
[ "$(cat project/.fyai/private)" = reserved ] || fail 'reserved host file changed'

run_fyai view enter demo --command 'test "$(cat file)" = agent && test "$(cat child/new)" = nested && rm link && mv child renamed' --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
assert json.load(open(sys.argv[1]))['exit_code'] == 0
PY
run_fyai view show demo --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] != v['root']
assert v['state'] == 'ready'
PY
run_fyai view list --output json
assert_status 0
assert_stdout_contains 'demo'

mkfifo project/fifo
run_fyai view create unsupported "$TEST_DIR/project"
assert_status 1
assert_stderr_contains 'Operation not supported'
pass
