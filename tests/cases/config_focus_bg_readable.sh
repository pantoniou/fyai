#!/bin/bash
# SPDX-License-Identifier: MIT
# A colour of the user's own asks for no detection, so display/focus_bg is the
# one display setting that can leave the input area unreadable. A literal the
# theme's text cannot be read on is reported, and one it can is not.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

check()
{
    theme=$1
    ground=$2
    want=$3
    "$FYAI_BIN" -k test-key -m mock-model --color on --theme "$theme" \
        --transient --set display/markdown=true \
        --set display/focus_bg="'$ground'" \
        config get display/focus_bg >"$TEST_DIR/out" 2>&1 || \
        fail "config get failed for $theme $ground"
    if grep -q "display/focus_bg: $ground" "$TEST_DIR/out"; then
        got=warned
    else
        got=quiet
    fi
    [ "$got" = "$want" ] || \
        fail "$theme with $ground: $got, expected $want"
}

# The text of the theme is on the other side of the ground.
check ember:light "#181828" warned
check ember:dark "#f0f0f0" warned
# A ground the text reads on says nothing.
check ember:light "#f0f0f0" quiet
check ember:dark "#181828" quiet
# The named grounds follow the palette and are never reported.
check ember:light theme quiet
check ember:light reverse quiet

pass
