#!/bin/bash
# SPDX-License-Identifier: MIT
# A shell that never started says so, and says it differently from a command
# that ran and found nothing.
#
# A shell is named as any other command is, thus a name is looked up on PATH.
# When it cannot be started, the exit status cannot carry that: a shell that
# starts and finds no command also exits 127. The child reports what stopped it
# on a descriptor of its own, and the model reads the cause.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_start_errors.json

# A shell of our own, on PATH under a bare name.
cat > fakesh <<'SH'
#!/bin/sh
echo "fake-shell-ran: $2"
SH
chmod +x fakesh
PATH="$TEST_DIR:$PATH"
export PATH

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "check how a shell starts"
assert_status 0

"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PYEOF' || fail "a failed start is not reported"
import json
import sys

last = json.loads(open(sys.argv[1]).read().splitlines()[-1])["body"]
out = {m["tool_call_id"]: m["content"] for m in last["messages"]
       if m.get("role") == "tool"}

def want(call, text):
    if text not in out.get(call, ""):
        sys.stderr.write("%s: expected %r, got %r\n" % (call, text, out.get(call)))
        sys.exit(1)

def unwanted(call, text):
    if text in out.get(call, ""):
        sys.stderr.write("%s: unexpected %r in %r\n" % (call, text, out.get(call)))
        sys.exit(1)

# A bare name is looked up on PATH, and that shell ran the command.
want("call_name", "fake-shell-ran: echo hello")
# A shell that is not there says which shell, and why.
want("call_missing", "could not start the shell nosuchshell")
want("call_missing", "No such file or directory")
# A terminal changes nothing about the report.
want("call_missing_tty", "could not start the shell nosuchshell")
# A command that ran and found nothing is the answer of a shell that started.
want("call_notfound", "status 127")
unwanted("call_notfound", "could not start the shell")
PYEOF

mock_stop 2
pass
