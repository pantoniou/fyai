#!/bin/bash
# SPDX-License-Identifier: MIT
# The page places the tiles of every display/work_layout. Three shells and a
# notice must stand in each layout: the same rows, the same size given to each
# shell, and the same reversed cells.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

shell()
{
    printf '%s' "!sh -c 'while :; do printf \"\\r\\033[K$1 %s \" \"\$(stty size)\"; sleep 0.2; done'"
}

# The size each shell is granted once the notice stands under the screens.
# The case waits for these on the screen, and for each head to be made again
# at the width of its grant, which leaves no head cut with an ellipsis.
sizes()
{
    case $1 in
    auto|columns) echo "FIRST 8 47|SECOND 8 46|THIRD 8 98" ;;
    stack) echo "FIRST 6 98|SECOND 4 98|THIRD 4 98" ;;
    main-top) echo "FIRST 8 98|SECOND 8 47|THIRD 8 46" ;;
    main-left) echo "FIRST 18 47|SECOND 8 46|THIRD 8 46" ;;
    esac
}

run_with()
{
    renderer=$1
    layout=$2
    settled=$(sizes "$layout" | sed 's/^/wait-screen:/; s/|/|wait-screen:/g')
    fyai_test_setup
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="$(shell FIRST)" \
    FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:$(shell SECOND)|wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:$(shell THIRD)|wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:/nosuch|wait-screen:unknown|$settled|frame:4|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1|"\
"send:/kill bang-2|wait-screen:stopping shell bang-2|"\
"send:/kill bang-3|wait-screen:stopping shell bang-3" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_min_tile_cols=30 \
        --set "display/work_layout=$layout" --set display/work_columns=2 \
        -m mock-model -i
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer-$layout.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer-$layout.trace"
}

LAYOUTS="auto columns stack main-top main-left"
for layout in $LAYOUTS; do
    run_with page "$layout"
    if grep -a -q "needs a libfytimui" "$CAPTURES/page-$layout.out" \
        "$CAPTURES/page-$layout.trace"; then
        skip "this build has no page support"
    fi
    grep -a -q "page: .*tiles=4" "$CAPTURES/page-$layout.trace" ||
        fail "the page did not place three shells and a notice in $layout"
done

pass
