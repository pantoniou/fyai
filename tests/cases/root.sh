#!/bin/bash
# SPDX-License-Identifier: MIT
# Root handles: a branch name is stable but its meaning moves, so automation
# pins one exact state with `root print` and `--root`. Such a run is read-only,
# and a handle is admitted only if it is a root the arena actually published.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai --set model=first
assert_status 0

# Capture the handle for this exact state.
"$FYAI_BIN" root print >"$TEST_DIR/root" 2>"$TEST_DIR/stderr" || fail "root print"
ROOT="$(cat "$TEST_DIR/root")"
[ -n "$ROOT" ] || fail "root print produced nothing"

# Move the live state on: a new model and a new branch.
run_fyai --set model=second
assert_status 0
run_fyai branch create later
assert_status 0

# A historical root reports its own HEAD, not the live context's HEAD.
run_fyai checkout later
assert_status 0
run_fyai root show "$ROOT"
assert_status 0
assert_stdout_contains "main"
assert_stdout_not_contains "later"
run_fyai checkout main
assert_status 0

# The live state has moved.
run_fyai config get model
assert_status 0
assert_stdout_contains "second"

# The pinned state has not.
run_fyai --root "$ROOT" config get model
assert_status 0
assert_stdout_contains "first"

# A branch created after the handle was taken is not visible through it.
run_fyai --root "$ROOT" branch
assert_status 0
assert_stdout_contains "main"
assert_stdout_not_contains "later"

# `root show` describes the pinned state.
run_fyai --root "$ROOT" root show
assert_status 0
assert_stdout_contains "$ROOT"

# --- a pinned run is read-only -------------------------------------------
run_fyai --root "$ROOT" --set model=third
assert_status 1
assert_stderr_contains "read-only"

# The refusal really did leave the arena alone.
run_fyai config get model
assert_status 0
assert_stdout_contains "second"

run_fyai --root "$ROOT" branch create nope
assert_status 1

# --- handles are validated ------------------------------------------------
# Neither a published handle nor a known branch. --root takes both forms, so
# this is settled against the arena rather than by the shape of the argument.
run_fyai --root zzzz branch
assert_status 1
assert_stderr_contains "names no root"

# Hexadecimal but never published: refused by the membership test, not by
# dereferencing it.
run_fyai --root deadbeef branch
assert_status 1
assert_stderr_contains "names no root"

# An existing hexadecimal branch name takes precedence over handle syntax.
run_fyai branch create cafe
assert_status 0
run_fyai --root cafe branch
assert_status 0
assert_stdout_contains "cafe"

# Zero is not a root.
run_fyai --root 0 branch
assert_status 1

# --- a handle can also be named symbolically -------------------------------
# `root print <ref>` maps a moving reference to a stable handle, and --root
# accepts either form. Only a root is ever emitted, never a direct reference.
"$FYAI_BIN" root print "main@{1}" >"$TEST_DIR/root2" 2>/dev/null || \
	fail "root print with a reference"
ROOT2="$(cat "$TEST_DIR/root2")"
[ -n "$ROOT2" ] || fail "root print <ref> produced nothing"

run_fyai --root "main@{1}" config get model
assert_status 0
run_fyai --root "$ROOT2" config get model
assert_status 0

# Both forms name the same state.
"$FYAI_BIN" --root "main@{1}" root print >"$TEST_DIR/a" 2>/dev/null
"$FYAI_BIN" --root "$ROOT2" root print >"$TEST_DIR/b" 2>/dev/null
cmp -s "$TEST_DIR/a" "$TEST_DIR/b" || fail "handle and reference differ"

# A reference naming no branch is refused like a bad handle.
run_fyai --root "nosuch@{1}" branch
assert_status 1

pass
