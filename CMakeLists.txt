cmake_minimum_required(VERSION 3.20)
project(NightSlashersNative VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(NSL_BUILD_HEADLESS "Build the deterministic headless frame runner" ON)
set(NSL_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated" CACHE PATH
    "Directory holding the C produced by tools/recompile.py")

file(GLOB NSL_GENERATED_SOURCES CONFIGURE_DEPENDS
     "${NSL_GENERATED_DIR}/program_*.c")

if(NOT NSL_GENERATED_SOURCES)
    message(FATAL_ERROR
        "No translated program in ${NSL_GENERATED_DIR}.\n"
        "Run: python tools/recompile.py <path-to-nslasher.zip>")
endif()

add_library(nsl_runtime STATIC
    runtime/src/cpu.c
    runtime/src/machine.c
    runtime/src/run.c
    runtime/src/video.c
    ${NSL_GENERATED_SOURCES}
)

target_include_directories(nsl_runtime
    PUBLIC runtime/include
    PRIVATE ${NSL_GENERATED_DIR}
)

if(MSVC)
    target_compile_options(nsl_runtime PRIVATE /W3)
else()
    target_compile_options(nsl_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

set(NSL_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")

if(NSL_BUILD_HEADLESS)
    add_executable(nsl_headless ports/headless/main.c)
    target_link_libraries(nsl_headless PRIVATE nsl_runtime)
    set_target_properties(nsl_headless PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${NSL_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${NSL_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${NSL_OUTPUT_DIR}")
endif()
