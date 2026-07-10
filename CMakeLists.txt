# vim: set ts=4:sw=4:expandtab:
cmake_minimum_required(VERSION 3.14)

if (POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW) # Allow project(xxx VERSION a.b.c)
endif()

if (POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW) # find_package uses <PackageName>_ROOT variables
endif()

if (POLICY CMP0110)
    cmake_policy(SET CMP0110 NEW) # add_test() supports arbitrary characters in test names
endif()

# Get version from git-version-gen
execute_process(
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/git-version-gen" "${CMAKE_CURRENT_SOURCE_DIR}/.tarball-version"
    OUTPUT_VARIABLE PROJECT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Parse version components
string(REGEX MATCH "^([0-9]+)(\\.([0-9]+)(\\.([0-9]+))?)?(-(.*))?$" _ "${PROJECT_VERSION}")
set(VERSION_MAJOR "${CMAKE_MATCH_1}")
if(CMAKE_MATCH_3)
    set(VERSION_MINOR "${CMAKE_MATCH_3}")
else()
    set(VERSION_MINOR "0")
endif()
if(CMAKE_MATCH_5)
    set(VERSION_PATCH "${CMAKE_MATCH_5}")
else()
    set(VERSION_PATCH "0")
endif()
if(CMAKE_MATCH_7)
    set(VERSION_EXTRA "${CMAKE_MATCH_7}")
else()
    set(VERSION_EXTRA "")
endif()

project(fyai
    LANGUAGES C
    VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}${VERSION_EXTRA})

# Export compile commands by default for clangd LSP support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckCSourceCompiles)
include(CheckCCompilerFlag)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(CMakeDependentOption)
include(CTest)
include(FetchContent)

check_c_compiler_flag("-std=gnu2x" FYAI_HAVE_GNU2X)
if(FYAI_HAVE_GNU2X)
    set(FYAI_C_DIALECT "-std=gnu2x")
else()
    set(FYAI_C_DIALECT "-std=gnu11")
endif()

set(FYAI_C_WARNINGS -Wall -Wextra)
check_c_compiler_flag("-Wdeclaration-after-statement" FYAI_HAVE_WDECL_AFTER_STMT)
if(FYAI_HAVE_WDECL_AFTER_STMT)
    list(APPEND FYAI_C_WARNINGS -Wdeclaration-after-statement)
endif()

set(WITH_LIBFYAML "" CACHE PATH "Path to a libfyaml checkout build directory")
option(FYAI_WITH_LIBFYAML_TAGS "Include fetched libfyaml sources in fyai tags" ON)

# Static modes:
# - FYAI_MOSTLY_STATIC: static fyai deps, dynamic libc. This is the portable
#   glibc build; fully static glibc breaks getaddrinfo/NSS on other hosts.
# - FYAI_STATIC: legacy fully-static link with the host libc.
# - FYAI_MUSL_STATIC: fully-static link intended for a musl toolchain.
option(FYAI_MOSTLY_STATIC "Link fyai deps statically but leave libc dynamic" OFF)
option(FYAI_STATIC "Build a fully static fyai with the host C library" OFF)
option(FYAI_MUSL_STATIC "Build a fully static fyai with a musl toolchain" OFF)

if(FYAI_MUSL_STATIC)
    set(FYAI_STATIC ON CACHE BOOL "" FORCE)
endif()
if(FYAI_STATIC AND FYAI_MOSTLY_STATIC)
    message(FATAL_ERROR "Pick only one of FYAI_STATIC or FYAI_MOSTLY_STATIC")
endif()
set(FYAI_STATIC_DEPS OFF)
if(FYAI_STATIC OR FYAI_MOSTLY_STATIC)
    set(FYAI_STATIC_DEPS ON)
endif()

if(FYAI_STATIC_DEPS)
    find_package(PkgConfig REQUIRED)
endif()

if(FYAI_STATIC_DEPS AND NOT (APPLE AND FYAI_MOSTLY_STATIC))
    set(_FYAI_FIND_LIBRARY_SUFFIXES "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(FYAI_ZLIB_STATIC_LIB
        NAMES z
    )
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_FYAI_FIND_LIBRARY_SUFFIXES}")
    if(NOT FYAI_ZLIB_STATIC_LIB OR NOT FYAI_ZLIB_STATIC_LIB MATCHES "\\.a$")
        message(FATAL_ERROR
            "static builds require a static zlib archive (libz.a); "
            "found: ${FYAI_ZLIB_STATIC_LIB}")
    endif()
    set(ZLIB_USE_STATIC_LIBS ON)
    set(ZLIB_LIBRARY "${FYAI_ZLIB_STATIC_LIB}" CACHE FILEPATH "" FORCE)
    set(ZLIB_LIBRARY_RELEASE "${FYAI_ZLIB_STATIC_LIB}" CACHE FILEPATH "" FORCE)
endif()

if(ENABLE_ASAN)
  set(HAVE_ASAN 1)
  set(ASAN_C_FLAGS "-fsanitize=address,signed-integer-overflow,undefined" "-fno-omit-frame-pointer")
  set(ASAN_LINK_OPTS ${ASAN_C_FLAGS})
else()
  set(HAVE_ASAN 0)
  set(ASAN_C_FLAGS "")
  set(ASAN_LINK_OPTS ${ASAN_C_FLAGS})
endif()

# Find libfyaml
if(FYAI_STATIC_DEPS)
    # A static link must pull the libfyaml archive (.a), not the shared
    # imported target that find_package's CONFIG package exposes (ld refuses
    # to statically link a .so). Resolve the archive and headers via
    # pkg-config, then build an INTERFACE target around the .a explicitly.
    pkg_check_modules(LIBFYAMLPC REQUIRED libfyaml)
    find_library(LIBFYAML_STATIC_LIB
        NAMES libfyaml.a fyaml
        HINTS ${LIBFYAMLPC_LIBDIR} ${LIBFYAMLPC_LIBRARY_DIRS}
    )
    if(NOT LIBFYAML_STATIC_LIB OR NOT LIBFYAML_STATIC_LIB MATCHES "\\.a$")
        message(FATAL_ERROR
            "static builds require a static libfyaml archive (libfyaml.a); "
            "found: ${LIBFYAML_STATIC_LIB}")
    endif()
    add_library(libfyaml::libfyaml INTERFACE IMPORTED)
    # The archive pulls in libm (trunc et al.); pkg-config does not record it.
    set_target_properties(libfyaml::libfyaml PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBFYAMLPC_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${LIBFYAML_STATIC_LIB};m"
    )
elseif(WITH_LIBFYAML)
    find_package(libfyaml 1.0 REQUIRED CONFIG
        PATHS "${WITH_LIBFYAML}"
        NO_DEFAULT_PATH
    )
else()
    find_package(libfyaml 1.0 QUIET)
endif()

if(NOT FYAI_STATIC_DEPS AND NOT libfyaml_FOUND)
    # Fall back to pkg-config
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBFYAML QUIET libfyaml)
    endif()

    if(LIBFYAML_FOUND)
        # Create imported target for consistency
        add_library(libfyaml::libfyaml INTERFACE IMPORTED)
        set_target_properties(libfyaml::libfyaml PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LIBFYAML_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${LIBFYAML_LIBRARIES}"
            INTERFACE_LINK_DIRECTORIES "${LIBFYAML_LIBRARY_DIRS}"
            INTERFACE_COMPILE_OPTIONS "${LIBFYAML_CFLAGS_OTHER}"
        )
    endif()
endif()

if(NOT TARGET libfyaml::libfyaml)
    if(FYAI_WITH_LIBFYAML_TAGS)
        set(LIBFYAML_ENABLE_TAGS ON CACHE BOOL "Add libfyaml tags helper targets" FORCE)
    endif()
    FetchContent_Declare(libfyaml
        GIT_REPOSITORY https://github.com/pantoniou/libfyaml
        GIT_TAG 8bcb95f2e1b8f1d0546ef8e0dd304a50ee3d1bcb
    )
    FetchContent_MakeAvailable(libfyaml)
endif()

# A fully static fyai links only the *_static targets of the fetched deps, but
# libfymd4c/libfyts still build their shared .so by default - and that .so can
# not link the non-PIC static libfyaml.a (ld: "recompile with -fPIC"). Build the
# whole fetched tree static so only the archives we actually use are produced.
if(FYAI_STATIC_DEPS)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()

if(NOT TARGET libfymd4c::libfymd4c)
    find_package(libfymd4c QUIET)
endif()

if(NOT TARGET libfymd4c::libfymd4c AND
   NOT TARGET libfymd4c::libfymd4c_static)
    FetchContent_Declare(libfymd4c
        GIT_REPOSITORY https://github.com/pantoniou/libfymd4c
        GIT_TAG a12c0dd5a2345a039346f433b55f03cde9b6799b
    )
    FetchContent_MakeAvailable(libfymd4c)
endif()

if(FYAI_STATIC_DEPS AND TARGET libfyaml::fyaml_static)
    set_target_properties(libfyaml::fyaml_static PROPERTIES
        INTERFACE_LINK_LIBRARIES "Threads::Threads;m")
endif()

if(FYAI_STATIC_DEPS AND TARGET libfymd4c::libfymd4c_static)
    set(FYAI_LIBFYMD4C_TARGET libfymd4c::libfymd4c_static)
else()
    set(FYAI_LIBFYMD4C_TARGET libfymd4c::libfymd4c)
endif()

# Fetch and build a minimal libcurl from source as a static archive linked
# straight into fyai: OpenSSL TLS + zlib only, with the curl CLI and the heavy
# optional deps (HTTP/2, libssh, ldap, rtmp, psl, brotli, idn2, gssapi) disabled
# so the link needs only the OpenSSL/zlib archives, not curl's whole transitive
# chain. Exposes the CURL::libcurl target the fyai link expects, same as
# find_package(CURL). OpenSSL linkage (static vs the system .so) is left to the
# caller via OPENSSL_USE_STATIC_LIBS.
function(fyai_fetch_curl)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    set(CURL_ZLIB ON CACHE STRING "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_GSSAPI OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(USE_LIBRTMP OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(curl
        GIT_REPOSITORY https://github.com/curl/curl
        GIT_TAG curl-8_11_1
    )
    FetchContent_MakeAvailable(curl)
endfunction()

# Find libcurl. A static fyai always builds curl from source (with static
# OpenSSL). A shared fyai prefers the system libcurl, but falls back to the same
# from-source build when none is installed - so a virgin toolchain still builds
# (needing only OpenSSL/zlib, not a libcurl-dev package).
if(FYAI_STATIC_DEPS)
    set(OPENSSL_USE_STATIC_LIBS ON)
    fyai_fetch_curl()
else()
    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        message(STATUS
            "libcurl not found; building a minimal static libcurl from source")
        fyai_fetch_curl()
    endif()
endif()

# linenoise: tiny line-editing library, vendored under third_party/linenoise
# (single drop-in source file, the way upstream intends it to be embedded). It
# carries a local Ctrl-G external-editor hook, so it lives in-tree rather than
# fetched. Built as an isolated static lib with warnings off since it predates
# our -Wall -Wextra and GNU C dialect warnings.
add_library(linenoise STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/linenoise/linenoise.c")
target_include_directories(linenoise PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/linenoise")
target_compile_options(linenoise PRIVATE -w)

option(ENABLE_ASAN "Enable ASAN support" OFF)
option(BUILD_TESTING "Build tests" ON)

set(FYAI_PROGRAMS fyai)

# Markdown styling and theming are fully delegated to libfymd4c's embedded theme
# catalogue (selected by the display/markdown_theme config key); fyai ships no
# styling YAML of its own.

# Embed the vendored provider/model catalogue snapshot (a scrape-providers
# document) as the out-of-the-box fallback; `fyai catalog import` supersedes
# it with an arena-resident catalogue.
set(FYAI_CATALOG_YAML "${CMAKE_CURRENT_SOURCE_DIR}/data/catalog.yaml")
file(READ "${FYAI_CATALOG_YAML}" _fyai_catalog_hex HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," _fyai_catalog_bytes
    "${_fyai_catalog_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/embedded_catalog.inc"
    "/* Generated from ${FYAI_CATALOG_YAML}. */\n"
    "static const unsigned char FYAI_EMBEDDED_CATALOG[] = {\n"
    "${_fyai_catalog_bytes}"
    "\n};\n"
    "static const size_t FYAI_EMBEDDED_CATALOG_LEN = sizeof(FYAI_EMBEDDED_CATALOG);\n"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FYAI_CATALOG_YAML}")

# Embed the sample configuration so `fyai init` with no config file has a
# sensible starting document instead of committing an empty config.
set(FYAI_CONFIG_SAMPLE "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml.sample")
file(READ "${FYAI_CONFIG_SAMPLE}" _fyai_config_hex HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," _fyai_config_bytes
    "${_fyai_config_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/embedded_config.inc"
    "/* Generated from ${FYAI_CONFIG_SAMPLE}. */\n"
    "static const unsigned char FYAI_EMBEDDED_CONFIG[] = {\n"
    "${_fyai_config_bytes}"
    "\n};\n"
    "static const size_t FYAI_EMBEDDED_CONFIG_LEN = sizeof(FYAI_EMBEDDED_CONFIG);\n"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FYAI_CONFIG_SAMPLE}")

# Embed the config document JSON Schema (`fyai config validate`).
set(FYAI_CONFIG_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/data/config.schema.yaml")
file(READ "${FYAI_CONFIG_SCHEMA}" _fyai_config_schema_hex HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1,"
    _fyai_config_schema_bytes "${_fyai_config_schema_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/embedded_config_schema.inc"
    "/* Generated from ${FYAI_CONFIG_SCHEMA}. */\n"
    "static const unsigned char FYAI_EMBEDDED_CONFIG_SCHEMA[] = {\n"
    "${_fyai_config_schema_bytes}"
    "\n};\n"
    "static const size_t FYAI_EMBEDDED_CONFIG_SCHEMA_LEN = sizeof(FYAI_EMBEDDED_CONFIG_SCHEMA);\n"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FYAI_CONFIG_SCHEMA}")

# Generate config.h
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/config.h"
)

# fyai
add_executable(fyai
    src/main.c
    src/fyai_display.c
    src/fyai.c
    src/fyai_log.c
    src/fyai_markdown.c
    src/fyai_patch.c
    src/fyai_prof.c
    src/fyai_provider.c
    src/fyai_sandbox.c
    src/fyai_session.c
    src/fyai_signal.c
    src/fyai_storage.c
    src/fyai_stream.c
    src/fyai_terminal.c
    src/fyai_schema.c
    src/fyai_tools.c
    src/fyai_turn.c
    src/utils.c
    src/fyai_catalog.c
    src/fyai_config.c
    src/commands.c
)
# The C23 __isoc23_* redirect wrappers exist only so portable mostly-static
# glibc binaries do not require GLIBC_2.38; fully dynamic builds use the host
# glibc directly (and the interposers break ASAN's own interceptors).
if(FYAI_MOSTLY_STATIC)
    target_sources(fyai PRIVATE src/glibc_compat.c)
endif()
target_compile_options(fyai PRIVATE ${FYAI_C_DIALECT} ${FYAI_C_WARNINGS}
    ${ASAN_C_FLAGS})
target_link_libraries(fyai PRIVATE libfyaml::libfyaml ${FYAI_LIBFYMD4C_TARGET}
    CURL::libcurl linenoise ${ASAN_LINK_OPTS})
target_compile_definitions(fyai PRIVATE
    _GNU_SOURCE
    HAVE_CONFIG_H
    VERSION="${PROJECT_VERSION}"
    $<$<NOT:$<CONFIG:DEBUG>>:NDEBUG>
)
target_include_directories(fyai PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR})

if(BUILD_TESTING)
    add_executable(fyai_patch_test
        tests/fyai_patch_test.c
        src/fyai_patch.c
        src/fyai_sandbox.c
        src/utils.c
    )
    target_compile_options(fyai_patch_test PRIVATE ${FYAI_C_DIALECT}
        ${FYAI_C_WARNINGS} ${ASAN_C_FLAGS})
    target_compile_definitions(fyai_patch_test PRIVATE
        _GNU_SOURCE
        HAVE_CONFIG_H
        VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(fyai_patch_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(fyai_patch_test PRIVATE libfyaml::libfyaml CURL::libcurl
        ${ASAN_LINK_OPTS})
    add_test(NAME fyai/patch_apply COMMAND fyai_patch_test)

    add_executable(fyai_provider_test
        tests/fyai_provider_test.c
        src/fyai_provider.c
        src/fyai_sandbox.c
        src/utils.c
    )
    target_compile_options(fyai_provider_test PRIVATE ${FYAI_C_DIALECT}
        ${FYAI_C_WARNINGS} ${ASAN_C_FLAGS})
    target_compile_definitions(fyai_provider_test PRIVATE
        _GNU_SOURCE
        HAVE_CONFIG_H
        VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(fyai_provider_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(fyai_provider_test PRIVATE libfyaml::libfyaml CURL::libcurl
        ${ASAN_LINK_OPTS})
    add_test(NAME fyai/provider_translate COMMAND fyai_provider_test)

    add_executable(fyai_stackcheck_test
        tests/fyai_stackcheck_test.c
        src/fyai_sandbox.c
        src/utils.c
    )
    target_compile_options(fyai_stackcheck_test PRIVATE ${FYAI_C_DIALECT}
        ${FYAI_C_WARNINGS} ${ASAN_C_FLAGS})
    target_compile_definitions(fyai_stackcheck_test PRIVATE
        _GNU_SOURCE
        HAVE_CONFIG_H
        VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(fyai_stackcheck_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(fyai_stackcheck_test PRIVATE libfyaml::libfyaml CURL::libcurl
        ${ASAN_LINK_OPTS})
    add_test(NAME fyai/generic_stackcheck COMMAND fyai_stackcheck_test)

    add_executable(fyai_schema_test
        tests/fyai_schema_test.c
        src/fyai_schema.c
        src/fyai_sandbox.c
        src/utils.c
    )
    target_compile_options(fyai_schema_test PRIVATE ${FYAI_C_DIALECT}
        ${FYAI_C_WARNINGS} ${ASAN_C_FLAGS})
    target_compile_definitions(fyai_schema_test PRIVATE
        _GNU_SOURCE
        HAVE_CONFIG_H
        VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(fyai_schema_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(fyai_schema_test PRIVATE libfyaml::libfyaml CURL::libcurl
        ${ASAN_LINK_OPTS})
    add_test(NAME fyai/schema_validate COMMAND fyai_schema_test)

    add_executable(fyai_config_test
        tests/fyai_config_test.c
        tests/fyai_config_test_stubs.c
        src/fyai_catalog.c
        src/fyai_config.c
        src/fyai_prof.c
        src/fyai_sandbox.c
        src/fyai_schema.c
        src/fyai_storage.c
        src/fyai_terminal.c
        src/utils.c
    )
    target_compile_options(fyai_config_test PRIVATE ${FYAI_C_DIALECT}
        ${FYAI_C_WARNINGS} ${ASAN_C_FLAGS})
    target_compile_definitions(fyai_config_test PRIVATE
        _GNU_SOURCE
        HAVE_CONFIG_H
        VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(fyai_config_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(fyai_config_test PRIVATE libfyaml::libfyaml CURL::libcurl
        ${ASAN_LINK_OPTS})
    add_test(NAME fyai/config_root COMMAND fyai_config_test)

    # Functional cases: each script drives the real fyai binary against the
    # scenario-driven mock provider (tests/mock/mock_provider.py) in a
    # hermetic sandbox (private HOME/XDG_*, localhost only).
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    file(GLOB FYAI_TEST_CASES ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/*.sh)
    foreach(case_script ${FYAI_TEST_CASES})
        get_filename_component(case_name ${case_script} NAME_WE)
        add_test(NAME fyai/case_${case_name} COMMAND bash ${case_script})
        set_tests_properties(fyai/case_${case_name} PROPERTIES
            TIMEOUT 60
            ENVIRONMENT "FYAI_BIN=$<TARGET_FILE:fyai>;PYTHON=${Python3_EXECUTABLE};TESTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests"
        )
    endforeach()

    add_test(NAME fyai/cli_dump_conflicting_selectors
        COMMAND fyai dump --first 1 --last 1)
    set_tests_properties(fyai/cli_dump_conflicting_selectors PROPERTIES
        WILL_FAIL TRUE)
    add_test(NAME fyai/cli_history_invalid_range
        COMMAND fyai history --range invalid)
    set_tests_properties(fyai/cli_history_invalid_range PROPERTIES
        WILL_FAIL TRUE)
endif()

if(FYAI_STATIC)
    target_link_options(fyai PRIVATE -static)
endif()

# Installation (optional)
install(TARGETS
    ${FYAI_PROGRAMS}
    RUNTIME DESTINATION bin/
)

# Install sample files
install(FILES
    README.md
    config.yaml.sample
    DESTINATION share/doc/fyai
)

# tags and cscope support
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tags.cmake)

# Show configuration info
message(STATUS "fyai configuration:")
message(STATUS "VERSION:               ${PROJECT_VERSION}")
message(STATUS "MAJOR.MINOR:           ${VERSION_MAJOR}.${VERSION_MINOR}")
message(STATUS "PATCH:                 ${VERSION_PATCH}")
message(STATUS "EXTRA:                 ${VERSION_EXTRA}")
message(STATUS "Build system:          ${CMAKE_HOST_SYSTEM_NAME} ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message(STATUS "Target system:         ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "C compiler:            ${CMAKE_C_COMPILER}")
message(STATUS "Build type:            ${CMAKE_BUILD_TYPE}")
message(STATUS "HAVE_ASAN:             ${HAVE_ASAN}")
message(STATUS "FYAI_MOSTLY_STATIC:    ${FYAI_MOSTLY_STATIC}")
message(STATUS "FYAI_STATIC:           ${FYAI_STATIC}")
message(STATUS "FYAI_MUSL_STATIC:      ${FYAI_MUSL_STATIC}")
