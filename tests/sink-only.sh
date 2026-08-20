#!/bin/bash
# SPDX-License-Identifier: MIT
# One component owns every byte the user sees. A direct write to standard
# output or standard error outside the rendering sink defeats a second
# backend, so it fails here unless tests/sink-only-allow.txt explains it.
set -eu

src_dir="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
allow="$src_dir/tests/sink-only-allow.txt"

[ -f "$allow" ] || { echo "sink-only: missing $allow" >&2; exit 99; }

# The allowed files, comments and blank lines removed.
allowed=()
while IFS= read -r entry; do
	allowed+=("$entry")
done < <(grep -vE '^[[:space:]]*(#|$)' "$allow")

is_allowed() {
	local f="$1" a
	for a in "${allowed[@]}"; do
		[ "$f" = "$a" ] && return 0
	done
	return 1
}

# A write reaches the user only through stdout/stderr. printf/puts/putchar
# always do; the f* forms only when the stream is stdout or stderr, because
# the same calls build Markdown into an open_memstream() buffer, which is
# construction rather than output.
pattern='\b(printf|puts|putchar)[[:space:]]*\(|\b(fprintf|fputs|fputc|fwrite|vfprintf)[[:space:]]*\([^;]*\b(stdout|stderr)\b'

# A stream handed to another function reaches the user just as directly. The
# call that writes it is then in a different file, and the f* pattern above
# cannot see it, so naming stdout or stderr in any call argument counts too.
pattern_indirect='[a-z_][a-z0-9_]*[[:space:]]*\(([^;]*,)?[[:space:]]*(stdout|stderr)[[:space:]]*[,)]'

status=0
while IFS= read -r file; do
	rel="${file#"$src_dir"/}"
	is_allowed "$rel" && continue
	hits=$(grep -nE "$pattern|$pattern_indirect" "$file" |
		grep -vE '\b(snprintf|asprintf|vsnprintf|vasprintf)\b' |
		grep -vE '^[0-9]+:[[:space:]]*(\*|//|/\*)' |
		grep -vE '\b(fileno|isatty|terminal_is_tty|clearerr|fflush|setvbuf|dup|dup2|freopen)[[:space:]]*\(' ||
		true)
	[ -z "$hits" ] && continue
	status=1
	echo "sink-only: $rel writes outside the sink:" >&2
	echo "$hits" | sed 's/^/  /' >&2
done < <(find "$src_dir/src" -name '*.c' -print | LC_ALL=C sort)

if [ "$status" -ne 0 ]; then
	echo >&2
	echo "Route these through src/fyai_sink.c (fyai_result(), fyai_report()," >&2
	echo "fyai_sink_write(), fyai_sink_markdown()), or add the file to" >&2
	echo "tests/sink-only-allow.txt with the reason it cannot be." >&2
	exit 1
fi
echo "sink-only: no writes outside the sink"
