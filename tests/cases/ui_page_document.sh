#!/bin/bash
# SPDX-License-Identifier: MIT
# display/page names the file of a page document. A file that loads draws the
# page, and /page names it and shows the state of the frame. A file that does
# not load is not used: the session says why, and the embedded document draws
# the page.
set -eu
. "$(dirname "$0")/../harness.sh"

DOCS=$(mktemp -d)
trap 'rm -rf "$DOCS"' EXIT

"$PYTHON" - "$TESTS_DIR/../data/page.yaml" "$DOCS" <<'PY' || fail "cannot write the page documents"
import sys

src, out = sys.argv[1], sys.argv[2]
s = open(src).read()
header = '\n        - slot: { id: header, rows: 1 }\n'
if s.count(header) != 1:
    raise SystemExit("the header slot of data/page.yaml moved")
open(out + "/good.yaml", "w").write(
    s.replace(header, '\n        - row:\n            - text: "CUSTOM-PAGE"' +
              header))
# The keys of ask_text end with Escape; those of ask go on to the numbers.
keys = '            Escape: ask.dismiss\n          body:\n'
if s.count(keys) != 1:
    raise SystemExit("the keys of the ask_text case moved")
# The unknown action is in ask_text, a case that is not showing.
at = s.index(keys)
open(out + "/bad.yaml", "w").write(
    s[:at] + '            Escape: ask.dismiss\n            x: no-such-action\n'
    '          body:\n' + s[at + len(keys):])
PY

page()
{
    name=$1
    fyai_test_setup
    driver=0
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 FYAI_PTY_INPUT="/page" \
    FYAI_PTY_NEEDLE="Regions" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="drain:1" \
    FYAI_PTY_AFTER_PAUSE=0.5 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/renderer=page \
        --set "display/page=$DOCS/$name.yaml" -m mock-model -i ||
        driver=$?
    if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out" \
            "$TEST_DIR/trace.log" 2>/dev/null; then
        skip "this build has no page support"
    fi
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$TEST_DIR/pty.out" >&2
        fail "/page did not report the page with the $name document"
    fi
}

page good
grep -a -q "CUSTOM-PAGE" "$TEST_DIR/pty.out" ||
    fail "the page did not draw the document of display/page"
grep -a -q "The document is the file of" "$TEST_DIR/pty.out" ||
    fail "/page did not name the file of display/page"
grep -a -q "$DOCS/good.yaml" "$TEST_DIR/pty.out" ||
    fail "/page did not show the path of the document"
grep -a -q "State" "$TEST_DIR/pty.out" ||
    fail "/page did not show the state of the frame"
if grep -a -q "is not used" "$TEST_DIR/pty.out"; then
    fail "a document that loads was reported as not used"
fi

page bad
grep -a -q "display/page is not used" "$TEST_DIR/pty.out" ||
    fail "the session did not say that display/page is not used"
grep -a -q "no-such-action" "$TEST_DIR/pty.out" ||
    fail "the warning did not say why the document is not used"
grep -a -q "The document is the embedded" "$TEST_DIR/pty.out" ||
    fail "/page did not report the embedded document"
if grep -a -q "CUSTOM-PAGE" "$TEST_DIR/pty.out"; then
    fail "a rejected document drew the page"
fi

pass
