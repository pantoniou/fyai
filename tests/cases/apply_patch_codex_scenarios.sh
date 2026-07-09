#!/bin/bash
# SPDX-License-Identifier: MIT
# Runs fyai's apply_patch tool against the upstream codex-rs apply-patch
# end-to-end scenario fixtures (tests/fixtures/apply_patch_scenarios/,
# vendored verbatim from openai/codex codex-rs/apply-patch/tests/fixtures).
# Each scenario is judged purely on final filesystem state, exactly as the
# codex-rs harness does - the exit status/result string of `fyai tool
# apply_patch` is not asserted, only that input/ + patch.txt -> expected/.
#
# One upstream scenario is intentionally skipped:
# 015_failure_after_partial_success_leaves_changes expects a failed patch to
# leave earlier, already-applied ops committed to disk. fyai deliberately
# keeps all-or-nothing atomic commit semantics instead (see
# src/fyai_patch.c's patch_ops_commit()), so that scenario's expectation does
# not - and should not - hold here.
set -u
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

SCEN_ROOT="$TESTS_DIR/fixtures/apply_patch_scenarios"
[ -d "$SCEN_ROOT" ] || fail "missing $SCEN_ROOT"

failures=0
total=0

for scen in "$SCEN_ROOT"/*/; do
	name="$(basename "$scen")"
	[ -f "$scen/patch.txt" ] || continue
	[ "$name" = "015_failure_after_partial_success_leaves_changes" ] && continue
	total=$((total + 1))

	work="$TEST_DIR/scen-$name"
	rm -rf "$work"
	mkdir -p "$work"
	[ -d "$scen/input" ] && cp -r "$scen/input/." "$work/"

	patch_args="$("$PYTHON" -c '
import json, sys
patch = open(sys.argv[1], "r").read()
print(json.dumps({"patch": patch}))
' "$scen/patch.txt")"

	( cd "$work" && printf '%s' "$patch_args" | "$FYAI_BIN" tool apply_patch >/dev/null 2>&1 )

	if [ -d "$scen/expected" ]; then
		if ! diff -rq "$scen/expected" "$work" \
			--exclude=patch.txt >/tmp/apply_patch_diff.$$ 2>&1; then
			echo "FAIL scenario $name:" >&2
			cat /tmp/apply_patch_diff.$$ >&2
			failures=$((failures + 1))
		fi
		rm -f /tmp/apply_patch_diff.$$
	fi
done

[ "$total" -gt 0 ] || fail "no scenarios found under $SCEN_ROOT"
[ "$failures" -eq 0 ] || fail "$failures/$total codex apply_patch scenarios failed"

pass
