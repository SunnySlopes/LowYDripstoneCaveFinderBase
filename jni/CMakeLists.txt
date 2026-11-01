cmake_minimum_required(VERSION 3.15)

project(DripstoneCaveFinderLibJ LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# JNI headers.
#
# FindJNI breaks when JAVA_HOME contains spaces on Windows (e.g.
# C:\Program Files\Java\...). Resolve the JDK manually and let Gradle pass
# -DJAVA_HOME=... with forward slashes.
# ---------------------------------------------------------------------------
if(DEFINED JAVA_HOME)
    set(_JAVA_HOME "${JAVA_HOME}")
elseif(DEFINED ENV{JAVA_HOME})
    set(_JAVA_HOME "$ENV{JAVA_HOME}")
else()
    find_program(_JAVA_EXE java REQUIRED)
    get_filename_component(_JAVA_BIN "${_JAVA_EXE}" DIRECTORY)
    get_filename_component(_JAVA_HOME "${_JAVA_BIN}" DIRECTORY)
endif()

file(TO_CMAKE_PATH "${_JAVA_HOME}" _JAVA_HOME_CMAKE)

set(JNI_INCLUDE_DIRS "${_JAVA_HOME_CMAKE}/include")
if(WIN32)
    list(APPEND JNI_INCLUDE_DIRS "${_JAVA_HOME_CMAKE}/include/win32")
elseif(APPLE)
    list(APPEND JNI_INCLUDE_DIRS "${_JAVA_HOME_CMAKE}/include/darwin")
else()
    list(APPEND JNI_INCLUDE_DIRS "${_JAVA_HOME_CMAKE}/include/linux")
endif()

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The JNI header is checked in so LowYDripstoneCaveFinderBase builds without
# the Java sources. Whenever LowYDripstoneCaveFinderBridge.java changes,
# regenerate it and copy it here; the main module's verifyJniContract task
# enforces the match.
set(JNI_GEN_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge.h")
if(NOT EXISTS "${JNI_GEN_HEADER}")
    message(FATAL_ERROR "Missing JNI header: ${JNI_GEN_HEADER}")
endif()

set(LOWYD_JNI_SOURCES
    sunnyslopes_lowydscavefinder_LowYDripstoneCaveFinderBridge.cpp
    ${JNI_GEN_HEADER}
)

# When configured from the repository root, link the shared static engine.
# When configured standalone (cmake -S jni -B ...), compile the engine and the
# nested cubiomes checkout directly so this directory remains self-contained.
if(TARGET lowydripstonecavefinder-core)
    add_library(DripstoneCaveFinderLibJ SHARED ${LOWYD_JNI_SOURCES})
    target_link_libraries(DripstoneCaveFinderLibJ PRIVATE lowydripstonecavefinder-core)
    target_include_directories(DripstoneCaveFinderLibJ PRIVATE
        ${JNI_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(NOT MSVC)
        # The bridge includes cubiomes headers too; the forced include keeps
        # cubiomes' global lerp() away from C++20 std::lerp.
        target_compile_options(DripstoneCaveFinderLibJ PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-include;${PROJECT_ROOT}/cxx_compat/pre_cubiomes.h>
        )
    endif()
else()
    set(LOWYD_CUBIOMES_DIR "${PROJECT_ROOT}/cubiomes"
        CACHE PATH "Path to the cubiomes checkout")
    if(NOT EXISTS "${LOWYD_CUBIOMES_DIR}/generator.c")
        message(FATAL_ERROR
            "LOWYD_CUBIOMES_DIR does not look like cubiomes: ${LOWYD_CUBIOMES_DIR}\n"
            "Run 'git submodule update --init --recursive' in LowYDripstoneCaveFinderBase.")
    endif()
    get_filename_component(CUBIOMES_INCLUDE_ROOT "${LOWYD_CUBIOMES_DIR}" DIRECTORY)

    add_library(DripstoneCaveFinderLibJ SHARED
        ${LOWYD_JNI_SOURCES}
        "${PROJECT_ROOT}/lowydripstonecavefinder.cpp"
        "${PROJECT_ROOT}/BiomeSampler.cpp"
        "${PROJECT_ROOT}/OctaveFieldCache.cpp"
        "${LOWYD_CUBIOMES_DIR}/generator.c"
        "${LOWYD_CUBIOMES_DIR}/layers.c"
        "${LOWYD_CUBIOMES_DIR}/biomenoise.c"
        "${LOWYD_CUBIOMES_DIR}/biomes.c"
        "${LOWYD_CUBIOMES_DIR}/noise.c"
        "${LOWYD_CUBIOMES_DIR}/util.c"
    )
    target_include_directories(DripstoneCaveFinderLibJ PRIVATE
        ${JNI_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_ROOT}"
    )
    if(NOT MSVC)
        target_compile_options(DripstoneCaveFinderLibJ PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-O3;-DNDEBUG;-ffp-contract=off>
            $<$<COMPILE_LANGUAGE:CXX>:-O3;-DNDEBUG;-ffp-contract=off;-include;${PROJECT_ROOT}/cxx_compat/pre_cubiomes.h>
        )
    endif()
endif()

if(MSVC)
    target_compile_options(DripstoneCaveFinderLibJ PRIVATE /O2 /DNDEBUG)
else()
    target_compile_options(DripstoneCaveFinderLibJ PRIVATE -O3 -DNDEBUG -ffp-contract=off)
endif()

if(MINGW)
    target_link_options(DripstoneCaveFinderLibJ PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -pthread
    )
endif()

set_target_properties(DripstoneCaveFinderLibJ PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "DripstoneCaveFinderLibJ"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
)

if(NOT MSVC)
    if(NOT CMAKE_STRIP)
        find_program(CMAKE_STRIP strip)
    endif()
    if(CMAKE_STRIP)
        add_custom_command(TARGET DripstoneCaveFinderLibJ POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:DripstoneCaveFinderLibJ>
        )
    endif()
endif()
