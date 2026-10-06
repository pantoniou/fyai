#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that gc collects what no view reaches in the project storage: the
# manifests, the blobs and the runtime trees of a removed view, and that it
# leaves what a stored view needs, and what is younger than the grace.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mkdir project
cd project
printf 'display:\n  markdown: false\n  renderer: stack\n  screen: inline\n  work_controls: none\n  work_zoom_rows: full\n' > config.yaml
"$FYAI_BIN" init >/dev/null 2>&1 || fail "fyai init"
rm -f config.yaml

count() {
	find ".fyai/$1" -maxdepth "${2:-1}" -type f | wc -l
}
trees() {
	find .fyai/views -maxdepth 1 -name 'view-*' -type d | wc -l
}

printf one > file
run_fyai view create keep .
assert_status 0
printf two > file
run_fyai view create drop .
assert_status 0
[ "$(trees)" = 2 ] || fail "expected two view trees"
blobs=$(count objects/blake3 2)
manifests=$(count manifests)

# A view that is stored keeps everything: nothing is garbage yet.
run_fyai gc --grace 0
assert_status 0
assert_stdout_contains 'removed 0 manifests, 0 objects and 0 view trees'
[ "$(count manifests)" = "$manifests" ] || fail "gc removed a live manifest"

run_fyai view remove drop
assert_status 0

# The files of the removed view are young: the grace keeps them.
run_fyai gc
assert_status 0
assert_stdout_contains 'removed 0 manifests, 0 objects and 0 view trees'
[ "$(trees)" = 2 ] || fail "gc removed a tree inside the grace"

run_fyai gc --grace 0
assert_status 0
assert_stdout_contains 'removed 1 manifests'
assert_stdout_contains '1 view trees'
[ "$(trees)" = 1 ] || fail "the tree of the removed view stays"
[ "$(count manifests)" -lt "$manifests" ] || fail "the manifest of the removed view stays"
[ "$(count objects/blake3 2)" -lt "$blobs" ] || fail "the blob of the removed view stays"
# An object is below the directory of its first digest byte, and gc removes a
# directory that it emptied.
[ -z "$(find .fyai/objects/blake3 -maxdepth 1 -type f -name '[0-9a-f]*')" ] || fail "an object is not in a shard directory"
[ -z "$(find .fyai/objects/blake3 -mindepth 1 -type d -empty)" ] || fail "gc left an empty shard directory"

# The view that stays still reads.
run_fyai view diff keep --stat
assert_status 0
run_fyai view show keep
assert_status 0
assert_stdout_contains 'keep'

# A second collection finds nothing.
run_fyai gc --grace 0
assert_status 0
assert_stdout_contains 'removed 0 manifests, 0 objects and 0 view trees'

# --tips drops every ref log and keeps the tip of the branch and the views.
run_fyai gc --grace 0 --tips
assert_status 0
run_fyai list reflog --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
assert len(rows) == 1, rows
PY
run_fyai view show keep
assert_status 0
pass
