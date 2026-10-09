# ── NIFFER ────────────────────────────────────────────────────────────────────
# Clean-room C++23 NIF (NetImmerse/Gamebryo) library for Skyrim Special Edition
# ONLY (NIF 20.2.0.7, user version 12, BS stream version 100), plus FaceGen TRI
# (FRTRI003). Byte-exact Load → Save round-trip.
#
# Consumed via FetchContent; consumers link `niffer::niffer` and
# `#include <niffer/Niffer.h>`. Layout:
#   include/niffer/Niffer.h  — the ONE public header
#   hpp/                     — private component headers (Stream, registry)
#   cpp/                     — the impl (cpp/blocks/ = typed block layouts)
#   tests/                   — synthetic self-verifying tests (CTest)
#   tools/                   — niffer-tool, the real-data corpus gates
#
# Dependency-free LEAF library. The MSVC runtime (static /MT vs dynamic /MD) is
# deliberately NOT set here — it inherits CMAKE_MSVC_RUNTIME_LIBRARY from the
# consuming project, so the same source serves SKSE plugins and desktop apps.

cmake_minimum_required(VERSION 3.28)

project(NIFFER
    VERSION 0.1.0
    DESCRIPTION "Clean-room C++23 NIF + FaceGen TRI library for Skyrim Special Edition"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(NIFFER_BUILD_TESTS "Build NIFFER tests" ${PROJECT_IS_TOP_LEVEL})
option(NIFFER_BUILD_TOOLS "Build niffer-tool (the real-data corpus gates)" OFF)

# ── niffer ────────────────────────────────────────────────────────────────────
file(GLOB_RECURSE NIFFER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")
file(GLOB_RECURSE NIFFER_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/hpp/*.h")

add_library(niffer STATIC ${NIFFER_SOURCES} ${NIFFER_HEADERS})
add_library(niffer::niffer ALIAS niffer)

# PUBLIC = the single public header; PRIVATE hpp/ = internals, included as "Foo.h".
target_include_directories(niffer
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hpp")
target_compile_features(niffer PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(niffer PRIVATE /W4 /wd4100 /utf-8)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${NIFFER_SOURCES} ${NIFFER_HEADERS})

# ── Tests (Tier 1: self-verifying synthetic round-trips) ──────────────────────
if(NIFFER_BUILD_TESTS)
    enable_testing()
    file(GLOB NIFFER_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    add_executable(niffer-test ${NIFFER_TEST_SOURCES})
    target_link_libraries(niffer-test PRIVATE niffer::niffer)
    # Whitebox: tests may include the lib's INTERNAL headers directly ("Foo.h").
    target_include_directories(niffer-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hpp")
    if(MSVC)
        target_compile_options(niffer-test PRIVATE /utf-8)
    endif()
    add_test(NAME niffer-test COMMAND niffer-test)
endif()

# ── Tools (Tier 2: vanilla-corpus gates; need game data, not ctests) ──────────
#   niffer-tool histogram|roundtrip|dump — see tools/niffer_tool.cpp
if(NIFFER_BUILD_TOOLS)
    add_executable(niffer-tool tools/niffer_tool.cpp)
    target_link_libraries(niffer-tool PRIVATE niffer::niffer)
    # Whitebox like the tests: the tool dumps internal block/framing state.
    target_include_directories(niffer-tool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hpp")
    if(MSVC)
        target_compile_options(niffer-tool PRIVATE /utf-8)
    endif()
endif()
