#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the commands that address a path of a view as NAME:PATH: ls, diff, rm
# and cp, and that a path that is missing, on either side, is an answer and not a
# failure of the command.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"

mkdir -p project/src project/doc
printf 'one\n' > project/a.txt
printf 'main\n' > project/src/main.c
printf 'doc\n' > project/doc/d.md
printf 'host\n' > project/host.txt
printf 'hidden\n' > project/.hidden
printf 'zed\n' > project/zed.txt
git init -q project

run_fyai view create v project
assert_status 0

# ls: the root, a directory, a file, and a path that is not there.
run_fyai view ls v
assert_status 0
assert_stdout_contains 'a.txt'
assert_stdout_contains 'src/'
run_fyai view ls v:src
assert_status 0
assert_stdout_contains 'main.c'
assert_stdout_not_contains 'a.txt'
run_fyai view ls v:src/main.c
assert_status 0
assert_stdout_contains 'main.c'
assert_stdout_contains 'src'

# The root is NAME, NAME:/ and NAME:. . A name with a dot is left out as ls leaves it
# out, and --all lists it with . and .. . The names come sorted.
for root in v v:/ v:. v:./; do
	run_fyai view ls "$root"
	assert_status 0
	assert_stdout_contains 'a.txt'
	assert_stdout_contains 'src/'
	assert_stdout_not_contains '.hidden'
done
run_fyai view ls v:/src
assert_status 0
assert_stdout_contains 'main.c'
run_fyai view ls --output json v
assert_status 0
python3 - "$TEST_DIR/stdout" <<'PY' || fail 'ls did not sort the names'
import json, sys
names = [r["name"] for r in json.load(open(sys.argv[1]))]
assert names == sorted(names), names
PY
run_fyai view ls -a v:/
assert_status 0
assert_stdout_contains '.hidden'
assert_stdout_contains '.'
assert_stdout_contains '..'
run_fyai view ls -a --output json v:src
assert_status 0
python3 - "$TEST_DIR/stdout" <<'PY' || fail 'ls -a did not list . and .. first'
import json, sys
names = [r["name"] for r in json.load(open(sys.argv[1]))]
assert names[:2] == ['.', '..'], names
PY
run_fyai view ls v:nothing
assert_status 1
assert_stderr_contains "has no nothing"
run_fyai view ls missing-view:src
assert_status 1

# rm removes in the view, and not in the project.
run_fyai view rm v:a.txt v:doc
assert_status 0
[ -e project/a.txt ] || fail 'rm changed the project'
run_fyai view ls v
assert_stdout_not_contains 'a.txt'
assert_stdout_not_contains 'doc/'
run_fyai view rm v:a.txt
assert_status 1
run_fyai view rm --force v:a.txt v:nothing
assert_status 0
run_fyai view rm v:.git
assert_status 1
run_fyai view rm a.txt
assert_status 1
assert_stderr_contains 'NAME:PATH'

# diff takes the same address, and a path that one side lacks is selected.
run_fyai view diff --stat v:a.txt
assert_status 0
assert_stdout_contains 'deleted a.txt'
run_fyai view diff --stat v:doc
assert_status 0
assert_stdout_contains 'deleted doc/d.md'
assert_stdout_not_contains 'a.txt'
run_fyai view diff --stat v:src
assert_status 0
assert_stdout_not_contains 'deleted'
# An unchanged path is an empty diff; a path of neither side is a mistake.
run_fyai view diff --stat v:src/main.c
assert_status 0
run_fyai view diff --stat v:neither
assert_status 1
assert_stderr_contains 'no path'

# cp from the project into the view: a directory that ends in a slash, and a new name.
printf 'edited\n' > project/src/main.c
run_fyai view cp src/main.c host.txt v:copied/
assert_status 0
run_fyai view ls v:copied
assert_stdout_contains 'main.c'
assert_stdout_contains 'host.txt'
run_fyai view cp host.txt v:renamed.txt
assert_status 0
run_fyai view cp host.txt v:three.txt
assert_status 0
run_fyai view cp src/main.c host.txt v:single
assert_status 1
assert_stderr_contains 'ends in a slash'
# A directory goes with what it holds, and a path of the project is taken whole.
run_fyai view cp src v:srccopy
assert_status 0
run_fyai view ls v:srccopy
assert_stdout_contains 'main.c'
# The view records it; the project is unchanged.
run_fyai view diff --stat v:renamed.txt
assert_stdout_contains 'added renamed.txt'
[ ! -e project/renamed.txt ] || fail 'cp into a view changed the project'
[ ! -e project/copied ] || fail 'cp into a view made a directory in the project'

# cp from the view into the project replaces what is there with no check.
run_fyai view cp v:renamed.txt out/
assert_status 0
[ "$(cat project/out/renamed.txt)" = host ] || fail 'cp to the project did not write the file'
# "." and "./" are the top of the project and "dir/." is the directory: each takes
# the source by its name, as cp does.
run_fyai view cp v:renamed.txt .
assert_status 0
[ "$(cat project/renamed.txt)" = host ] || fail 'cp to . did not write the file'
printf 'stale\n' > project/renamed.txt
run_fyai view cp v:renamed.txt ./
assert_status 0
[ "$(cat project/renamed.txt)" = host ] || fail 'cp to ./ did not write the file'
run_fyai view cp v:renamed.txt out/.
assert_status 0
[ "$(cat project/out/renamed.txt)" = host ] || fail 'cp to out/. did not write the file'
run_fyai view cp v:renamed.txt v:.
assert_status 1
printf 'late\n' > project/late.txt
run_fyai view cp late.txt v:.
assert_status 0
run_fyai view ls v:late.txt
assert_status 0
printf 'later\n' > project/later.txt
run_fyai view cp later.txt v:./
assert_status 0
run_fyai view ls v:later.txt
assert_status 0
printf 'changed in the project\n' > project/host.txt
run_fyai view cp v:renamed.txt host.txt
assert_status 0
[ "$(cat project/host.txt)" = host ] || fail 'cp to the project did not replace the file'
run_fyai view cp v:copied project-copy
assert_status 0
[ "$(cat project/project-copy/main.c)" = edited ] || fail 'cp of a directory to the project'

# A missing source fails before it changes anything, on either side.
run_fyai view cp v:nothing nothing.txt
assert_status 1
[ ! -e project/nothing.txt ] || fail 'a missing source made a file'
run_fyai view cp nothing.txt v:nothing.txt
assert_status 1
run_fyai view ls v:nothing.txt
assert_status 1
# One view and the project, and nothing of the reserved names.
run_fyai view cp v:renamed.txt v:other.txt
assert_status 1
run_fyai view cp .git/config v:config
assert_status 1
run_fyai view cp host.txt v:../escape
assert_status 1

# apply writes the removal to the project, so rm and apply are the pair.
run_fyai view apply v a.txt doc
assert_status 0
[ ! -e project/a.txt ] || fail 'apply did not remove the file that rm removed'
[ ! -e project/doc ] || fail 'apply did not remove the directory that rm removed'

pass
