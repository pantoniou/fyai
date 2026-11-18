#!/bin/bash
# SPDX-License-Identifier: MIT
# With `durable`, a session in a view that is killed resumes as if it was not.
# The view that was not committed is dropped, and a new one is made from the
# state that the last checkpoint recorded: the files of the rounds before it
# are there, the file that the round in flight wrote is not, and the project
# is as it was.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view lies outside the project, and so does the
# FIFO that tells this case when the round in flight has written its file.
fyai_test_scratch
printf base > edit.txt
fyai_test_on_exit 'chmod -R u+rwx "$TEST_DIR"/.fyai/views 2>/dev/null || true'
mkfifo "$TEST_SCRATCH/written"
mock_start durable_view.json
export OPENAI_API_KEY=test-key

run_fyai config set view/ignore '["home","tmp","port","served","mock.log","requests.jsonl","pty1.out","pty2.out","stdout","stderr","out","err","config.yaml","recordings","pty-script.*"]'
run_fyai config set display/stream false
run_fyai config set tools true
"$FYAI_BIN" branch create s1 >/dev/null 2>&1 || fail 'no branch'
for kv in "api chat-completions" "model mock-model" \
	  "api_url $MOCK_URL/v1/chat/completions" \
	  "view/isolate_session true" "durable true"; do
	set -- $kv
	run_fyai -b s1 config set "$1" "$2"
	assert_status 0
done

# The first round writes a.txt. The second writes b.txt, says so and holds.
(pty_run "$TEST_DIR/pty1.out" "$FYAI_BIN" -b s1 -i <<PTY
timeout: 60
steps:
  - type: Edit the files
  - wait-screen: B-WRITTEN
    timeout: session
  - touch: $TEST_SCRATCH/written
    timeout: session
  - wait-screen: NEVER-ON-SCREEN
    timeout: session
PTY
) >/dev/null 2>&1 &
driver=$!
# The driver opens the FIFO and closes it: the read ends at once, with no data.
read -r _ <"$TEST_SCRATCH/written" || true

# Every process of the session, found from the directory that it runs in.
# The mock is not part of the session: it answers the new one.
tree=$(ps -eo pid=,args= | grep -F -- "$TEST_DIR" | grep -v grep |
       awk -v mock="$MOCK_PID" '$1 != mock {print $1}')
all=$tree
while [ -n "$tree" ]; do
	next=
	for p in $tree; do next="$next $(pgrep -P "$p" || true)"; done
	tree=$(echo $next)
	all="$all $tree"
done
kill -9 $all 2>/dev/null || true
wait "$driver" 2>/dev/null || true
[ ! -e a.txt ] && [ ! -e b.txt ] && [ "$(cat edit.txt)" = base ] ||
	fail 'the view wrote to the project'

# The session starts again on the branch and goes on from the checkpoint. Its
# view is the one that was lost, put back to the state of that checkpoint.
pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" -b s1 -i <<'PTY'
timeout: 60
steps:
  - type: /continue
  - wait-screen: Listed.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'EOF' || fail 'the new view is not the state of the checkpoint'
import json
import sys

reqs = [json.loads(l) for l in open(sys.argv[1]).read().splitlines()]
text = json.dumps(reqs[-1]["body"]["messages"])
if "LS[a.txt EDITED]" not in text:
    raise SystemExit("the listing is not a.txt alone: %s" % text[-300:])
EOF
# The session ended at its prompt, so its view went into the project: the file
# of the round before the checkpoint, and not the one of the round that was lost.
[ "$(cat a.txt)" = ROUND1 ] || fail 'the file of the first round is not in the project'
[ ! -e b.txt ] || fail 'the file of the lost round is in the project'
[ "$(cat edit.txt)" = EDITED ] || fail 'the edit of a file of the project is not in the project'
mock_stop
pass
