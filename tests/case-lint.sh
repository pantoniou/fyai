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

# Every AFTER step kind the driver accepts. A misspelled step fails only
# after the session budget; the driver also validates, but the suite
# should not depend on one layer.
for f in "$cases_dir"/*.sh; do
	while IFS= read -r script; do
		[ -n "$script" ] || continue
		old_ifs="$IFS"
		IFS='|'
		# shellcheck disable=SC2086
		set -f
		for step in $script; do
			case "$step" in
			send:*|raw:*|resize:*|wait:*|wait-frame:*|release:*|drain:*|settle:*|snapshot|"") ;;
			# Composed in a shell variable elsewhere; the driver
			# validates the expanded script at runtime.
			\$*) ;;
			*)
				echo "case-lint: unknown AFTER step '$step': $f" >&2
				bad=1
				;;
			esac
		done
		set +f
		IFS="$old_ifs"
	done < <(grep -oE 'FYAI_PTY_AFTER="[^"]*"' "$f" | sed 's/^FYAI_PTY_AFTER="//;s/"$//')
done

# Steps composed in shell variables ($AFTER, $ZOOM_AFTER) are invisible
# to the grep above; the driver validates them at runtime instead.

[ "$bad" = 0 ] || exit 1
echo "PASS: case-lint.sh"
