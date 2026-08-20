#!/bin/bash
# SPDX-License-Identifier: MIT
# A session name is how the model addresses a shell, thus a name that is taken
# is refused rather than made unique behind the model's back, an unknown name
# says so instead of waiting, and a name that cannot be one is rejected.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session_names.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "open shells"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "session names are not policed"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) != 4:
    sys.stderr.write("expected four tool results, got %d\n" % len(results))
    sys.exit(1)
first, again, ghost, bad = results

if "started" not in first:
    sys.stderr.write("the first session did not start: %r\n" % first)
    sys.exit(1)
if "already open" not in again:
    sys.stderr.write("a name in use must be refused: %r\n" % again)
    sys.exit(1)
if "no shell named 'ghost'" not in ghost:
    sys.stderr.write("an unknown name must say so: %r\n" % ghost)
    sys.exit(1)
if "usable session name" not in bad:
    sys.stderr.write("a name that cannot be one must be refused: %r\n" % bad)
    sys.exit(1)
PYEOF

mock_stop 5
pass
