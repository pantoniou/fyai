#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build fyai with Fil-C: zlib and OpenSSL from source into a private prefix,
# then fyai with static libraries. See doc/filc.md.
#
# Usage: scripts/build-filc.sh [-b BUILD_DIR] [-p PREFIX] [-j JOBS] [-o OUTPUT]
#
# FILC is the Fil-C distribution (default $HOME/filc). The tarballs of zlib and
# OpenSSL are read from DOWNLOADS (default PREFIX/src) and fetched when absent.
# LIBFYAML_SRC and LIBFYTS_SRC name local source trees to use instead of the
# pinned ones. OUTPUT is the directory that receives the fyai executable.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
filc=${FILC:-$HOME/filc}
build=$PWD/build-filc
prefix=
jobs=${BUILD_JOBS:-4}
out=

zlib_ver=1.3.2
zlib_sha=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
ssl_ver=3.6.5
ssl_sha=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98

while [ $# -gt 0 ]; do
	case "$1" in
	-b) build=$2; shift 2 ;;
	-p) prefix=$2; shift 2 ;;
	-j) jobs=$2; shift 2 ;;
	-o) out=$2; shift 2 ;;
	*) echo "usage: $0 [-b BUILD_DIR] [-p PREFIX] [-j JOBS] [-o OUTPUT]" >&2; exit 2 ;;
	esac
done
[ -n "$prefix" ] || prefix=$build/prefix
downloads=${DOWNLOADS:-$prefix/src}
cc=$filc/build/bin/clang

fetch()
{
	# fetch FILE URL SHA256
	[ -f "$downloads/$1" ] || curl -fsSL -o "$downloads/$1" "$2"
	echo "$3  $downloads/$1" | sha256sum -c -
}

mkdir -p "$downloads" "$prefix"
prefix=$(cd "$prefix" && pwd)

if [ ! -f "$prefix/lib/libz.a" ]; then
	fetch zlib.tar.gz "https://github.com/madler/zlib/releases/download/v$zlib_ver/zlib-$zlib_ver.tar.gz" "$zlib_sha"
	rm -rf "$prefix/src/zlib"
	mkdir -p "$prefix/src/zlib"
	tar xzf "$downloads/zlib.tar.gz" -C "$prefix/src/zlib" --strip-components=1
	(cd "$prefix/src/zlib" &&
	 CC=$cc CFLAGS="-O2" ./configure --prefix="$prefix" --static &&
	 make -j"$jobs" && make install)
fi

# The assembly of OpenSSL does not build with Fil-C: use the C code.
if [ ! -f "$prefix/lib/libcrypto.a" ]; then
	fetch openssl.tar.gz "https://github.com/openssl/openssl/releases/download/openssl-$ssl_ver/openssl-$ssl_ver.tar.gz" "$ssl_sha"
	rm -rf "$prefix/src/openssl"
	mkdir -p "$prefix/src/openssl"
	tar xzf "$downloads/openssl.tar.gz" -C "$prefix/src/openssl" --strip-components=1
	(cd "$prefix/src/openssl" &&
	 CC="$cc -O2" ./Configure no-asm no-shared no-tests no-docs zlib \
		--prefix="$prefix" --libdir=lib \
		--with-zlib-include="$prefix/include" --with-zlib-lib="$prefix/lib" &&
	 make -j"$jobs" build_libs && make install_dev)
fi

fetch_args=
[ -z "${LIBFYAML_SRC:-}" ] || fetch_args="$fetch_args -DFETCHCONTENT_SOURCE_DIR_LIBFYAML=$LIBFYAML_SRC"
[ -z "${LIBFYTS_SRC:-}" ] || fetch_args="$fetch_args -DFETCHCONTENT_SOURCE_DIR_LIBFYTS=$LIBFYTS_SRC"

# Hide the host packages, so that every library is built with Fil-C.
# shellcheck disable=SC2086
PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig \
cmake -S "$here" -B "$build" -G Ninja \
	-DCMAKE_C_COMPILER="$cc" -DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
	"-DCMAKE_IGNORE_PREFIX_PATH=/usr/local;/usr" \
	-DCMAKE_PREFIX_PATH="$prefix" -DOPENSSL_ROOT_DIR="$prefix" \
	-DOPENSSL_USE_STATIC_LIBS=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
	$fetch_args
cmake --build "$build" --target fyai -j"$jobs"

if [ -n "$out" ]; then
	mkdir -p "$out"
	cp "$build/fyai" "$out/fyai"
fi
