#!/bin/bash
# SPDX-License-Identifier: MIT
# A `#` comment inside a `\`-continued shell statement breaks the
# continuation: the assignments before it become bare shell variables
# (never exported to the driver), and the steps after it still run.
# The driver then runs on defaults and fails minutes later describing
# only the symptom. Keep comments above the statement, never inside it.
set -eu

cases_dir="$(cd "$(dirname "$0")/cases" && pwd)"
bad=0

for f in "$cases_dir"/*.sh; do
	if awk 'prev_cont && /^[[:space:]]*#/ {found=1; exit 1} {prev_cont = (/\\$/ ? 1 : 0)}' "$f"; then
		:
	else
		echo "case-lint: comment inside continuation: $f" >&2
		awk 'prev_cont && /^[[:space:]]*#/ {print FILENAME":"FNR": "$0}' "$f" >&2
		bad=1
	fi
done

# The driver reads a YAML script (doc/pty-driver.md) and checks it before the
# session starts. An FYAI_PTY_ variable in front of pty_run is not read by the
# driver, so the case would run on defaults and prove less than it says.
# pty_resize.py and the emoji model of screen.py read their own variables.
for f in "$cases_dir"/*.sh; do
	case "$f" in
	*/shell_tty_resize.sh|*/ui_page_emoji_width.sh) continue ;;
	esac
	if grep -q 'FYAI_PTY_' "$f"; then
		echo "case-lint: FYAI_PTY_ variable in a case: $f" >&2
		grep -n 'FYAI_PTY_' "$f" >&2
		bad=1
	fi
done

# A case waits for a state and never for a time. The sleep step exists for the
# driver self-test of a wait that is itself under test.
for f in "$cases_dir"/*.sh; do
	if grep -n '^[[:space:]]*- sleep:' "$f" >&2; then
		echo "case-lint: sleep step in a case: $f" >&2
		bad=1
	fi
done

# The harness owns the EXIT trap. A trap that a case sets replaces the one of
# the harness, or is replaced by it at fyai_test_setup, and a directory is left
# behind: a case registers its cleanup with fyai_test_on_exit.
for f in "$cases_dir"/*.sh; do
	if grep -n '^[[:space:]]*trap .*[[:space:]]EXIT[[:space:]]*$' "$f" >&2; then
		echo "case-lint: EXIT trap in a case, use fyai_test_on_exit: $f" >&2
		bad=1
	fi
done

[ "$bad" = 0 ] || exit 1
echo "PASS: case-lint.sh"
