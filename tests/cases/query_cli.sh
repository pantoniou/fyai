#!/bin/bash
# SPDX-License-Identifier: MIT
# `fyai query branches` answers from the arena of the process: a page, the
# cursor of the next page, the rev of the data and, when the rev is given
# back, an answer with no rows. A cursor of data that has since changed is
# refused.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
for n in a b c d; do
	run_fyai branch create "main/$n"
	assert_status 0
done

run_fyai query branches --limit 2 --output json
assert_status 0
cp "$TEST_DIR/stdout" "$TEST_DIR/page1.json"
"$PYTHON" - "$TEST_DIR/page1.json" >"$TEST_DIR/cursor" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
assert a['what'] == 'branches' and len(a['rows']) == 2, a
assert a['total'] == 5 and a['next'] and isinstance(a['rev'], int), a
# The newest branch first, and a row has the rev of its entry.
assert a['rows'][0]['branch'] == 'main/d' and a['rows'][0]['rev'], a
print(a['next'])
PY
CURSOR="$(cat "$TEST_DIR/cursor")"
REV="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["rev"])' "$TEST_DIR/page1.json")"

# The next page, and the last one has no cursor.
run_fyai query branches --limit 2 --after "$CURSOR" --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
assert [r['branch'] for r in a['rows']] == ['main/b', 'main/a'], a
assert a['next']
PY

# The data as it was: no rows.
run_fyai query branches --if-rev "$REV" --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" "$REV" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
assert a == {'unchanged': True, 'rev': int(sys.argv[2])}, a
PY

# The data changed: the cursor is refused, and the rev is not the same.
run_fyai branch create main/e
assert_status 0
run_fyai query branches --limit 2 --after "$CURSOR"
assert_status 1
assert_stderr_contains "stale_cursor"
run_fyai query branches --if-rev "$REV" --output json
assert_status 0
assert_stdout_contains '"rows"'

# A cursor that no answer made, and a query that nobody answers.
run_fyai query branches --after garbage
assert_status 1
assert_stderr_contains "bad_request"
run_fyai query nothing
assert_status 1
pass
