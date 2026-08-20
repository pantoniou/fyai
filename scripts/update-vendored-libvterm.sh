#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Automates the mechanical half of updating third_party/libvterm. See
# third_party/libvterm/PROVENANCE.md for the two-step process this script
# covers, and for the judgment calls it does NOT automate: deciding which
# neovim/neovim commits to port, and hand-porting them onto this copy's
# plain chars[] cell representation.
set -euo pipefail

BASE_REPO=https://github.com/neovim/libvterm
PATCH_REPO=https://github.com/neovim/neovim
PATCH_SUBDIR=src/nvim/vterm

root_dir=$(cd "$(dirname "$0")/.." && pwd)
vendor_dir="$root_dir/third_party/libvterm"
provenance="$vendor_dir/PROVENANCE.md"
cache_dir="$root_dir/.cache/update-vendored-libvterm"

usage() {
	cat <<'EOF'
usage: scripts/update-vendored-libvterm.sh <command> [args]

commands:
  base [ref]
      Refresh the vendored source from neovim/libvterm. Clones (or
      updates a cached clone of) that repo at <ref> (default: its
      default branch), copies the tracked file set into
      third_party/libvterm, and rewrites the commit hash and date at
      the top of PROVENANCE.md. Prints a reminder that the "Patches
      carried" section still needs re-checking against the new base.

  patches [since]
      List neovim/neovim commits touching src/nvim/vterm after
      <since> (default: the "last commit considered" hash recorded in
      PROVENANCE.md), oldest first, one line per commit. This is a
      triage list, not a patch set: read PROVENANCE.md's "Patches
      carried from Neovim's own copy" section for how to decide port
      vs skip, then hand-port what applies and update that section
      and the "last commit considered" hash yourself.

  help
      Show this message.
EOF
}

clone_or_update() {
	local repo="$1" dir="$2"

	if [ -d "$dir/.git" ]; then
		git -C "$dir" remote set-url origin "$repo"
		git -C "$dir" fetch --tags origin
	else
		mkdir -p "$(dirname "$dir")"
		git clone "$repo" "$dir"
	fi
}

cmd_base() {
	local ref="${1:-}"
	local dir="$cache_dir/libvterm"

	clone_or_update "$BASE_REPO" "$dir"
	git -C "$dir" checkout -q "${ref:-$(git -C "$dir" symbolic-ref --short refs/remotes/origin/HEAD | sed 's#^origin/##')}"

	local commit date
	commit=$(git -C "$dir" rev-parse HEAD)
	date=$(date -u +%Y-%m-%d)

	rm -rf "$vendor_dir/src" "$vendor_dir/include"
	mkdir -p "$vendor_dir/src/encoding" "$vendor_dir/include"
	cp "$dir/LICENSE" "$vendor_dir/LICENSE"
	cp "$dir/include/vterm.h" "$dir/include/vterm_keycodes.h" "$vendor_dir/include/"
	cp "$dir"/src/*.c "$dir"/src/*.h "$vendor_dir/src/"
	cp "$dir"/src/encoding/*.tbl "$dir"/src/encoding/*.inc "$vendor_dir/src/encoding/"
	cp "$dir/src/fullwidth.inc" "$vendor_dir/src/"

	sed -i \
		-e "s/^- Vendored commit: .*/- Vendored commit: $commit/" \
		-e "s/^- Vendored on: .*/- Vendored on: $date/" \
		"$provenance"

	echo "Base sync done: $BASE_REPO @ $commit"
	echo
	echo "This overwrote every vendored .c/.h/.tbl/.inc file with the"
	echo "unpatched upstream version. Every entry in PROVENANCE.md's"
	echo "\"Patches carried from Neovim's own copy\" section needs to be"
	echo "re-applied by hand, and re-checked against the new base before"
	echo "you trust it still applies cleanly."
}

cmd_patches() {
	local since="${1:-}"
	local dir="$cache_dir/neovim"

	if [ -z "$since" ]; then
		since=$(grep -oE '[0-9a-f]{40}' "$provenance" | tail -1)
		if [ -z "$since" ]; then
			echo "error: could not find a 'last commit considered' hash in" \
				"$provenance; pass one explicitly" >&2
			exit 1
		fi
	fi

	if [ -d "$dir/.git" ]; then
		git -C "$dir" fetch origin
	else
		mkdir -p "$(dirname "$dir")"
		git clone --filter=blob:none --no-checkout --sparse "$PATCH_REPO" "$dir"
		git -C "$dir" sparse-checkout set "$PATCH_SUBDIR"
	fi
	git -C "$dir" checkout -q origin/HEAD

	echo "neovim/neovim commits touching $PATCH_SUBDIR after $since:"
	echo
	git -C "$dir" log --reverse --format='%h %ad %s' --date=short \
		"$since..HEAD" -- "$PATCH_SUBDIR"
}

case "${1:-}" in
base)
	shift
	cmd_base "$@"
	;;
patches)
	shift
	cmd_patches "$@"
	;;
help|-h|--help|"")
	usage
	;;
*)
	echo "error: unknown command '$1'" >&2
	usage >&2
	exit 1
	;;
esac
