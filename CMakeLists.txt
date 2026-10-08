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

# Embed an install-time rpath so an installed binary finds its shared
# libraries (libfymd4c, libfyts) at their installed location instead of
# relying on the build tree's rpath.
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
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
include(ProcessorCount)
include(CTest)
include(FetchContent)

ProcessorCount(NUM_PROCS)

# Use two workers when processor detection fails.
if(NUM_PROCS EQUAL 0)
    set(NUM_PROCS 2)
endif()

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
    add_library(libfyaml::fyaml ALIAS libfyaml::libfyaml)
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
        GIT_TAG b545aebc996bedc0733259ba488dcf89150af8e5
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

# One line per libfy* dependency: name and pinned git tag. fyai_fy_dep()
# (cmake/FyDeps.cmake) reuses an existing target, else an installed package,
# else fetches the tag; FYAI_LIB<NAME>_TARGET picks the _static target when
# FYAI_STATIC_DEPS is on. libfypalette comes first: libfymd4c and
# libfymermaid consume it.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FyDeps.cmake)
fyai_fy_dep(libfypalette 3cc484148aeb10d7c7f3a3772173c42c51dad567)
fyai_fy_dep(libfymd4c df8293f6502a897a2931d1fb0aa792d74986f574)

if(FYAI_STATIC_DEPS AND TARGET libfyaml::fyaml_static)
    set_target_properties(libfyaml::fyaml_static PROPERTIES
        INTERFACE_LINK_LIBRARIES "Threads::Threads;m")
endif()

fyai_fy_dep(libfymermaid 59727e84ce577af7854b37f3f89213b368604c04)
fyai_fy_dep(libfytimui 814ad84eebcab4c69b2afce9b989d4dbea291af0)
# libfyvterm: the terminal emulator behind the terminal view and the terminal
# rendering tests.
fyai_fy_dep(libfyvterm b71ae17bd756d95291f352648f5adbc3633ee266)

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
        GIT_TAG curl-8_22_0
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

# Embed a source file as a byte array in a generated header:
#   fyai_embed(SYMBOL header.inc path/relative/to/source)
# The header defines SYMBOL[] and SYMBOL_LEN. A change of the file configures
# again, which writes the header again.
function(fyai_embed symbol header path)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
    file(READ "${_src}" _hex HEX)
    string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," _bytes "${_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${header}"
        "/* Generated from ${_src}. */\n"
        "static const unsigned char ${symbol}[] = {\n"
        "${_bytes}"
        "\n};\n"
        "static const size_t ${symbol}_LEN = sizeof(${symbol});\n"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
endfunction()

# Markdown styling and theming are fully delegated to libfymd4c's embedded theme
# catalogue (selected by the display/theme config key); fyai ships no
# styling YAML of its own.

# Embed the vendored provider/model catalogue snapshot (a scrape-providers
# document) as the out-of-the-box fallback; `fyai catalog import` supersedes
# it with an arena-resident catalogue.
fyai_embed(FYAI_EMBEDDED_CATALOG embedded_catalog.inc data/catalog.yaml)

# Embed the catalogue schema that a catalogue edit is checked against.
fyai_embed(FYAI_EMBEDDED_CATALOG_SCHEMA embedded_catalog_schema.inc
    data/catalog.schema.yaml)

# Embed the sample configuration so `fyai init` with no config file has a
# sensible starting document instead of committing an empty config.
fyai_embed(FYAI_EMBEDDED_CONFIG embedded_config.inc config.yaml.sample)

# Embed the config document JSON Schema (`fyai config validate`).
fyai_embed(FYAI_EMBEDDED_CONFIG_SCHEMA embedded_config_schema.inc
    data/config.schema.yaml)

# Embed the built-in tool specifications in wire shape.
fyai_embed(FYAI_EMBEDDED_TOOLS embedded_tools.inc data/tools.yaml)

# Embed the page document that the page renderer transcribes.
fyai_embed(FYAI_EMBEDDED_PAGE embedded_page.inc data/page.yaml)

# Embed the schema that a page document is checked against when it loads.
fyai_embed(FYAI_EMBEDDED_PAGE_SCHEMA embedded_page_schema.inc
    data/page.schema.yaml)

# Embed the command definitions and their schema (src/fyai_cmd.c).
fyai_embed(FYAI_EMBEDDED_COMMANDS embedded_commands.inc data/commands.yaml)
fyai_embed(FYAI_EMBEDDED_COMMAND_SCHEMA embedded_command_schema.inc
    data/command.schema.yaml)

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
# execinfo/backtrace() for the fatal-signal backtrace (fyai_crash.c).
check_include_file("execinfo.h" HAVE_EXECINFO)
if(HAVE_EXECINFO)
    check_function_exists(backtrace HAVE_BACKTRACE)
    check_function_exists(backtrace_symbols_fd HAVE_BACKTRACE_SYMBOLS_FD)
endif()
if(HAVE_EXECINFO AND HAVE_BACKTRACE AND HAVE_BACKTRACE_SYMBOLS_FD)
    set(FYAI_HAVE_BACKTRACE ON)
else()
    set(FYAI_HAVE_BACKTRACE OFF)
endif()
message(STATUS "fyai: crash backtrace ${FYAI_HAVE_BACKTRACE}")

set(FYAI_SOURCES
    src/fyai_cas.c
    src/fyai_scratch.c
    src/fyai_ignore.c
    src/fyai_view_fs.c
    src/fyai_manifest.c
    src/fyai_wire.c
    src/fyai_project.c
    src/fyai_project_diff.c
    src/fyai_project_capture.c
    src/fyai_fsview.c
    src/fyai_view_apply.c
    src/fyai_view_gc.c
    ${FYAI_EVENT_SOURCES}
    src/fyai_crash.c
    src/fyai_display.c
    src/fyai_import.c
    src/fyai_foreign_import.c
    src/fyai_flow.c
    src/fyai_output.c
    src/fyai.c
    src/fyai_agent.c
    src/fyai_agents.c
    src/fyai_branch.c
    src/fyai_merge.c
    src/fyai_diff.c
    src/fyai_diag.c
    src/fyai_log.c
    src/fyai_markdown.c
    src/fyai_chrome.c
    src/fyai_ui.c
    src/fyai_workpane.c
    src/fyai_page.c
    src/fyai_transcript_view.c
    src/fyai_mcp.c
    src/fyai_mcp_import.c
    src/fyai_jsonrpc.c
    src/fyai_patch.c
    src/fyai_prof.c
    src/fyai_provider.c
    src/fyai_render.c
    src/fyai_sandbox.c
    src/fyai_session.c
    src/fyai_browser.c
    src/fyai_sink.c
    src/fyai_storage.c
    src/fyai_stream.c
    src/fyai_term.c
    src/fyai_wait.c
    src/fyai_terminal.c
    src/fyai_terminal_session.c
    src/fyai_terminal_view.c
    src/fyai_schema.c
    src/fyai_tools.c
    src/fyai_tool_spec.c
    src/fyai_tool_registry.c
    src/fyai_tool_template.c
    src/fyai_turn.c
    src/utils.c
    src/fyai_catalog.c
    src/fyai_cost.c
    src/fyai_config.c
    src/fyai_curl.c
    src/fyai_xfer.c
    src/fyai_transport.c
    src/fyai_transport_msg.c
    src/fyai_transport_server.c
    src/fyai_transport_ctl.c
    src/fyai_transport_verb.c
    src/fyai_transport_client.c
    src/fyai_transport_cfg.c
    src/fyai_transport_boot.c
    src/fyai_auth.c
    src/fyai_oauth.c
    src/fyai_auth_util.c
    src/fyai_secret.c
    src/fyai_redact.c
    src/fyai_cmd.c
    src/fyai_cmd_auth.c
    src/fyai_cmd_branch.c
    src/fyai_cmd_catalog.c
    src/fyai_cmd_complete.c
    src/fyai_cmd_config.c
    src/fyai_cmd_help.c
    src/fyai_cmd_mcp.c
    src/fyai_cmd_session.c
    src/fyai_cmd_page.c
    src/fyai_cmd_misc.c
    src/fyai_cmd_store.c
    src/fyai_cmd_view.c
    src/fyai_cmd_fsview.c
)

configure_file(scripts/git-remote-fyai git-remote-fyai COPYONLY)
install(PROGRAMS scripts/git-remote-fyai DESTINATION bin/)

add_executable(fyai
    src/main.c
    ${FYAI_SOURCES}
)
# Write the command reference, doc/commands.md, from the definitions. The
# case commands_reference fails when the file is out of date.
add_custom_target(docs-commands
    COMMAND sh -c "\"$<TARGET_FILE:fyai>\" help --markdown > \"${CMAKE_CURRENT_SOURCE_DIR}/doc/commands.md\""
    DEPENDS fyai
    COMMENT "Writing doc/commands.md"
    VERBATIM)

# The C23 __isoc23_* redirect wrappers exist only so portable mostly-static
# glibc binaries do not require GLIBC_2.38; fully dynamic builds use the host
# glibc directly (and the interposers break ASAN's own interceptors).
if(FYAI_MOSTLY_STATIC)
    target_sources(fyai PRIVATE src/glibc_compat.c)
endif()
target_compile_options(fyai PRIVATE ${FYAI_C_DIALECT} ${FYAI_C_WARNINGS}
    ${ASAN_C_FLAGS})
target_link_libraries(fyai PRIVATE libfyaml::libfyaml ${FYAI_LIBFYMD4C_TARGET}
    ${FYAI_LIBFYPALETTE_TARGET}
    ${FYAI_LIBFYTIMUI_TARGET} ${FYAI_LIBFYMERMAID_TARGET} CURL::libcurl OpenSSL::Crypto
    ${FYAI_LIBFYVTERM_TARGET} ${ASAN_LINK_OPTS})
if(UNIX AND NOT APPLE)
    target_link_libraries(fyai PRIVATE util)
endif()
# Symbol names for backtrace(): export dynamic symbols on ELF linkers.
# Static links keep every symbol already, so only dynamic builds need it.
if(FYAI_HAVE_BACKTRACE AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   NOT FYAI_STATIC_DEPS)
    target_link_options(fyai PRIVATE -rdynamic)
endif()
if(FYAI_HAVE_BACKTRACE)
    target_compile_definitions(fyai PRIVATE HAVE_EXECINFO)
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
    # Embed the recorded page sources that the page tests compare against.
    fyai_embed(FYAI_EMBEDDED_PAGE_GOLDEN embedded_page_golden.inc
        tests/data/page-source-golden.txt)

    # Embed the schema of the state of the page document for its test.
    fyai_embed(FYAI_EMBEDDED_PAGE_STATE_SCHEMA embedded_page_state_schema.inc
        data/page-state.schema.yaml)

    file(GLOB FYAI_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fyai_*_test.c")
    # The transport channels are message sockets that only Linux has.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(FYAI_TEST_TRANSPORT_MOCK tests/fyai_transport_mock.c)
    else()
        list(FILTER FYAI_TEST_SOURCES EXCLUDE REGEX
            "/fyai_transport_(server_|client_|ctl_)?test\\.c$")
        set(FYAI_TEST_TRANSPORT_MOCK "")
    endif()
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
        tests/fyai_test_scratch.c
        ${FYAI_TEST_TRANSPORT_MOCK}
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
        ${FYAI_LIBFYMD4C_TARGET} ${FYAI_LIBFYPALETTE_TARGET} ${FYAI_LIBFYTIMUI_TARGET}
        ${FYAI_LIBFYMERMAID_TARGET} CURL::libcurl
        OpenSSL::Crypto Threads::Threads ${FYAI_LIBFYVTERM_TARGET}
        ${ASAN_LINK_OPTS})
    if(FYAI_HAVE_BACKTRACE)
        target_compile_definitions(fyai_test PRIVATE HAVE_EXECINFO)
    endif()
    if(ENABLE_LIBSECRET)
        target_link_libraries(fyai_test PRIVATE PkgConfig::LIBSECRET)
        target_compile_definitions(fyai_test PRIVATE HAVE_LIBSECRET)
    endif()
    if(APPLE)
        target_link_libraries(fyai_test PRIVATE
            "-framework Security" "-framework CoreFoundation")
    endif()

    # Add one CTest test for each registry entry. Each runs through
    # tests/unit_run.sh so that a run can put FYAI_VALGRIND in front of the
    # binary, the same variable the functional cases take.
    foreach(_t ${FYAI_TESTS})
        string(REPLACE ":" ";" _parts "${_t}")
        list(GET _parts 0 _suite)
        list(GET _parts 1 _name)
        # A sanitizer takes over the end of a process that crashes, so the
        # report of a crash is not tested under ASAN.
        if(ENABLE_ASAN AND "${_suite}/${_name}" STREQUAL "crash/report")
            continue()
        endif()
        add_test(NAME fyai/unit/${_suite}/${_name}
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_run.sh
                $<TARGET_FILE:fyai_test> ${_suite}/${_name})
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
    set(FYAI_CASE_TIMEOUT_tool_timeout 120)
    set(FYAI_CASE_TIMEOUT_ui_page_layouts 240)
    # The transport cases wait on states with generous failure bounds: a slow
    # runner can stall for minutes, and a pass does not wait for the bound.
    set(FYAI_CASE_TIMEOUT_transport_verb 300)
    set(FYAI_CASE_TIMEOUT_transport_bootstrap 300)
    set(FYAI_CASE_TIMEOUT_transport_persona 300)
    set(FYAI_CASE_TIMEOUT_transport_reload 300)
    set(FYAI_CASE_TIMEOUT_transport_indicator 300)
    set(FYAI_CASE_TIMEOUT_transport_indicator_pty 300)

    # Every deadline in the suite is multiplied by this. A sanitized build, or
    # a suite run with many jobs at once, takes several times as long for the
    # same work, and a case that waits for output must wait that much longer
    # rather than report a failure that says nothing about the code. The cases,
    # the PTY drivers, and the CTest budget all read it. A run raises it
    # without reconfiguring by exporting FYAI_TIMEOUT_SCALE, which the cases
    # prefer to this default; the CTest budget takes `ctest --timeout`.
    if(HAVE_ASAN)
        set(FYAI_TIMEOUT_SCALE_DEFAULT 3)
    else()
        set(FYAI_TIMEOUT_SCALE_DEFAULT 1)
    endif()
    set(FYAI_TIMEOUT_SCALE ${FYAI_TIMEOUT_SCALE_DEFAULT} CACHE STRING
        "Multiplier for every test deadline (integer, 1 or more)")

    file(GLOB FYAI_TEST_CASES ${CMAKE_CURRENT_SOURCE_DIR}/tests/cases/*.sh)
    foreach(case_script ${FYAI_TEST_CASES})
        get_filename_component(case_name ${case_script} NAME_WE)
        set(case_timeout 60)
        if(DEFINED FYAI_CASE_TIMEOUT_${case_name})
            set(case_timeout ${FYAI_CASE_TIMEOUT_${case_name}})
        endif()
        math(EXPR case_timeout "${case_timeout} * ${FYAI_TIMEOUT_SCALE}")
        add_test(NAME fyai/case_${case_name} COMMAND bash ${case_script})
        set_tests_properties(fyai/case_${case_name} PROPERTIES
            TIMEOUT ${case_timeout}
            SKIP_RETURN_CODE 77
            FIXTURES_REQUIRED fyai_binary
            ENVIRONMENT "FYAI_BIN=$<TARGET_FILE:fyai>;PYTHON=${Python3_EXECUTABLE};TESTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests;FYAI_ASAN=${HAVE_ASAN};FYAI_TIMEOUT_SCALE_DEFAULT=${FYAI_TIMEOUT_SCALE}"
        )
        # Only the PTY cases that have actually starved each other on
        # slow runners take the lock: interactive turn legs, live
        # resizes, tool interrupts, and termination. The rest assert
        # through polled needles with room in their budgets, so they run
        # free. Grow this list from failure evidence, not suspicion.
        if(case_name MATCHES "^(ui_interactive|ui_pattern_tty_resize|ui_curses_tty_resize|ui_bang_resize|ui_bang_multi_resize|ui_idle_resize_repaint|ui_band_resize_width|ui_reflow_width|term|shell_tty_resize|ui_tool_interrupt|ui_tool_interrupt_ctrl_c|ui_native_shell_interrupt|ui_mcp_stdio_interrupt|tool_terminate|tool_timeout)$")
            set_tests_properties(fyai/case_${case_name} PROPERTIES
                RESOURCE_LOCK fyai_pty)
        endif()
        # Valgrind cases are nightly-only: memcheck multiplies startup
        # by an order of magnitude and proves nothing per push that
        # ASAN does not already cover. CI excludes this label by
        # default; the nightly leg runs it explicitly.
        if(case_name MATCHES "^vg_")
            set_tests_properties(fyai/case_${case_name} PROPERTIES
                LABELS nightly)
        endif()
    endforeach()

    # One component owns every byte the user sees: no direct stdout/stderr
    # write outside the sink, except the files tests/sink-only-allow.txt
    # explains.
    add_test(NAME fyai/sink_only
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/sink-only.sh
                ${CMAKE_CURRENT_SOURCE_DIR})

    # Static checks over the case scripts: comments inside a continued
    # statement silently drop driver variables, and a misspelled AFTER
    # step fails only after the session budget. Catch both at build time.
    add_test(NAME fyai/case_lint
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/case-lint.sh)
    set_tests_properties(fyai/case_lint PROPERTIES
        FIXTURES_REQUIRED fyai_binary)

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

    add_custom_target(parallel-test
        COMMAND ${CMAKE_CTEST_COMMAND} -j ${NUM_PROCS} --output-on-failure
        COMMENT "Running test suite in parallel using ${NUM_PROCS} CPU cores..."
        USES_TERMINAL)
    # Build test executables before running CTest.
    add_dependencies(parallel-test fyai fyai_test)
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
