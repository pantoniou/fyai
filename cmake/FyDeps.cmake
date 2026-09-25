# One-liner resolver for the pantoniou/libfy* dependency family.
#
# Every libfy* dep resolves the same way: reuse an already-defined target,
# then an installed package, then fetch the pinned git tag as a last resort.
# Call site is a single line per dependency - name and git tag:
#
#   fyai_fy_dep(libfypalette 2e697319cb8081769f3472c98a0f5371dc266135)
#   fyai_fy_dep(libfymd4c 8c2d6c96c01c946521db0cdfb6d74a6cc2061e02)
#
# The result is stored in FYAI_LIB<NAME>_TARGET (e.g. FYAI_LIBFYMD4C_TARGET),
# holding the _static target when FYAI_STATIC_DEPS is on and one exists,
# else the shared one. Per-dependency quirks (alternate target names, build
# options to force OFF) live here, not at the call site.
include_guard(GLOBAL)

function(fyai_fy_dep _ffd_name _ffd_tag)
    # Optional overrides for future libs: OUT <var>, TARGETS <tgt>...,
    # STATIC_TARGETS <tgt>..., CMAKE_DEFS VAR=VAL..., REPO <url>.
    cmake_parse_arguments(_FFD "" "OUT;REPO"
        "TARGETS;STATIC_TARGETS;CMAKE_DEFS" ${ARGN})

    if(NOT _FFD_REPO)
        set(_FFD_REPO "https://github.com/pantoniou/${_ffd_name}")
    endif()

    # Per-dependency quirks, so the call site stays name + tag only.
    if("${_ffd_name}" STREQUAL "libfytimui")
        if(NOT _FFD_TARGETS)
            set(_FFD_TARGETS libfytimui::libfytimui libfytimui::fytimui)
        endif()
        if(NOT _FFD_STATIC_TARGETS)
            set(_FFD_STATIC_TARGETS libfytimui::libfytimui_static libfytimui::fytimui_static)
        endif()
        if(NOT _FFD_CMAKE_DEFS)
            set(_FFD_CMAKE_DEFS "BUILD_FYTIMUI_TESTS=OFF" "BUILD_TIMUI_CORE_TESTS=OFF")
        endif()
    elseif("${_ffd_name}" STREQUAL "libfypalette")
        if(NOT _FFD_CMAKE_DEFS)
            set(_FFD_CMAKE_DEFS "BUILD_FYPALETTE_TESTS=OFF" "BUILD_FYPALETTE_TOOLS=OFF")
        endif()
    elseif("${_ffd_name}" STREQUAL "libfymd4c")
        if(NOT _FFD_CMAKE_DEFS)
            set(_FFD_CMAKE_DEFS "BUILD_FYMD4C_EXECUTABLE=OFF")
        endif()
    elseif("${_ffd_name}" STREQUAL "libfymermaid")
        if(NOT _FFD_CMAKE_DEFS)
            set(_FFD_CMAKE_DEFS "BUILD_FYMERMAID_EXECUTABLE=OFF")
        endif()
    elseif("${_ffd_name}" STREQUAL "libfyvterm")
        if(NOT _FFD_CMAKE_DEFS)
            set(_FFD_CMAKE_DEFS "BUILD_FYVTERM_TESTS=OFF" "BUILD_FYVTERM_TOOLS=OFF")
        endif()
    endif()

    if(NOT _FFD_TARGETS)
        set(_FFD_TARGETS "${_ffd_name}::${_ffd_name}")
    endif()
    if(NOT _FFD_STATIC_TARGETS)
        set(_FFD_STATIC_TARGETS "${_ffd_name}::${_ffd_name}_static")
    endif()
    if(NOT _FFD_OUT)
        string(TOUPPER "${_ffd_name}" _ffd_upper)
        set(_FFD_OUT "FYAI_${_ffd_upper}_TARGET")
    endif()

    macro(_ffd_pick _out)
        set(${_out} "")
        if(FYAI_STATIC_DEPS)
            foreach(_t IN LISTS _FFD_STATIC_TARGETS)
                if(TARGET ${_t})
                    set(${_out} "${_t}")
                    break()
                endif()
            endforeach()
        endif()
        if(NOT ${_out})
            foreach(_t IN LISTS _FFD_TARGETS)
                if(TARGET ${_t})
                    set(${_out} "${_t}")
                    break()
                endif()
            endforeach()
        endif()
    endmacro()

    # Already provided by the parent project (or a previous resolve).
    _ffd_pick(_ffd_picked)
    if(_ffd_picked)
        set(${_FFD_OUT} "${_ffd_picked}" PARENT_SCOPE)
        return()
    endif()

    find_package(${_ffd_name} QUIET)

    _ffd_pick(_ffd_picked)
    if(_ffd_picked)
        set(${_FFD_OUT} "${_ffd_picked}" PARENT_SCOPE)
        return()
    endif()

    foreach(_def IN LISTS _FFD_CMAKE_DEFS)
        string(REGEX MATCH "^([^=]+)=(.*)$" _m "${_def}")
        if(NOT _m)
            message(FATAL_ERROR "fyai_fy_dep(${_ffd_name}): bad CMAKE_DEFS entry '${_def}', want VAR=VAL")
        endif()
        set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" CACHE BOOL "" FORCE)
    endforeach()

    # Never let a fetched dep re-enable the top-level test build.
    set(_ffd_have_testing FALSE)
    if(DEFINED BUILD_TESTING)
        set(_ffd_have_testing TRUE)
        set(_ffd_saved_testing "${BUILD_TESTING}")
    endif()
    set(BUILD_TESTING OFF)

    FetchContent_Declare(${_ffd_name}
        GIT_REPOSITORY "${_FFD_REPO}"
        GIT_TAG "${_ffd_tag}"
    )
    FetchContent_MakeAvailable(${_ffd_name})

    if(_ffd_have_testing)
        set(BUILD_TESTING "${_ffd_saved_testing}")
    else()
        unset(BUILD_TESTING)
    endif()

    _ffd_pick(_ffd_picked)
    if(NOT _ffd_picked)
        message(FATAL_ERROR "fyai_fy_dep(${_ffd_name}): no target after fetch (tried: ${_FFD_STATIC_TARGETS} ${_FFD_TARGETS})")
    endif()
    set(${_FFD_OUT} "${_ffd_picked}" PARENT_SCOPE)
endfunction()
