#!/bin/bash
# SPDX-License-Identifier: MIT
# A client with no arena asks the worker for rows, and the query goes where
# the session goes: over the Unix socket, over HTTP, over HTTPS with a pinned
# key, and through ssh. `/query` runs in the client, sends `fyai_query`, and
# shows the page that the worker made.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

REMOTE_SERVE_ARGS="--listen https://127.0.0.1:0 --listen 127.0.0.1:0"
remote_test_setup claude_sdk_branch.json
for n in alpha bravo; do
	"$FYAI_BIN" branch create "remote/$n" >/dev/null 2>&1
done
"$FYAI_BIN" branch create remote/q >/dev/null 2>&1

HTTP_PORT="$(sed -n 's#.*listening on http://127.0.0.1:\([0-9]*\)/.*#\1#p' "$TEST_DIR/server.err")"
TLS_PORT="$(sed -n 's#.*listening on https://127.0.0.1:\([0-9]*\)/.*#\1#p' "$TEST_DIR/server.err")"
PIN="$(sed -n 's#.*the key of the server is \(sha256:[0-9a-f]*\).*#\1#p' "$TEST_DIR/server.err")"
[ -n "$HTTP_PORT" ] && [ -n "$TLS_PORT" ] && [ "${#PIN}" -eq 71 ] ||
	fail "the server did not say where it listens: $(cat "$TEST_DIR/server.err")"
export FYAI_REMOTE_PIN="$PIN"

# The fake ssh drops the options and the host, and runs the bridge here.
cat >"$TEST_DIR/fake-ssh" <<SH
#!/bin/sh
while [ "\$1" != "--" ]; do shift; done
shift 3
printf 'FYAI-BRIDGE 0000\n'
exec "$FYAI_BIN" "\$@"
SH
chmod +x "$TEST_DIR/fake-ssh"
export FYAI_SSH="$TEST_DIR/fake-ssh"

query() {
	local name=$1 target=$2
	pty_run "$TEST_DIR/$name.out" "$(command -v env)" -u MOCK_KEY \
		"$FYAI_BIN" --remote "$target" -b remote/q <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: /query branches --all --limit 1
  - wait-screen: "what: branches"
  - wait-screen: "branch: remote/q"
  - type: /query branches --after garbage
  - wait-screen: "bad_request"
  - type: /quit
PTY
}
query socket .
query http "http://127.0.0.1:$HTTP_PORT/."
query https "https://127.0.0.1:$TLS_PORT/."
query ssh "ssh://host/."
remote_test_stop
pass
