#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai page review draws a sample page with each area painted and named, and
# lists the areas; a machine format gives only the list.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
out=$("$FYAI_BIN" --color=off page review question --width 70 --height 30) ||
    fail "page review did not draw the question sample"
for name in transcript "pane/blank" pane.cap "header.shown/blank" header \
        question ask.question_shown ask.from_agent "ask.options/selected" \
        "ask.options/other" "ask.options/selected_description" \
        "ask.options/other_description" \
        prompt status.hint status.row; do
    printf '%s\n' "$out" | grep -q -F " $name " ||
        fail "page review did not name the area $name"
done
# The row of the question keeps its text; the name stands at the right.
printf '%s\n' "$out" | grep -q -E '^  Patch  Apply the patch\?.* ask.question_shown $' ||
    fail "the name of a row covered its text"

json=$("$FYAI_BIN" page review popup --width 60 --height 12 --output json) ||
    fail "page review --output json failed"
"$PYTHON" - "$json" <<'PY' || fail "the areas of the popup are wrong"
import json
import sys

areas = json.loads(sys.argv[1])
names = [a["area"] for a in areas]
if "popup" not in names:
    raise SystemExit("no popup slot in %r" % names)
popup = next(a for a in areas if a["area"] == "popup" and a["kind"] == "slot")
if popup["kind"] != "slot" or popup["row"] != 1 or popup["height"] != 11:
    raise SystemExit("popup slot %r" % popup)
if "\x1b" in sys.argv[1] or "transcript" in names:
    raise SystemExit("the picture or a covered area reached the list")
PY
# --page reviews a document of the user: here the pane goes under the status.
DOCS=$(mktemp -d)
trap 'rm -rf "$DOCS"' EXIT
"$PYTHON" - "$TESTS_DIR/../data/page.yaml" "$DOCS" <<'PY' ||
import sys

s = open(sys.argv[1]).read()
above = "    - if: pane.above\n      page: pane\n"
below = "    - if: pane.below\n      page: pane"
if s.count(above) != 1 or s.count(below) != 1:
    raise SystemExit("the pane of data/page.yaml moved")
s = s.replace(above, "").replace(below, "    - page: pane")
open(sys.argv[2] + "/below.yaml", "w").write(s)
open(sys.argv[2] + "/bad.yaml", "w").write("broken: [\n")
PY
    fail "cannot write the page documents"
json=$("$FYAI_BIN" page review --page "$DOCS/below.yaml" --output json) ||
    fail "page review --page did not review the document"
"$PYTHON" - "$json" <<'PY' || fail "--page did not draw the document"
import json
import sys

rows = {a["area"]: a["row"] for a in json.loads(sys.argv[1])}
if not rows["head:1"] > rows["status.row"]:
    raise SystemExit("the pane is not under the status: %r" % rows)
PY
# A terminal as wide as the side layout asks for stands the tiles in a column
# of panels beside the transcript; one column less keeps the band.
side=$("$FYAI_BIN" page review --width 170 --height 40 --output json) ||
    fail "page review did not draw the side layout"
band=$("$FYAI_BIN" page review --width 169 --height 40 --output json) ||
    fail "page review did not draw the band layout"
"$PYTHON" - "$side" "$band" <<'PY' || fail "the layout does not follow the size"
import json
import sys

side = {a["area"]: a for a in json.loads(sys.argv[1])}
band = {a["area"]: a for a in json.loads(sys.argv[2])}
t, h1, s1, h2 = side["transcript"], side["head:1"], side["screen:1"], side["head:2"]
if t["col"] != 0 or t["width"] != 88 or t["height"] != 33:
    raise SystemExit("side transcript %r" % t)
if h1["col"] != 88 or h1["width"] != 82 or h1["row"] != 0:
    raise SystemExit("side head %r" % h1)
if s1["height"] != 24 or h2["row"] != 25 or h2["col"] != 88:
    raise SystemExit("side panels %r %r" % (s1, h2))
if "pane/blank" in side or "pane.cap" in side:
    raise SystemExit("the side layout kept the rows of the band")
if band["transcript"]["width"] != 169 or band["head:1"]["row"] <= band["transcript"]["row"]:
    raise SystemExit("band %r" % band["head:1"])
PY

if "$FYAI_BIN" page review --page "$DOCS/bad.yaml" >"$TEST_DIR/bad.out" 2>&1; then
    fail "page review drew a document that does not load"
fi
grep -q "bad.yaml is not used" "$TEST_DIR/bad.out" ||
    fail "page review did not say why the document is not used"
pass
