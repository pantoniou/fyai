#!/bin/bash
# SPDX-License-Identifier: MIT
# The page places the tiles of every display/work_layout. Three shells and a
# notice must stand in each layout: the same rows, the same size given to each
# shell, and the same reversed cells.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
fyai_test_on_exit 'rm -rf "$CAPTURES"'

# The steps that wait for the size each shell is granted once the notice
# stands under the screens, and for each head to be made again at the width of
# its grant, which leaves no head cut with an ellipsis.
sizes()
{
    case $1 in
    auto|columns) set -- "FIRST 8 47" "SECOND 8 46" "THIRD 8 98" ;;
    stack) set -- "FIRST 6 98" "SECOND 4 98" "THIRD 4 98" ;;
    main-top) set -- "FIRST 8 98" "SECOND 8 47" "THIRD 8 46" ;;
    main-left) set -- "FIRST 18 47" "SECOND 8 46" "THIRD 8 46" ;;
    esac
    printf '  - wait-screen: "%s"\n' "$@"
}

run_with()
{
    renderer=$1
    layout=$2
    fyai_test_setup
    {
        cat <<'PTY'
cols: 100
timeout: 20
wait_timeout: 10
steps:
  - type: "!sh -c 'while :; do printf \"\\r\\033[KFIRST %s \" \"$(stty size)\"; sleep 0.2; done'"
  - wait: FIRST
    timeout: session
  - wait-screen: "Ctrl-]"
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "!sh -c 'while :; do printf \"\\r\\033[KSECOND %s \" \"$(stty size)\"; sleep 0.2; done'"
  - wait-screen: "Ctrl-]"
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "!sh -c 'while :; do printf \"\\r\\033[KTHIRD %s \" \"$(stty size)\"; sleep 0.2; done'"
  - wait-screen: "Ctrl-]"
  - write: "\x1d"
  - wait-gone: "Ctrl-]"
  - type: "/nosuch"
  - wait-screen: unknown
PTY
        sizes "$layout"
        cat <<'PTY'
  - frame: 4
  - type: "/kill bang-1"
  - wait-screen: stopping shell bang-1
  - type: "/kill bang-2"
  - wait-screen: stopping shell bang-2
  - type: "/kill bang-3"
  - wait-screen: stopping shell bang-3
PTY
    } | FYAI_TRACE="$TEST_DIR/trace.log" pty_run "$TEST_DIR/pty.out" \
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
