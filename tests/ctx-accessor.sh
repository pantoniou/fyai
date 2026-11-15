#!/bin/bash
# SPDX-License-Identifier: MIT
# An object that stands on a context gives it through an accessor
# (NAME_ctx()). A direct read of its ctx field outside that accessor fails
# here: the field is set when the object is made and read nowhere else.
set -eu

src_dir="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

# An assignment sets the field; the accessor and the container_of() lookup of
# an app component are the reads that stay.
bad=$(find "$src_dir/src" \( -name '*.c' -o -name '*.h' \) -print0 |
	xargs -0 grep -nE -- '->ctx\b' |
	grep -vE -- '->ctx[[:space:]]*=[^=]|return [a-z_]+->ctx;|container_of|&app->ctx' || true)

if [ -n "$bad" ]; then
	echo "ctx-accessor: read the context through its accessor:" >&2
	echo "$bad" >&2
	exit 1
fi
