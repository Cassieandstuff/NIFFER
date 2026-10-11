# ── NIFFER ────────────────────────────────────────────────────────────────────
# Clean-room C++23 NIF (NetImmerse/Gamebryo) library for Skyrim Special Edition
# ONLY (NIF 20.2.0.7, user version 12, BS stream version 100), plus FaceGen TRI
# (FRTRI003). Byte-exact Load → Save round-trip.
#
# Consumed via FetchContent; consumers link `niffer::niffer` and
# `#include <niffer/Niffer.h>`. Layout:
#   include/niffer/Niffer.h  — the ONE header consumers include (the umbrella)
#   include/niffer/Schema.h  — the schema engine, behind the umbrella
#   include/niffer/generated — generated handles + views, behind the umbrella
#   hpp/                     — private headers (Stream, framing, collision,
#                              generated/Tables.h)
#   cpp/                     — the impl (cpp/engine/ = the schema engine,
#                              cpp/collision/ = ExtractCollision)
#   schema/                  — the YAML schema (docs/schema-language.md)
#   tests/                   — synthetic self-verifying tests (CTest)
#   tools/                   — niffer-tool (real-data gates), schemagen
#
# Dependency-free LEAF library. The MSVC runtime (static /MT vs dynamic /MD) is
# deliberately NOT set here — it inherits CMAKE_MSVC_RUNTIME_LIBRARY from the
# consuming project, so the same source serves SKSE plugins and desktop apps.

cmake_minimum_required(VERSION 3.28)

project(NIFFER
    VERSION 0.2.0
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

# ── Schema generator (contributors only) ──────────────────────────────────────
# schema/ is compiled into schema.json + include/niffer/generated/*.h +
# hpp/generated/Tables.h by tools/schemagen. The output is committed, so
# consumers never need Python; these targets exist only where Python 3 +
# PyYAML are available.
set(NIFFER_SCHEMAGEN_AVAILABLE OFF)
if(PROJECT_IS_TOP_LEVEL)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import yaml"
                        RESULT_VARIABLE _niffer_yaml_rc OUTPUT_QUIET ERROR_QUIET)
        if(_niffer_yaml_rc EQUAL 0)
            set(NIFFER_SCHEMAGEN_AVAILABLE ON)
        endif()
    endif()
endif()

set(_NIFFER_SCHEMAGEN "${CMAKE_CURRENT_SOURCE_DIR}/tools/schemagen/schemagen.py")
set(_NIFFER_GEN_MAIN
    --schema "${CMAKE_CURRENT_SOURCE_DIR}/schema"
    --out "${CMAKE_CURRENT_SOURCE_DIR}/include/niffer/generated"
    --tables "${CMAKE_CURRENT_SOURCE_DIR}/hpp/generated/Tables.h"
    --json "${CMAKE_CURRENT_SOURCE_DIR}/schema.json")
set(_NIFFER_GEN_TEST
    --schema "${CMAKE_CURRENT_SOURCE_DIR}/tests/schema"
    --out "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated"
    --tables "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated/Tables.h"
    --tables-ns niffer_test::tables --fields-ns niffer_test::fields --views-ns niffer_test::view)

if(NIFFER_SCHEMAGEN_AVAILABLE)
    add_custom_target(niffer-schemagen
        COMMAND "${Python3_EXECUTABLE}" -I "${_NIFFER_SCHEMAGEN}" ${_NIFFER_GEN_MAIN}
        COMMAND "${Python3_EXECUTABLE}" -I "${_NIFFER_SCHEMAGEN}" ${_NIFFER_GEN_TEST}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Regenerating NIFFER schema outputs"
        VERBATIM)
endif()

# ── Tests (Tier 1: self-verifying synthetic round-trips) ──────────────────────
if(NIFFER_BUILD_TESTS)
    enable_testing()
    file(GLOB NIFFER_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    add_executable(niffer-test ${NIFFER_TEST_SOURCES})
    target_link_libraries(niffer-test PRIVATE niffer::niffer)
    # Whitebox: tests may include the lib's INTERNAL headers directly ("Foo.h").
    # The test schema's generated headers are included relative to tests/.
    target_include_directories(niffer-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hpp")
    if(MSVC)
        target_compile_options(niffer-test PRIVATE /utf-8)
    endif()
    add_test(NAME niffer-test COMMAND niffer-test)

    if(NIFFER_SCHEMAGEN_AVAILABLE)
        add_test(NAME niffer-schema-fresh
            COMMAND "${Python3_EXECUTABLE}" -I "${_NIFFER_SCHEMAGEN}" ${_NIFFER_GEN_MAIN} --check)
        add_test(NAME niffer-test-schema-fresh
            COMMAND "${Python3_EXECUTABLE}" -I "${_NIFFER_SCHEMAGEN}" ${_NIFFER_GEN_TEST} --check)
        add_test(NAME niffer-schemagen-test
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/schemagen_test.py")
    endif()
endif()

# ── Tools (Tier 2: vanilla-corpus gates; need game data, not ctests) ──────────
#   niffer-tool roundtrip|tri-roundtrip|collision-validate|… — see tools/niffer_tool.cpp
if(NIFFER_BUILD_TOOLS)
    add_executable(niffer-tool tools/niffer_tool.cpp)
    target_link_libraries(niffer-tool PRIVATE niffer::niffer)
    # Whitebox like the tests: the tool reads the header and collision internals.
    target_include_directories(niffer-tool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hpp")
    if(MSVC)
        target_compile_options(niffer-tool PRIVATE /utf-8)
    endif()
endif()
