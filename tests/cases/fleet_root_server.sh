#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify root-server routing for subprojects: a client walks up to the nearest
# running ancestor server, which lazily starts and routes to the nearest
# project server owning the client's directory.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

ROOT="$TEST_DIR/work"
# project DIR MODEL: initialize a project at DIR configured with MODEL.
project() {
	mkdir -p "$1/.git"
	(cd "$1" && "$FYAI_BIN" init >/dev/null 2>&1 &&
		"$FYAI_BIN" config set model "$2" >/dev/null 2>&1) ||
		fail "cannot make the project '$1'"
	[ -d "$1/.fyai/arena" ] || fail "the project '$1' has no arena"
}
project "$ROOT" root-model
project "$ROOT/a" a-model
# `libs` is a plain directory; `libs/inner` is a nested project under root.
project "$ROOT/libs/inner" inner-model
mkdir -p "$ROOT/a/src" "$ROOT/libs/inner/deep" "$ROOT/notes"

# model_at DIR: query `model` via `--remote .` from DIR.
model_at() {
	(cd "$1" && "$FYAI_BIN" --remote . config get model)
}

# With no server running, `--remote .` must fail.
set +e
(cd "$ROOT/a/src" && "$FYAI_BIN" --remote . config get model) \
	>"$TEST_DIR/none.out" 2>"$TEST_DIR/none.err"
status=$?
set -e
[ "$status" -ne 0 ] || fail "a client found a server where none runs"

(cd "$ROOT" && "$FYAI_BIN" serve --daemon) >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '(cd "$ROOT" && "$FYAI_BIN" serve stop >/dev/null 2>&1) || true'

[ "$(model_at "$ROOT")" = root-model ] || fail "the root does not serve itself"
[ "$(model_at "$ROOT/notes")" = root-model ] ||
	fail "a directory with no project is not of the root"
[ "$(model_at "$ROOT/a/src")" = a-model ] ||
	fail "a project below the root is not served by its own server"
[ "$(model_at "$ROOT/libs/inner/deep")" = inner-model ] ||
	fail "a project below a directory is not served by its own server"
# Subsequent requests reuse the project server already started by the root.
[ "$(model_at "$ROOT/a")" = a-model ] || fail "the project server is not kept"

(cd "$ROOT" && "$FYAI_BIN" serve status) >"$TEST_DIR/status.out"
grep -q "root: $ROOT/a\$" "$TEST_DIR/status.out" ||
	fail "the root server does not list the project: $(cat "$TEST_DIR/status.out")"
grep -q "root: $ROOT/libs/inner\$" "$TEST_DIR/status.out" ||
	fail "the root server does not list the project below a directory"
[ "$(grep -c 'started the project server' "$ROOT/.fyai/logs/serve.log")" -eq 2 ] ||
	fail "the root server did not start one server for each project: $(cat "$ROOT/.fyai/logs/serve.log")"

# FYAI_CEILING_DIRECTORIES blocks upward traversal before reaching the root.
set +e
(cd "$ROOT/a/src" && FYAI_CEILING_DIRECTORIES="$ROOT" "$FYAI_BIN" --remote . \
	config get model) >"$TEST_DIR/ceiling.out" 2>"$TEST_DIR/ceiling.err"
status=$?
set -e
[ "$status" -ne 0 ] || fail "the walk went past a ceiling"

# Stopping the root server shuts down all spawned project servers as well.
pids=$(sed -n 's/^ *pid: //p' "$TEST_DIR/status.out")
(cd "$ROOT" && "$FYAI_BIN" serve stop) >/dev/null
for pid in $pids; do
	"$PYTHON" -I "$TESTS_DIR/wait_exit.py" "$pid" $((100 * FYAI_TIMEOUT_SCALE)) ||
		fail "the server $pid outlived the root server"
done
[ ! -e "$ROOT/.fyai/unix/remote.sock" ] || fail "the root server left its socket"
pass
