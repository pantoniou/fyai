#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that the ignore rules face the user and the project and not the view: a
# view holds the whole project, build artifacts included, and what view/ignore and
# the .gitignore files name is left out of its diff, its list of changes and its
# apply.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"

mkdir -p project/build project/src project/doc
printf '*.o\nbuild/\n!keep.o\n' > project/.gitignore
printf '*.tmp\n' > project/doc/.gitignore
printf x > project/build/out
printf x > project/a.o
printf x > project/keep.o
printf 'int x;\n' > project/src/main.c
printf x > project/src/b.o
printf x > project/doc/n.tmp
printf d > project/doc/d.md
printf c > project/cfg.local
git init -q project

# paths_of VIEW [OPTION...]: the paths of the changes of a view, one for each line.
paths_of() {
	local view="$1"
	shift
	run_fyai --transient "$@" view diff --stat "$view" --output json
	assert_status 0
	"$PYTHON" -c 'import json, sys; print("\n".join(sorted(c["path"] for c in json.load(sys.stdin)["changes"])))' \
		< "$TEST_DIR/stdout"
}

# has PATHS PATH: whether a line of PATHS is PATH.
has() {
	printf '%s\n' "$1" | grep -qx "$2"
}

EDIT='echo new > build/new; echo y > z.o; echo m >> src/main.c; echo q > doc/q.tmp;
      echo ok > added.txt; echo l > cfg.local; echo e > keep.o'

# The view holds everything: the agent that works in it needs the artifacts.
run_fyai view create v project
assert_status 0
run_fyai view enter v sh -c 'test -e build/out && test -e a.o && test -e src/b.o && test -e doc/n.tmp && test -e cfg.local'
assert_status 0
run_fyai view ls v
assert_stdout_contains 'build/'
assert_stdout_contains 'a.o'
run_fyai view enter v sh -c "$EDIT"
assert_status 0

# The rules of the project and of the configuration leave paths out of the diff.
paths="$(paths_of v --set 'view/ignore=["cfg.local"]')"
for want in added.txt src/main.c keep.o; do
	has "$paths" "$want" || fail "$want is not in the changes: $paths"
done
for gone in build build/new z.o doc/q.tmp cfg.local; do
	! has "$paths" "$gone" || fail "$gone is in the changes: $paths"
done

# Without the rule of the configuration a path of it is a change. The rules of the
# files still apply.
paths="$(paths_of v)"
has "$paths" cfg.local || fail "cfg.local is not in the changes: $paths"
! has "$paths" z.o || fail "z.o is in the changes: $paths"

# The rules of the configuration outrank the files, in both directions.
paths="$(paths_of v --set 'view/ignore=["!z.o", "src/"]')"
has "$paths" z.o || fail "z.o is not in the changes: $paths"
! has "$paths" src/main.c || fail "src/main.c is in the changes: $paths"

# Without view/gitignore only the configuration counts.
paths="$(paths_of v --set view/gitignore=false)"
for want in build/new z.o doc/q.tmp; do
	has "$paths" "$want" || fail "$want is not in the changes: $paths"
done

# A scope narrows the diff as before, and the rules still apply inside it.
run_fyai view diff --stat v:build
assert_status 0
assert_stdout_not_contains 'build/new'

# The apply leaves the ignored paths alone.
run_fyai --transient --set 'view/ignore=["cfg.local"]' view apply v
assert_status 0
[ "$(cat project/added.txt)" = ok ] || fail 'the added file was not applied'
[ "$(sed -n 2p project/src/main.c)" = m ] || fail 'the change was not applied'
[ "$(cat project/keep.o)" = e ] || fail 'a path that a negation keeps was not applied'
[ ! -e project/z.o ] || fail 'an ignored file was applied'
[ ! -e project/build/new ] || fail 'a file of an ignored directory was applied'
[ ! -e project/doc/q.tmp ] || fail 'a file of an ignore file was applied'
[ "$(cat project/cfg.local)" = c ] || fail 'a path of view/ignore was applied'
[ "$(cat project/build/out)" = x ] || fail 'an ignored file of the project changed'

# The view still holds what it wrote there.
run_fyai view enter v sh -c 'test -e build/new && test -e z.o'
assert_status 0

pass
