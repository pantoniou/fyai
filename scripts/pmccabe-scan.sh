#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

pmccabe=${1:?pmccabe executable is required}
source_dir=${2:?source directory is required}
threshold=${PMCCABE_THRESHOLD:-20}
limit=${PMCCABE_LIMIT:-5}

printf 'Functions with McCabe complexity greater than %s (top %s):\n' \
	"$threshold" "$limit"

# Keep this as a report, rather than a pass/fail check. The existing source
# has functions above the threshold. The report gives each cleanup pass a
# bounded work list without changing the build result.
"$pmccabe" -v "$source_dir"/*.c |
	awk -F '\t' -v threshold="$threshold" \
		'$1 ~ /^[0-9]+$/ && $1 > threshold { print }' |
	sort -k1,1nr |
	head -n "$limit"
