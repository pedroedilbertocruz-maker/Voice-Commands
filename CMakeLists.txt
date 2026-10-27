cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(voice_commands VERSION 0.2.0 LANGUAGES CXX)

option(VOICE_BUNDLE_MODELS "Download the English and Portuguese speech models into the mod" ON)
option(VOICE_BUILD_TESTS "Build the offline recognition test tool" OFF)

# The Dusklight version to build against: a release tag (e.g. "v2.0.3") or a commit SHA.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.3")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

include(FetchContent)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VoiceDeps.cmake")

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(voice_commands
        FEATURES game
        SOURCES src/mod.cpp src/commands.cpp src/voice_engine.cpp src/miniaudio_impl.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR "${VOICE_RES_DIR}"
        RUNTIME_LIBRARIES ${VOSK_RUNTIME_LIBRARIES}
)
target_include_directories(voice_commands PRIVATE "${miniaudio_SOURCE_DIR}")
target_compile_definitions(voice_commands PRIVATE
        MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH
        MA_NO_ENGINE)
if (NOT WIN32)
    target_link_libraries(voice_commands PRIVATE ${CMAKE_DL_LIBS} pthread m)
endif ()

# MSVC reports __cplusplus as 199711L unless told otherwise, and the game's dolphin/types.h then
# does `#define nullptr NULL`, breaking the C++ standard library. The full Dusklight build passes
# this flag, but the standalone mod SDK does not.
if (MSVC)
    target_compile_options(voice_commands PRIVATE /Zc:__cplusplus)
endif ()

if (VOICE_BUILD_TESTS)
    add_executable(voice_recognize tools/recognize.cpp src/commands.cpp)
    target_include_directories(voice_recognize PRIVATE src)
    if (NOT WIN32)
        target_link_libraries(voice_recognize PRIVATE ${CMAKE_DL_LIBS})
    endif ()
endif ()
