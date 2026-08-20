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
    set(_FYAI_BUILD_TESTING "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    FetchContent_Declare(libfyaml
        GIT_REPOSITORY https://github.com/pantoniou/libfyaml
        GIT_TAG v1.0.0-beta1
    )
    FetchContent_MakeAvailable(libfyaml)
    set(BUILD_TESTING "${_FYAI_BUILD_TESTING}")
    unset(_FYAI_BUILD_TESTING)
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
    set(BUILD_FYMD4C_EXECUTABLE OFF CACHE BOOL
        "Build the libfymd4c command-line executable" FORCE)
    FetchContent_Declare(libfymd4c
        GIT_REPOSITORY https://github.com/pantoniou/libfymd4c
        GIT_TAG eaf7ee05ef5cb67bedbbc78ce7170610f33f3b30
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

if(NOT TARGET libfytimui::libfytimui AND
   NOT TARGET libfytimui::fytimui)
    find_package(libfytimui CONFIG QUIET)
endif()
if(NOT TARGET libfytimui::libfytimui AND
   NOT TARGET libfytimui::fytimui AND
   NOT TARGET libfytimui::libfytimui_static AND
   NOT TARGET libfytimui::fytimui_static)
    FetchContent_Declare(libfytimui
        GIT_REPOSITORY https://github.com/pantoniou/libfytimui
        GIT_TAG f6b12ada4d66fdf901d448e67853217b66757393
    )
    set(BUILD_FYTIMUI_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_TIMUI_CORE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libfytimui)
endif()
if(FYAI_STATIC_DEPS AND TARGET libfytimui::libfytimui_static)
    set(FYAI_LIBFYTIMUI_TARGET libfytimui::libfytimui_static)
elseif(FYAI_STATIC_DEPS AND TARGET libfytimui::fytimui_static)
    set(FYAI_LIBFYTIMUI_TARGET libfytimui::fytimui_static)
elseif(TARGET libfytimui::libfytimui)
    set(FYAI_LIBFYTIMUI_TARGET libfytimui::libfytimui)
else()
    set(FYAI_LIBFYTIMUI_TARGET libfytimui::fytimui)
endif()

# libfyvterm: the terminal emulator behind the terminal view and the terminal
# rendering tests. A parent project may already provide the target (its own
# find_package or add_subdirectory); reuse that rather than re-resolving.
if(NOT TARGET libfyvterm::libfyvterm)
    find_package(libfyvterm CONFIG REQUIRED)
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

option(ENABLE_ASAN "Enable ASAN support" OFF)
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_LIBSECRET "Store credentials in the Linux Secret Service" OFF)

set(FYAI_PROGRAMS fyai)

# Report the five most complex functions when pmccabe is available.
find_program(FYAI_PMCCABE_EXECUTABLE pmccabe)
if(FYAI_PMCCABE_EXECUTABLE)
    add_custom_target(pmccabe
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/pmccabe-scan.sh"
            "${FYAI_PMCCABE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM)
endif()

# Markdown styling and theming are fully delegated to libfymd4c's embedded theme
# catalogue (selected by the display/theme config key); fyai ships no
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

# Embed the built-in tool specifications in wire shape.
set(FYAI_TOOLS_YAML "${CMAKE_CURRENT_SOURCE_DIR}/data/tools.yaml")
file(READ "${FYAI_TOOLS_YAML}" _fyai_tools_hex HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1,"
    _fyai_tools_bytes "${_fyai_tools_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/embedded_tools.inc"
    "/* Generated from ${FYAI_TOOLS_YAML}. */\n"
    "static const unsigned char FYAI_EMBEDDED_TOOLS[] = {\n"
    "${_fyai_tools_bytes}"
    "\n};\n"
    "static const size_t FYAI_EMBEDDED_TOOLS_LEN = sizeof(FYAI_EMBEDDED_TOOLS);\n"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FYAI_TOOLS_YAML}")

# Generate config.h
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/config.h"
)

# fyai
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
if(ENABLE_LIBSECRET)
    if(FYAI_STATIC_DEPS)
        message(FATAL_ERROR "ENABLE_LIBSECRET is incompatible with static dependency builds")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
endif()
# The event-poll layer: one backend-independent core plus exactly one native
# backend. else() rather than a UNIX check on purpose - an unsupported platform
# then fails at link with a clear missing symbol instead of quietly building a
# loop with no backend behind it.
# The event layer reports through the diagnostic layer, so any target that
# links it needs fyai_diag.c as well.
set(FYAI_DIAG_SOURCES src/fyai_diag.c)
set(FYAI_EVENT_SOURCES src/fyai_event.c src/fyai_event_dump.c)
if(APPLE)
    list(APPEND FYAI_EVENT_SOURCES src/fyai_event_mac.c)
else()
    list(APPEND FYAI_EVENT_SOURCES src/fyai_event_linux.c)
endif()

# List the production sources except src/main.c.
# Use this list for the fyai and fyai_test binaries.
set(FYAI_SOURCES
    ${FYAI_EVENT_SOURCES}
    src/fyai_display.c
    src/fyai_import.c
    src/fyai_output.c
    src/fyai.c
    src/fyai_agent.c
    src/fyai_branch.c
    src/fyai_merge.c
    src/fyai_diag.c
    src/fyai_log.c
    src/fyai_markdown.c
    src/fyai_ui.c
    src/fyai_mcp.c
    src/fyai_mcp_import.c
    src/fyai_jsonrpc.c
    src/fyai_patch.c
    src/fyai_prof.c
    src/fyai_provider.c
    src/fyai_render.c
    src/fyai_sandbox.c
    src/fyai_session.c
    src/fyai_sink.c
    src/fyai_storage.c
    src/fyai_stream.c
    src/fyai_terminal.c
    src/fyai_terminal_session.c
    src/fyai_schema.c
    src/fyai_tools.c
    src/fyai_tool_spec.c
    src/fyai_turn.c
    src/utils.c
    src/fyai_catalog.c
    src/fyai_config.c
    src/fyai_curl.c
    src/fyai_auth.c
    src/fyai_oauth.c
    src/fyai_auth_util.c
    src/fyai_secret.c
    src/commands.c
)

add_executable(fyai
    src/main.c
    ${FYAI_SOURCES}
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
    ${FYAI_LIBFYTIMUI_TARGET} CURL::libcurl OpenSSL::Crypto
    libfyvterm::libfyvterm ${ASAN_LINK_OPTS})
if(UNIX AND NOT APPLE)
    target_link_libraries(fyai PRIVATE util)
endif()
if(ENABLE_LIBSECRET)
    target_link_libraries(fyai PRIVATE PkgConfig::LIBSECRET)
    target_compile_definitions(fyai PRIVATE HAVE_LIBSECRET)
endif()
if(APPLE)
    target_link_libraries(fyai PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
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
    # The diagnostics test races raisers against each other to prove the
    # lock-free publish drops nothing.
    find_package(Threads REQUIRED)

    # Build all unit tests in one binary.
    #
    # Add a test function to a tests/fyai_*_test.c file. Add one
    # FYAI_TEST_ENTRY() line after the includes:
    #
    #     FYAI_TEST_ENTRY(suite, test_name, entry_function)
    #
    # CMake uses these entries to generate the registry and CTest tests.
    file(GLOB FYAI_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fyai_*_test.c")
    set(FYAI_TESTS "")
    foreach(_test_source ${FYAI_TEST_SOURCES})
        file(STRINGS "${_test_source}" _test_entry_lines
            REGEX "^FYAI_TEST_ENTRY\\(")
        foreach(_line ${_test_entry_lines})
            # Parse each identifier separately.
            if(NOT _line MATCHES
                    "^FYAI_TEST_ENTRY\\([ \t]*([A-Za-z0-9_]+)")
                message(FATAL_ERROR
                    "malformed FYAI_TEST_ENTRY in ${_test_source}: ${_line}")
            endif()
            set(_suite "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "^FYAI_TEST_ENTRY\\([ \t]*[A-Za-z0-9_]+[ \t]*,[ \t]*" "" _rest "${_line}")
            if(NOT _rest MATCHES "^([A-Za-z0-9_]+)")
                message(FATAL_ERROR
                    "malformed FYAI_TEST_ENTRY in ${_test_source}: ${_line}")
            endif()
            set(_name "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "^[A-Za-z0-9_]+[ \t]*,[ \t]*" "" _rest "${_rest}")
            if(NOT _rest MATCHES "^([A-Za-z0-9_]+)[ \t]*\\)$")
                message(FATAL_ERROR
                    "malformed FYAI_TEST_ENTRY in ${_test_source}: ${_line}")
            endif()
            set(_entry "${CMAKE_MATCH_1}")
            list(APPEND FYAI_TESTS "${_suite}:${_name}:${_entry}")
        endforeach()
    endforeach()
    list(SORT FYAI_TESTS)

    # Generate the test registry.
    set(FYAI_TEST_REGISTER_C "${CMAKE_CURRENT_BINARY_DIR}/fyai_test_register.c")
    set(_reg "/* Generated by CMake from FYAI_TEST_ENTRY lines. Do not edit. */\n")
    string(APPEND _reg "#include \"fyai_test_registry.h\"\n\n")
    foreach(_t ${FYAI_TESTS})
        string(REPLACE ":" ";" _parts "${_t}")
        list(GET _parts 2 _entry)
        string(APPEND _reg "extern int ${_entry}(void);\n")
    endforeach()
    string(APPEND _reg "\nvoid fyai_test_register_all(void)\n{\n")
    foreach(_t ${FYAI_TESTS})
        string(REPLACE ":" ";" _parts "${_t}")
        list(GET _parts 0 _suite)
        list(GET _parts 1 _name)
        list(GET _parts 2 _entry)
        string(APPEND _reg
            "\tfyai_test_register(\"${_suite}\", \"${_name}\", ${_entry});\n")
    endforeach()
    string(APPEND _reg "}\n")
    file(WRITE "${FYAI_TEST_REGISTER_C}" "${_reg}")

    add_executable(fyai_test
        ${FYAI_SOURCES}
        ${FYAI_TEST_SOURCES}
        tests/fyai_test_registry.c
        tests/fyai_test_main.c
        "${FYAI_TEST_REGISTER_C}"
    )
    if(FYAI_MOSTLY_STATIC)
        target_sources(fyai_test PRIVATE src/glibc_compat.c)
    endif()
    target_compile_options(fyai_test PRIVATE ${FYAI_C_DIALECT}
        ${FYAI_C_WARNINGS} ${ASAN_C_FLAGS})
    target_compile_definitions(fyai_test PRIVATE
        _GNU_SOURCE
        HAVE_CONFIG_H
        VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(fyai_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(fyai_test PRIVATE libfyaml::libfyaml
        ${FYAI_LIBFYMD4C_TARGET} ${FYAI_LIBFYTIMUI_TARGET} CURL::libcurl
        OpenSSL::Crypto Threads::Threads libfyvterm::libfyvterm
        ${ASAN_LINK_OPTS})
    if(ENABLE_LIBSECRET)
        target_link_libraries(fyai_test PRIVATE PkgConfig::LIBSECRET)
        target_compile_definitions(fyai_test PRIVATE HAVE_LIBSECRET)
    endif()
    if(APPLE)
        target_link_libraries(fyai_test PRIVATE
            "-framework Security" "-framework CoreFoundation")
    endif()

    # Add one CTest test for each registry entry.
    foreach(_t ${FYAI_TESTS})
        string(REPLACE ":" ";" _parts "${_t}")
        list(GET _parts 0 _suite)
        list(GET _parts 1 _name)
        add_test(NAME fyai/unit/${_suite}/${_name}
            COMMAND fyai_test ${_suite}/${_name})
    endforeach()

    # Functional cases: each script drives the real fyai binary against the
    # scenario-driven mock provider (tests/mock/mock_provider.py) in a
    # hermetic sandbox (private HOME/XDG_*, localhost only).
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # One preflight test reports a missing binary. Without it an incomplete
    # build produces the same "FYAI_BIN not executable" failure once per case,
    # and the build failure behind them is lost in the list.
    add_test(NAME fyai/preflight
        COMMAND ${CMAKE_COMMAND} -E true)
    set_tests_properties(fyai/preflight PROPERTIES
        FIXTURES_SETUP fyai_binary
        REQUIRED_FILES $<TARGET_FILE:fyai>)

    # Cases that need more wall clock than the common budget.
    set(FYAI_CASE_TIMEOUT_tool_terminate 120)

    file(GLOB FYAI_TEST_CASES ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/*.sh)
    foreach(case_script ${FYAI_TEST_CASES})
        get_filename_component(case_name ${case_script} NAME_WE)
        set(case_timeout 60)
        if(DEFINED FYAI_CASE_TIMEOUT_${case_name})
            set(case_timeout ${FYAI_CASE_TIMEOUT_${case_name}})
        endif()
        add_test(NAME fyai/case_${case_name} COMMAND bash ${case_script})
        set_tests_properties(fyai/case_${case_name} PROPERTIES
            TIMEOUT ${case_timeout}
            FIXTURES_REQUIRED fyai_binary
            ENVIRONMENT "FYAI_BIN=$<TARGET_FILE:fyai>;PYTHON=${Python3_EXECUTABLE};TESTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests;FYAI_ASAN=${HAVE_ASAN}"
        )
    endforeach()

    # One component owns every byte the user sees: no direct stdout/stderr
    # write outside the sink, except the files tests/sink-only-allow.txt
    # explains.
    add_test(NAME fyai/sink_only
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/sink-only.sh
                ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(NAME fyai/cli_dump_conflicting_selectors
        COMMAND fyai dump --first 1 --last 1)
    set_tests_properties(fyai/cli_dump_conflicting_selectors PROPERTIES
        WILL_FAIL TRUE)
    add_test(NAME fyai/cli_history_invalid_range
        COMMAND fyai history --range invalid)
    set_tests_properties(fyai/cli_history_invalid_range PROPERTIES
        WILL_FAIL TRUE)
    add_test(NAME fyai/cli_invalid_option
        COMMAND fyai --invalid-option)
    set_tests_properties(fyai/cli_invalid_option PROPERTIES
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
message(STATUS "ENABLE_LIBSECRET:      ${ENABLE_LIBSECRET}")
message(STATUS "FYAI_STATIC:           ${FYAI_STATIC}")
message(STATUS "FYAI_MUSL_STATIC:      ${FYAI_MUSL_STATIC}")
