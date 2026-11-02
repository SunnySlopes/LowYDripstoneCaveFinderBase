cmake_minimum_required(VERSION 3.15)
project(LowYDripstoneCaveFinderBase LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
# cubiomes rng.h defines a global lerp(); C++20 math.h injects std::lerp into
# the global namespace. cxx_compat/pre_cubiomes.h renames cubiomes' lerp while
# the include guard is installed; the engine itself stays on C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# cubiomes.
#
# cubiomes is a nested submodule of this repository:
#
#   LowYDripstoneCaveFinderBase/cubiomes
#
# A standalone build therefore needs no external source tree. An embedding
# project that already owns a cubiomes checkout can override:
#
#   -DLOWYD_CUBIOMES_DIR=<path-to-cubiomes>
# ---------------------------------------------------------------------------
set(LOWYD_CUBIOMES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cubiomes"
    CACHE PATH "Path to the cubiomes checkout")
if(NOT EXISTS "${LOWYD_CUBIOMES_DIR}/generator.c")
    message(FATAL_ERROR
        "LOWYD_CUBIOMES_DIR does not look like cubiomes: ${LOWYD_CUBIOMES_DIR}\n"
        "Run 'git submodule update --init --recursive' in LowYDripstoneCaveFinderBase, "
        "or pass -DLOWYD_CUBIOMES_DIR=<path-to-cubiomes>.")
endif()

# The sources include "cubiomes/...", so the include root is the parent
# directory of the cubiomes checkout.
get_filename_component(CUBIOMES_INCLUDE_ROOT "${LOWYD_CUBIOMES_DIR}" DIRECTORY)

set(LOWYD_CUBIOMES_SOURCES
    "${LOWYD_CUBIOMES_DIR}/generator.c"
    "${LOWYD_CUBIOMES_DIR}/layers.c"
    "${LOWYD_CUBIOMES_DIR}/biomenoise.c"
    "${LOWYD_CUBIOMES_DIR}/biomes.c"
    "${LOWYD_CUBIOMES_DIR}/noise.c"
    "${LOWYD_CUBIOMES_DIR}/util.c"
)

# ---------------------------------------------------------------------------
# Shared static engine. Both the CLI and the JNI library link this target.
# lowydripstonecavefinder.cpp includes dripstonecave_finder.cpp and
# LargePitSearch.cpp; they must not be compiled separately.
# ---------------------------------------------------------------------------
add_library(lowydripstonecavefinder-core STATIC
    lowydripstonecavefinder.cpp
    BiomeSampler.cpp
    OctaveFieldCache.cpp
    ${LOWYD_CUBIOMES_SOURCES}
)

target_include_directories(lowydripstonecavefinder-core PUBLIC
    "${CUBIOMES_INCLUDE_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(MSVC)
    target_compile_options(lowydripstonecavefinder-core PRIVATE /O2 /DNDEBUG)
else()
    target_compile_options(lowydripstonecavefinder-core PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-O3;-DNDEBUG;-ffp-contract=off>
        $<$<COMPILE_LANGUAGE:CXX>:-O3;-DNDEBUG;-ffp-contract=off;-include;${CMAKE_CURRENT_SOURCE_DIR}/cxx_compat/pre_cubiomes.h>
    )
endif()

if(NOT WIN32)
    target_link_libraries(lowydripstonecavefinder-core PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# Standalone command-line program (self-contained; statically linked on MinGW).
# ---------------------------------------------------------------------------
option(LOWYDRIPSTONE_BUILD_CLI "Build the standalone lowydripstonecavefinder-cli" ON)
if(LOWYDRIPSTONE_BUILD_CLI)
    add_executable(lowydripstonecavefinder-cli lowydripstonecavefinder_cli.cpp)
    target_link_libraries(lowydripstonecavefinder-cli PRIVATE lowydripstonecavefinder-core)
    if(WIN32)
        # --bench reports the process peak working set (GetProcessMemoryInfo).
        target_link_libraries(lowydripstonecavefinder-cli PRIVATE psapi)
    endif()
    set_target_properties(lowydripstonecavefinder-cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    if(MINGW)
        target_link_options(lowydripstonecavefinder-cli PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
            -pthread
        )
    endif()
    if(NOT MSVC)
        target_compile_options(lowydripstonecavefinder-cli PRIVATE -O2 -DNDEBUG)
    endif()
endif()

# ---------------------------------------------------------------------------
# JNI library for the LowYDripstoneCaveFinder Java GUI.
# ---------------------------------------------------------------------------
option(LOWYDRIPSTONE_BUILD_JNI "Build the libDripstoneCaveFinderLibJ JNI library" ON)
if(LOWYDRIPSTONE_BUILD_JNI)
    add_subdirectory(jni)
endif()
