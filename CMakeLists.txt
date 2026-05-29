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

if(HAVE_ASAN)
  set(ASAN_C_FLAGS "-fsanitize=address,signed-integer-overflow,undefined" "-fno-omit-frame-pointer")
endif()

# Find libfyaml
find_package(libfyaml 1.0 QUIET)

if(NOT libfyaml_FOUND)
    # Fall back to pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBFYAML REQUIRED libfyaml)

    # Create imported target for consistency
    add_library(libfyaml::libfyaml INTERFACE IMPORTED)
    set_target_properties(libfyaml::libfyaml PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBFYAML_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${LIBFYAML_LIBRARIES}"
        INTERFACE_LINK_DIRECTORIES "${LIBFYAML_LIBRARY_DIRS}"
        INTERFACE_COMPILE_OPTIONS "${LIBFYAML_CFLAGS_OTHER}"
    )
endif()

option(ENABLE_ASAN "Enable ASAN support" OFF)
option(BUILD_TESTING "Build tests" ON)

if(ENABLE_ASAN)
  set(ASAN_C_FLAGS "-fsanitize=address,signed-integer-overflow,undefined" "-fno-omit-frame-pointer")
endif()

set(FYAI_PROGRAMS fyai)

# fyai
add_executable(fyai main.c)
target_compile_options(fyai PRIVATE -std=gnu2x -Wall -Wextra ${ASAN_CFLAGS})
target_link_libraries(fyai PRIVATE libfyaml::libfyaml)

# Installation (optional)
install(TARGETS
	${FYAI_PROGRAMS}
	RUNTIME DESTINATION bin/
)

# Install sample files
install(FILES
    README.md
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
if(libfyaml_FOUND)
    message(STATUS "  Found libfyaml via CMake config")
else()
    message(STATUS "  Found libfyaml via pkg-config")
endif()
