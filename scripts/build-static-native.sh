#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build a mostly-static fyai on this machine, without Docker, and copy the
# executable out. It is the native counterpart of build-static-docker.sh and
# runs the same steps: a static libfyaml in a private prefix, then fyai with
# FYAI_MOSTLY_STATIC=ON, which fetches and links the other libraries.
#
# Usage:
#   scripts/build-static-native.sh [-b BUILD_DIR] [--libfyaml-ref REF]
#                                  [-j JOBS] [-t TYPE] [-o OUTPUT]
#   scripts/build-static-native.sh [OUTPUT]
#
# OUTPUT defaults to ./fyai in the repo root and BUILD_DIR to
# ./build-static-native in the current directory. TYPE is the CMake build type
# (default Release; RelWithDebInfo keeps the debug information).
# Env defaults: LIBFYAML_REF, BUILD_JOBS, BUILD_TYPE.
#
# The libfy* packages that are installed on the machine are hidden from CMake,
# so the result does not depend on them. Nothing is installed outside
# BUILD_DIR. The only dependencies left are the system libraries: libSystem
# and the frameworks on macOS, libc, libm and the loader on Linux.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=
libfyaml_ref=${LIBFYAML_REF:-41dfb98d56596cd295af902f280c24add27bd57d}
libfyaml_repo=${LIBFYAML_REPO:-https://github.com/pantoniou/libfyaml}
jobs=${BUILD_JOBS:-2}
type=${BUILD_TYPE:-Release}
out=

usage()
{
	echo "usage: $0 [-b BUILD_DIR] [--libfyaml-ref REF] [-j JOBS] [-t TYPE] [-o OUTPUT]" >&2
	echo "       $0 [OUTPUT]" >&2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b|--build-dir)
		[ $# -gt 1 ] || { usage; exit 2; }
		build=$2
		shift 2
		;;
	--libfyaml-ref)
		[ $# -gt 1 ] || { usage; exit 2; }
		libfyaml_ref=$2
		shift 2
		;;
	-j|--jobs)
		[ $# -gt 1 ] || { usage; exit 2; }
		jobs=$2
		shift 2
		;;
	-t|--build-type)
		[ $# -gt 1 ] || { usage; exit 2; }
		type=$2
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

build=${build:-"$PWD/build-static-native"}
out=${out:-"$here/fyai"}
mkdir -p "$build"
build=$(CDPATH= cd -- "$build" && pwd)
prefix=$build/prefix

# A static libfyaml, pinned as in docker/Dockerfile.static.
if [ ! -f "$prefix/lib/libfyaml.a" ]; then
	rm -rf "$build/libfyaml"
	git clone "$libfyaml_repo" "$build/libfyaml"
	git -C "$build/libfyaml" checkout "$libfyaml_ref"
	cmake -S "$build/libfyaml" -B "$build/libfyaml/build" \
		-DCMAKE_BUILD_TYPE="$type" \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_TESTING=OFF \
		-DCMAKE_INSTALL_PREFIX="$prefix"
	cmake --build "$build/libfyaml/build" -j"$jobs"
	cmake --install "$build/libfyaml/build"
fi

# Hide the libfy* packages that are installed on the machine: their configs
# name shared targets that clash with the static libfyaml.
ignore=
for d in /usr/local/lib/cmake/libfy* /opt/homebrew/lib/cmake/libfy* \
	 /usr/lib/cmake/libfy* /usr/lib/*/cmake/libfy*; do
	[ -d "$d" ] || continue
	ignore="${ignore:+$ignore;}$d"
done

PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

cmake -S "$here" -B "$build/fyai" \
	-DCMAKE_BUILD_TYPE="$type" \
	-DFYAI_MOSTLY_STATIC=ON \
	-DBUILD_TESTING=OFF \
	-DCMAKE_PREFIX_PATH="$prefix" \
	${ignore:+"-DCMAKE_IGNORE_PATH=$ignore"}
cmake --build "$build/fyai" -j"$jobs"

mkdir -p "$(dirname -- "$out")"
cp "$build/fyai/fyai" "$out"

# The result may use only system libraries.
case "$(uname -s)" in
Darwin)
	other=$(otool -L "$out" | tail -n +2 | awk '{print $1}' |
		grep -vE '^(/usr/lib/|/System/Library/)' || true)
	;;
*)
	other=$(ldd "$out" 2>/dev/null | awk '{print $1}' |
		grep -vE '^(linux-vdso|libc\.|libm\.|ld-linux|/lib.*ld-linux)' ||
		true)
	;;
esac
if [ -n "$other" ]; then
	echo "$0: $out needs libraries that do not travel:" >&2
	echo "$other" | sed 's/^/  /' >&2
	exit 1
fi
echo "$0: $out"
