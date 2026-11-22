#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify `fleet scan` and `fleet list` across session states: stored, running
# without a server, lost with pending work, and live on a server, plus project
# filtering and shell completion.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_list.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true --set durable=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
fyai_test_on_exit 'touch "$TEST_DIR/release-pending"'

# project DIR: initialize a nested project at DIR under the fleet root.
project() {
	mkdir -p "$1/.git"
	(cd "$1" && "$FYAI_BIN" init >/dev/null 2>&1 &&
		"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) ||
		fail "cannot make the project '$1'"
}
project a
project libs/inner
project node_modules/x
(cd libs/inner && "$FYAI_BIN" branch create work/one >/dev/null 2>&1)
mkdir -p libs/inner/deep

# fleet_json SUBCMD...: run `fleet` in JSON mode from a nested subdirectory.
fleet_json() {
	(cd libs/inner/deep && "$FYAI_BIN" fleet "$@" --output json)
}
# check_rows [PROJECT BRANCH STATE PID]...: assert expected rows in fleet.json.
check_rows() {
	"$PYTHON" - "$TEST_DIR/fleet.json" "$@" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
want = sys.argv[2:]
for i in range(0, len(want), 4):
    project, branch, state, pid = want[i:i + 4]
    got = [r for r in rows if r['project'] == project and r['branch'] == branch]
    assert len(got) == 1, (project, branch, rows)
    assert got[0]['state'] == state, (got, state)
    if pid != '-':
        assert got[0].get('pid') == int(pid), (got, pid)
assert not [r for r in rows if r['project'].startswith('node_modules')], rows
PY
}

# Start a standalone run blocked on its second request (live PID in pending).
mock_watch
env -C a "$FYAI_BIN" -b main --color off "Start the jobs" >out.txt 2>err.txt 8<&- &
runner=$!
wait_requests 2

fleet_json scan >"$TEST_DIR/scan.json"
"$PYTHON" - "$TEST_DIR/scan.json" <<'PY' || fail "the scan: $(cat "$TEST_DIR/scan.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [r['project'] for r in rows] == ['.', 'a', 'libs/inner'], rows
PY

# An active session scans via a child process because arenas map at fixed
# addresses and a process with one open cannot map another.
(cd libs/inner/deep && printf '/fleet scan\n' | "$FYAI_BIN" -i) \
	>"$TEST_DIR/slash.out" 2>&1 || fail "/fleet scan failed: $(cat "$TEST_DIR/slash.out")"
grep -Eq 'libs/inner +│ +2 ' "$TEST_DIR/slash.out" ||
	fail "/fleet scan in a session read no arena: $(cat "$TEST_DIR/slash.out")"

fleet_json list >"$TEST_DIR/fleet.json"
check_rows a main running "$runner" libs/inner work/one stored - ||
	fail "the list of a running session: $(cat "$TEST_DIR/fleet.json")"

# Project argument filters results to that project and its descendants.
fleet_json list libs >"$TEST_DIR/filter.json"
"$PYTHON" - "$TEST_DIR/filter.json" <<'PY' || fail "fleet list libs: $(cat "$TEST_DIR/filter.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert rows and all(r['project'].startswith('libs') for r in rows), rows
PY
fleet_json list a >"$TEST_DIR/filter.json"
"$PYTHON" - "$TEST_DIR/filter.json" <<'PY' || fail "fleet list a: $(cat "$TEST_DIR/filter.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert rows and all(r['project'] == 'a' for r in rows), rows
PY

# Session name completion works from nested subdirectories.
(cd libs/inner/deep && "$FYAI_BIN" __complete fleet attach "") >"$TEST_DIR/words" 2>&1
grep -q '^libs/inner:work/one' "$TEST_DIR/words" ||
	fail "the sessions of the fleet did not complete: $(cat "$TEST_DIR/words")"

# Kill the standalone worker tree so its pending work marks the session `lost`.
tree=$("$PYTHON" -I - "$runner" <<'PY'
import os, sys
pids, todo = [], [int(sys.argv[1])]
while todo:
    p = todo.pop()
    pids.append(p)
    for t in os.listdir('/proc/%d/task' % p) if os.path.isdir('/proc/%d' % p) else []:
        try:
            todo += [int(c) for c in open('/proc/%d/task/%s/children' % (p, t)).read().split()]
        except OSError:
            pass
print(' '.join(map(str, pids)))
PY
)
kill -9 $tree 2>/dev/null || true
wait "$runner" 2>/dev/null || true
for pid in $tree; do
	"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$pid" $((100 * FYAI_TIMEOUT_SCALE)) ||
		fail "the process $pid outlived its kill"
done
fleet_json list >"$TEST_DIR/fleet.json"
check_rows a main lost - ||
	fail "the list of a lost session: $(cat "$TEST_DIR/fleet.json")"

# A session registered with the root server is reported as `idle`.
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the server did not start: $(cat "$TEST_DIR/serve.out")"
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-file: "$SYNC/checked"
    equals: ok
    timeout: session
  - type: /exit
PTY
driver=$!
read -r _ <"$SYNC/ready" || true
fleet_json list >"$TEST_DIR/fleet.json"
check_rows . main idle - a main lost - libs/inner work/one stored - ||
	fail "the list of a live session: $(cat "$TEST_DIR/fleet.json")"
echo ok >"$SYNC/checked"
wait "$driver" || fail "the session failed: $(tail -c 2000 "$TEST_DIR/pty.out")"
pass
