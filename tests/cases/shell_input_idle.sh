#!/bin/bash
# SPDX-License-Identifier: MIT
# Keystrokes wait for the program to be quiet, and no longer than they are told.
#
# A program that is still drawing has not taken its terminal, and what is typed
# at it then is lost: the line discipline holds the bytes while the terminal is
# cooked, and a program that starts by asking the terminal about itself reads
# them as the answer. `idle_trigger` says how long the session must write
# nothing before it is typed at. `max_wait` opens the gate anyway, because a
# program that never stops writing is one that time cannot describe.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_input_idle.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "drive the programs"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "the gate did not hold the input"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) < 4:
    raise SystemExit("expected the full exchange, got %d results" % len(results))

ticker, noisy = results[1], results[3]
# The program wrote for about 600 ms before it read. The gate waited that out,
# thus every tick is there and the answer reached the read.
for tick in ("TICK0", "TICK1", "TICK2"):
    if tick not in ticker:
        raise SystemExit("typed before the program was quiet: %r" % ticker)
if "GOT-HELLO" not in ticker:
    raise SystemExit("the program never got its answer: %r" % ticker)

# This one never goes quiet. It is typed at when max_wait says so, and answers.
if "NOISE" not in noisy:
    raise SystemExit("the noisy program wrote nothing: %r" % noisy)
if "GOT2-HI" not in noisy:
    raise SystemExit("max_wait did not open the gate: %r" % noisy)
PYEOF

mock_stop 5
pass
