#!/bin/sh
# Build a portable fyai inside Docker (docker/Dockerfile.static) and copy the
# executable out to the host.
#
# Usage:
#   scripts/build-static-docker.sh [-m mostly|musl|full] [-i IMAGE]
#                                  [--libfyaml-ref REF] [-o OUTPUT]
#   scripts/build-static-docker.sh [OUTPUT]
#
# OUTPUT defaults to ./fyai in the repo root. Env defaults still work:
# MODE, IMAGE, LIBFYAML_REF.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mode=${MODE:-mostly}
image=${IMAGE:-}
libfyaml_ref=${LIBFYAML_REF:-}
out=

usage()
{
	echo "usage: $0 [-m mostly|musl|full] [-i IMAGE] [--libfyaml-ref REF] [-o OUTPUT]" >&2
	echo "       $0 [OUTPUT]" >&2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-m|--mode)
		[ $# -gt 1 ] || { usage; exit 2; }
		mode=$2
		shift 2
		;;
	-i|--image)
		[ $# -gt 1 ] || { usage; exit 2; }
		image=$2
		shift 2
		;;
	--libfyaml-ref)
		[ $# -gt 1 ] || { usage; exit 2; }
		libfyaml_ref=$2
		shift 2
		;;
	-o|--output)
		[ $# -gt 1 ] || { usage; exit 2; }
		out=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	-*)
		usage
		exit 2
		;;
	*)
		[ -z "$out" ] || { usage; exit 2; }
		out=$1
		shift
		;;
	esac
done

image=${image:-fyai-$mode-static}
out=${out:-"$here/fyai"}

case "$mode" in
mostly|musl|full) ;;
*) echo "MODE must be mostly, musl, or full" >&2; exit 2 ;;
esac

build_args="--build-arg STATIC_MODE=$mode"
if [ -n "$libfyaml_ref" ]; then
	build_args="$build_args --build-arg LIBFYAML_REF=$libfyaml_ref"
fi

# shellcheck disable=SC2086
docker build $build_args -f "$here/docker/Dockerfile.static" -t "$image" "$here"

# Copy the binary out of a throwaway container created from the image.
cid=$(docker create "$image")
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
docker cp "$cid:/usr/local/bin/fyai" "$out"
chmod 0755 "$out"

echo "wrote $out"
file "$out" 2>/dev/null || true
