# SPDX-License-Identifier: MIT
#
# Targets that run the build scripts: a static executable and a Fil-C
# executable, each as a release build and as a build that keeps debug
# information (RelWithDebInfo), and a static executable with every library
# built with the address and undefined-behavior sanitizers (RelWithDebInfo).
# None is part of "all". Each build has its own
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

# fyai_script_check(NAME [EXCLUDE regex])
# Add the target check-NAME. It builds NAME, then runs the functional cases of
# the build directory against out/NAME/fyai. The unit tests link the sources of
# fyai and are not run. FYAI_CHECK_TIMEOUT_SCALE lengthens every wait of a case;
# configure with -DFYAI_TIMEOUT_SCALE for the budget of CTest.
set(FYAI_CHECK_TIMEOUT_SCALE 3 CACHE STRING
    "Multiplier for the waits of the cases that check-NAME targets run")

function(fyai_script_check name)
    cmake_parse_arguments(_sc "" "EXCLUDE" "" ${ARGN})
    set(_select -R "^fyai/case_" -LE nightly)
    if(_sc_EXCLUDE)
        list(APPEND _select -E "${_sc_EXCLUDE}")
    endif()

    add_custom_target(check-${name}
        COMMAND ${CMAKE_COMMAND} -E env
            "FYAI_TEST_BIN=${CMAKE_BINARY_DIR}/out/${name}/fyai"
            "FYAI_TIMEOUT_SCALE=${FYAI_CHECK_TIMEOUT_SCALE}"
            ${CMAKE_CTEST_COMMAND} ${_select}
            -j ${NUM_PROCS} --output-on-failure
        DEPENDS ${name}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running the cases against out/${name}/fyai"
        USES_TERMINAL
        VERBATIM)
endfunction()

if(UNIX)
    fyai_script_build(static-native build-static-native.sh Release)
    fyai_script_build(static-native-debug build-static-native.sh
        RelWithDebInfo)
    fyai_script_build(static-asan build-static-asan.sh RelWithDebInfo)
    if(BUILD_TESTING)
        fyai_script_check(static-asan)
    endif()
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
    if(BUILD_TESTING)
        # The cases that do not apply under Fil-C, as the build lists them.
        list(JOIN FYAI_FILC_DISABLED_CASES "|" _filc_off)
        fyai_script_check(filc EXCLUDE "^fyai/case_(${_filc_off})$")
    endif()
endif()
