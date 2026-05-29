#!/bin/sh
cmake --fresh .. \
    -DCMAKE_DISABLE_FIND_PACKAGE_libfyaml=ON \
    -DPKG_CONFIG_EXECUTABLE=/tmp/no-pkg-config \
    -DFETCHCONTENT_SOURCE_DIR_LIBFYAML=$1
