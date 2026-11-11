# SPDX-License-Identifier: MIT
# A remote case serves the sandbox with `fyai serve` and ends it at exit.
set -eu
. "$(dirname "${BASH_SOURCE[0]}")/harness.sh"

remote_test_cleanup() {
	result=$?
	set +e
	kill "$SERVER_PID" 2>/dev/null || true
	wait "$SERVER_PID" 2>/dev/null || true
	(exit "$result")
	fyai_test_cleanup
}

# remote_test_setup SCENARIO: a sandbox with the mock provider, served.
remote_test_setup() {
	fyai_test_setup
	mock_start "$1"
	export MOCK_KEY=test-key
	# The client of a session takes the display of the branch: these cases
	# read the full-screen client with Markdown.
	"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
		--set "api_url=$MOCK_URL/v1/chat/completions" \
		--set model=mock-model \
		--set display/screen=fullscreen --set display/markdown=true \
		--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
	remote_serve_start
}

# remote_serve_start: serve the sandbox, and wait until the server listens.
remote_serve_start() {
	# A case that serves more than the socket names the options here.
	"$FYAI_BIN" serve ${REMOTE_SERVE_ARGS:-} >"$TEST_DIR/server.out" 2>"$TEST_DIR/server.err" &
	SERVER_PID=$!
	trap remote_test_cleanup EXIT
	# The socket accepts a connection once the server listens.
	"$PYTHON" - "$TEST_DIR/.fyai/unix/remote.sock" "$SERVER_PID" <<'PY'
import os, socket, sys, time
path, pid = sys.argv[1], int(sys.argv[2])
end = time.monotonic() + 10 * int(os.environ.get('FYAI_TIMEOUT_SCALE', '1'))
while True:
    s = socket.socket(socket.AF_UNIX)
    try:
        s.connect(path)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        os.kill(pid, 0)
        assert time.monotonic() < end, 'the server did not listen'
    finally:
        s.close()
PY
}

remote_test_stop() {
	kill "$SERVER_PID"
	wait "$SERVER_PID"
	trap fyai_test_cleanup EXIT
}
