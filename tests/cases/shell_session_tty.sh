#!/bin/bash
# SPDX-License-Identifier: MIT
# A session needs a terminal; without one the command runs to completion.
#
# A terminal is what makes a program behave as it does for a person, and it is
# the only thing a session has to drive. On pipes a command has a definite end
# and the model asked for its output, so it runs and answers with what it
# wrote, as it did before sessions existed. A name with no terminal is refused
# rather than turned into a shell nobody can use.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_session_tty.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "probe the session"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "a session took the wrong terminal"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])
results = [m["content"] for m in last["body"]["messages"]
           if m.get("role") == "tool"]
if len(results) != 4:
    sys.stderr.write("expected four tool results, got %d\n" % len(results))
    sys.exit(1)
named, tty, tty_read, plain = results

# A name with no terminal is refused, and the answer says what to do instead.
if "started" in named:
    sys.stderr.write("a session opened with no terminal: %r\n" % named)
    sys.exit(1)
for want in ("tty", "name"):
    if want not in named:
        sys.stderr.write("the refusal does not name %r: %r\n" % (want, named))
        sys.exit(1)

# A name with a terminal is a session: it answers before the program ends,
# and the program finds a terminal.
if "started" not in tty:
    sys.stderr.write("the terminal session did not start: %r\n" % tty)
    sys.exit(1)
if "ON-A-TERMINAL" not in tty_read:
    sys.stderr.write("a session that asked for a terminal had none: %r\n"
                     % tty_read)
    sys.exit(1)

# No name: the command ran to completion and the output came back with it.
if "ON-A-PIPE" not in plain or "READY" not in plain:
    sys.stderr.write("a plain command did not return its output: %r\n" % plain)
    sys.exit(1)
if "started" in plain:
    sys.stderr.write("a plain command opened a session: %r\n" % plain)
    sys.exit(1)
PYEOF

mock_stop 5
pass
