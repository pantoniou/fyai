#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai-symbolize.sh - resolve a fatal-signal backtrace to file:line.
#
# The crash handler prints function names and offsets, which is what a
# signal handler can do safely. This script does the rest offline: feed it
# the captured report and it prints each frame with its source position.
#
# Usage:
#   fyai ... 2> crash.log; scripts/fyai-symbolize.sh crash.log
#   scripts/fyai-symbolize.sh < crash.log
#   scripts/fyai-symbolize.sh -b build/fyai crash.log
#
# The -b binary defaults to the build tree beside this script. Frames from
# shared libraries resolve against the libraries of this host, so symbolize
# on the machine that produced the report, with the same binary and
# libraries. Debuginfod-style separate debug files work: addr2line reads
# .gnu_debuglink and .debug files wherever gdb would find them.
#
# A frame holds a return address, which is the instruction after the call.
# Subtract one byte so the call itself is reported - except for the top
# frame, which is the faulting instruction. A frame of `exe(+offset)` has
# no symbol to anchor, so it takes the top-frame rule: no adjustment.
set -u

bin=""
while [ $# -gt 0 ]; do
	case "$1" in
	-b) bin="${2:-}"; shift 2 ;;
	-*) echo "usage: $0 [-b binary] [file]" >&2; exit 2 ;;
	*) break ;;
	esac
done
[ -n "$bin" ] || bin="$(cd "$(dirname "$0")/../build" && pwd)/fyai"
[ -x "$bin" ] || { echo "fyai-symbolize: not executable: $bin" >&2; exit 99; }
[ $# -gt 0 ] && exec < "$1"

command -v addr2line >/dev/null 2>&1 || {
	echo "fyai-symbolize: addr2line not found" >&2; exit 99; }

# The symbol table of the binary, for anchoring name+offset frames.
symbols="$(nm "$bin" 2>/dev/null)" || {
	echo "fyai-symbolize: cannot read symbols from $bin" >&2; exit 1; }

top=1
grep -E '^(fyai: fatal signal|[^ ]+\(|[^ ]+\[0x[0-9a-f]+])' | while IFS= read -r line; do
	case "$line" in
	"fyai: fatal signal"*)
		echo "$line"
		top=1
		continue
		;;
	esac

	# path(sym+off)[addr] or path(+off)[addr]; keep the line otherwise.
	rest="${line#*(}"
	case "$rest" in
	"$line") echo "$line"; continue ;;
	esac
	obj="${line%%(*}"
	inside="${rest%%)*}"
	sym="${inside%%+*}"
	off="${inside##*+}"

	# Already symbolized output passes through unchanged.
	case "$line" in *" -> "*) echo "$line"; continue ;; esac

	# Shared objects resolve against this host; only the fyai binary is
	# pinned to -b.
	case "$obj" in
	*libc.so*|*libpthread*|*libcurl*|*libssl*|*libcrypto*|*libfyaml*|*libfymd4c*|*libfyts*|*libfytimui*|*libfymermaid*|*libfyvterm*|*libz*|*libm*|*ld-linux*)
		target="$obj"
		;;
	*) target="$bin" ;;
	esac

	# Anchor a named frame at its symbol; an offset-only frame has no
	# symbol and keeps the top-frame rule. Subtract one elsewhere so
	# the call, not the return address, is reported.
	if [ -n "$sym" ] && [ "$sym" != "$inside" ]; then
		if [ "$target" = "$bin" ]; then
			table="$symbols"
		else
			table="$(nm -D "$target" 2>/dev/null)"
		fi
		# A dynamic symbol carries a version suffix
		# (epoll_wait@@GLIBC_2.3.2); match the name before it.
		base="$(echo "$table" | awk -v s="$sym" \
			'$3 == s || index($3, s "@@") == 1 || index($3, s "@") == 1 \
				{ print "0x" $1; exit }')"
		[ -n "$base" ] || { echo "$line"; continue; }
		addr="$(printf '0x%x' $((base + off - (top ? 0 : 1))))"
	else
		[ "$top" = 1 ] || off="$(printf '0x%x' $((off - 1)))"
		addr="$off"
	fi

	resolved="$(addr2line -e "$target" -f -C "$addr" 2>/dev/null)" || {
		echo "$line"; continue; }
	fn="${resolved%%$'\n'*}"
	loc="${resolved#*$'\n'}"
	if [ "$fn" = "??" ] && [ "$loc" = "??:0" ]; then
		echo "$line"
	else
		echo "$line -> $fn $loc"
	fi
	top=0
done
