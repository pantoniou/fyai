# SPDX-License-Identifier: MIT
#
# Targets that run the build scripts: a static executable and a Fil-C
# executable, each as a release build and as a build that keeps debug
# information (RelWithDebInfo). None is part of "all". Each build has its own
# directory under script-builds/ and leaves its executable in out/<name>/fyai.

set(FYAI_SCRIPT_JOBS 12 CACHE STRING "Parallel jobs of the script builds")

# fyai_script_build(NAME SCRIPT TYPE [ENV VAR=value...])
function(fyai_script_build name script type)
    cmake_parse_arguments(_sb "" "" "ENV" ${ARGN})
    set(_dir "${CMAKE_BINARY_DIR}/script-builds/${name}")
    set(_out "${CMAKE_BINARY_DIR}/out/${name}")

    add_custom_target(${name}
        COMMAND ${CMAKE_COMMAND} -E env ${_sb_ENV}
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/${script}"
            -b "${_dir}" -j ${FYAI_SCRIPT_JOBS} -t ${type} -o "${_out}/fyai"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        USES_TERMINAL
        VERBATIM)
endfunction()

if(UNIX)
    fyai_script_build(static-native build-static-native.sh Release)
    fyai_script_build(static-native-debug build-static-native.sh
        RelWithDebInfo)
endif()

# The Fil-C distribution: $FILC, else ~/filc, as build-filc.sh reads it.
if(DEFINED ENV{FILC})
    set(FYAI_FILC_DIR "$ENV{FILC}" CACHE PATH "Fil-C distribution")
elseif(DEFINED ENV{HOME})
    set(FYAI_FILC_DIR "$ENV{HOME}/filc" CACHE PATH "Fil-C distribution")
endif()

if(UNIX AND FYAI_FILC_DIR
   AND EXISTS "${FYAI_FILC_DIR}/build/bin/clang")
    fyai_script_build(filc build-filc.sh Release ENV FILC=${FYAI_FILC_DIR})
    fyai_script_build(filc-debug build-filc.sh RelWithDebInfo
        ENV FILC=${FYAI_FILC_DIR})
endif()
