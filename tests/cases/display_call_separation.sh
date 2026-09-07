#!/bin/bash
# SPDX-License-Identifier: MIT
# Two calls of one turn are separated in the transcript and in the live run. A
# newline that closes an open row is not a blank row. Counted as one, it made
# the manager remove the fence between the calls.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_parallel.json

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "go"
assert_status 0

cp "$TEST_DIR/stdout" "$TEST_DIR/live.txt"
"$FYAI_BIN" --color off history --last 1 | tail -n +3 > "$TEST_DIR/replay.txt"

# A blank row is over the second call and over the prose after it, on both
# paths.
"$PYTHON" - "$TEST_DIR/live.txt" "$TEST_DIR/replay.txt" <<'PY' ||
import re
import sys

for path in sys.argv[1:]:
    rows = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "",
                  open(path, encoding="utf8").read()).split("\n")
    heads = [i for i, r in enumerate(rows) if r.startswith("● ")]
    if len(heads) != 2:
        raise SystemExit("%s: %d call rows, want 2" % (path, len(heads)))
    if rows[heads[1] - 1].strip():
        raise SystemExit("%s: no blank row over the second call: %r"
                         % (path, rows[heads[1] - 1]))
    prose = [i for i, r in enumerate(rows)
             if r.startswith("  parallel tools completed")]
    if not prose:
        raise SystemExit("%s: the prose after the calls is missing" % path)
    if rows[prose[0] - 1].strip():
        raise SystemExit("%s: no blank row over the prose: %r"
                         % (path, rows[prose[0] - 1]))
PY
	fail "the calls of a turn are not separated"

diff -u "$TEST_DIR/live.txt" "$TEST_DIR/replay.txt" > "$TEST_DIR/parity.diff" ||
	{ cat "$TEST_DIR/parity.diff" >&2
	  fail "the replay diverged from the live render"; }

mock_stop 2
pass
