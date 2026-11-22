#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build a mostly-static fyai in which every library is built with the address
# and undefined-behavior sanitizers, and copy the executable out. It extends
# build-static-native.sh: zlib, OpenSSL and libfyaml are built from source with
# the sanitizers into a private prefix, then fyai is built with
# FYAI_MOSTLY_STATIC=ON and ENABLE_ASAN=ON. The other libfy* libraries and
# libcurl, which CMake fetches, take the sanitizer flags from CMAKE_C_FLAGS.
# The sanitizer runtime is linked in, so the result needs no libasan.
#
# Usage:
#   scripts/build-static-asan.sh [-b BUILD_DIR] [--libfyaml-ref REF]
#                                [-j JOBS] [-t TYPE] [-o OUTPUT]
#   scripts/build-static-asan.sh [OUTPUT]
#
# OUTPUT defaults to ./fyai-asan in the repo root and BUILD_DIR to
# ./build-static-asan in the current directory. TYPE is the CMake build type
# (default RelWithDebInfo). CC names the compiler (default cc). SAN_FLAGS
# replaces the sanitizer list. Env defaults: LIBFYAML_REF, BUILD_JOBS,
# BUILD_TYPE, DOWNLOADS (the directory of the zlib and OpenSSL tarballs).
#
# The libc stays dynamic: a sanitizer cannot run in a fully static glibc
# executable. The libfy* packages that are installed on the machine are hidden
# from CMake, so the result does not depend on them. Nothing is installed
# outside BUILD_DIR.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=
libfyaml_ref=${LIBFYAML_REF:-d312d213f50fb3fff46bd745488e605138b9d4d1}
libfyaml_repo=${LIBFYAML_REPO:-https://github.com/pantoniou/libfyaml}
jobs=${BUILD_JOBS:-2}
type=${BUILD_TYPE:-RelWithDebInfo}
out=
cc=${CC:-cc}
san=${SAN_FLAGS:--fsanitize=address,undefined -fno-omit-frame-pointer}

zlib_ver=1.3.2
zlib_sha=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
ssl_ver=3.6.5
ssl_sha=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98

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

build=${build:-"$PWD/build-static-asan"}
out=${out:-"$here/fyai-asan"}
mkdir -p "$build"
build=$(CDPATH= cd -- "$build" && pwd)
prefix=$build/prefix
downloads=${DOWNLOADS:-$build/src}
mkdir -p "$prefix" "$downloads"

# The sanitizer runtime goes into the executable. GCC and Clang name the
# option differently; Apple Clang has only a dynamic runtime.
link=
case "$(uname -s)" in
Darwin) ;;
*)
	if "$cc" --version 2>/dev/null | grep -qi clang; then
		link=-static-libsan
	else
		link="-static-libgcc -static-libasan -static-libubsan"
	fi
	;;
esac

fetch()
{
	# fetch FILE URL SHA256
	[ -f "$downloads/$1" ] || curl -fsSL -o "$downloads/$1" "$2"
	if command -v sha256sum >/dev/null 2>&1; then
		echo "$3  $downloads/$1" | sha256sum -c -
	else
		echo "$3  $downloads/$1" | shasum -a 256 -c -
	fi
}

if [ ! -f "$prefix/lib/libz.a" ]; then
	fetch zlib.tar.gz "https://github.com/madler/zlib/releases/download/v$zlib_ver/zlib-$zlib_ver.tar.gz" "$zlib_sha"
	rm -rf "$build/zlib"
	mkdir -p "$build/zlib"
	tar xzf "$downloads/zlib.tar.gz" -C "$build/zlib" --strip-components=1
	(cd "$build/zlib" &&
	 CC=$cc CFLAGS="-O2 -g $san" ./configure --prefix="$prefix" --static &&
	 make -j"$jobs" && make install)
fi

# Plain C code, so that the sanitizer sees every access.
if [ ! -f "$prefix/lib/libcrypto.a" ]; then
	fetch openssl.tar.gz "https://github.com/openssl/openssl/releases/download/openssl-$ssl_ver/openssl-$ssl_ver.tar.gz" "$ssl_sha"
	rm -rf "$build/openssl"
	mkdir -p "$build/openssl"
	tar xzf "$downloads/openssl.tar.gz" -C "$build/openssl" --strip-components=1
	# shellcheck disable=SC2086
	(cd "$build/openssl" &&
	 CC=$cc ./Configure no-asm no-shared no-tests no-docs zlib -g \
		$san \
		--prefix="$prefix" --libdir=lib \
		--with-zlib-include="$prefix/include" --with-zlib-lib="$prefix/lib" &&
	 make -j"$jobs" build_libs && make install_dev)
fi

# A static libfyaml, pinned as in docker/Dockerfile.static.
if [ ! -f "$prefix/lib/libfyaml.a" ]; then
	rm -rf "$build/libfyaml"
	git clone "$libfyaml_repo" "$build/libfyaml"
	git -C "$build/libfyaml" checkout "$libfyaml_ref"
	cmake -S "$build/libfyaml" -B "$build/libfyaml/build" \
		-DCMAKE_C_COMPILER="$cc" \
		-DCMAKE_C_FLAGS="$san" \
		-DCMAKE_BUILD_TYPE="$type" \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_TESTING=OFF \
		-DENABLE_PYTHON_BINDINGS=OFF \
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
	-DCMAKE_C_COMPILER="$cc" \
	-DCMAKE_C_FLAGS="$san" \
	-DCMAKE_EXE_LINKER_FLAGS="$san $link" \
	-DCMAKE_BUILD_TYPE="$type" \
	-DENABLE_ASAN=ON \
	-DFYAI_MOSTLY_STATIC=ON \
	-DBUILD_TESTING=OFF \
	-DCMAKE_PREFIX_PATH="$prefix" \
	-DOPENSSL_ROOT_DIR="$prefix" \
	-DOPENSSL_USE_STATIC_LIBS=ON \
	${ignore:+"-DCMAKE_IGNORE_PATH=$ignore"}
cmake --build "$build/fyai" -j"$jobs"

mkdir -p "$(dirname -- "$out")"
cp "$build/fyai/fyai" "$out"

# The result may use only system libraries, and no sanitizer runtime.
case "$(uname -s)" in
Darwin)
	other=$(otool -L "$out" | tail -n +2 | awk '{print $1}' |
		grep -vE '^(/usr/lib/|/System/Library/|@rpath/libclang_rt)' ||
		true)
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
