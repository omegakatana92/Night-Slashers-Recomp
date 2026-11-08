cmake_minimum_required(VERSION 3.20)
project(NightSlashersNative VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(NSL_BUILD_HEADLESS "Build the deterministic headless frame runner" ON)
option(NSL_BUILD_SDL2 "Build the SDL2 front end (window, audio, input, menus)" ON)
set(NSL_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated" CACHE PATH
    "Directory holding the C produced by tools/recompile.py")

file(GLOB NSL_GENERATED_SOURCES CONFIGURE_DEPENDS
     "${NSL_GENERATED_DIR}/program_*.c")

if(NOT NSL_GENERATED_SOURCES)
    message(FATAL_ERROR
        "No translated program in ${NSL_GENERATED_DIR}.\n"
        "Run: python tools/recompile.py <path-to-nslasher.zip>")
endif()

set(MINIZ_DIR "${CMAKE_SOURCE_DIR}/runtime/third_party/miniz")
add_library(nsl_miniz STATIC
    ${MINIZ_DIR}/miniz.c ${MINIZ_DIR}/miniz_tdef.c ${MINIZ_DIR}/miniz_tinfl.c ${MINIZ_DIR}/miniz_zip.c)
target_include_directories(nsl_miniz PUBLIC ${MINIZ_DIR})
target_compile_definitions(nsl_miniz PUBLIC MINIZ_NO_TIME PRIVATE _GNU_SOURCE _LARGEFILE64_SOURCE)

add_library(nsl_z80 STATIC runtime/third_party/z80/z80.c)
target_include_directories(nsl_z80 PUBLIC runtime/third_party/z80)
add_library(nsl_ymfm STATIC runtime/third_party/ymfm/ymfm_opm.cpp)
target_include_directories(nsl_ymfm PUBLIC runtime/third_party/ymfm)

add_library(nsl_runtime STATIC
    runtime/src/core.c
    runtime/src/interp.c
    runtime/src/cpu.c
    runtime/src/machine.c
    runtime/src/platform.c
    runtime/src/rom.c
    runtime/src/run.c
    runtime/src/sound.cpp
    runtime/src/state.c
    runtime/src/video.c
    ${NSL_GENERATED_SOURCES}
)
target_include_directories(nsl_runtime
    PUBLIC runtime/include
    PRIVATE runtime/src ${NSL_GENERATED_DIR}
)
target_link_libraries(nsl_runtime PUBLIC nsl_miniz nsl_z80 nsl_ymfm)

if(MSVC)
    target_compile_options(nsl_runtime PRIVATE /W3)
    target_compile_definitions(nsl_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(nsl_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
                           -Wno-unused-but-set-variable -Wno-unused-label)
    if(UNIX)
        target_link_libraries(nsl_runtime PUBLIC m)
    endif()
endif()

# AppImage build: look for the bundled SDL2 in ../lib. libstdc++/libgcc stay dynamic on
# purpose: a static libstdc++ from a new toolchain pulls in glibc 2.35-2.38 symbols,
# while the host's own libstdc++ (always present on SteamOS) keeps the glibc floor at 2.34.
option(NSL_PORTABLE_LINUX "Look for bundled libraries in ../lib (AppImage)" OFF)
if(NSL_PORTABLE_LINUX AND UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()

if(MINGW)
    # self-contained executables: no libgcc/libstdc++/winpthread DLLs to ship
    add_link_options(-static -static-libgcc -static-libstdc++)
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

if(NSL_BUILD_SDL2)
    find_package(SDL2 CONFIG QUIET)
    if(NOT SDL2_FOUND)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
        endif()
    endif()
    if(TARGET SDL2::SDL2 OR TARGET PkgConfig::SDL2)
        file(GLOB NSL_FRONTEND_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ports/sdl2/*.c")
        add_executable(nightslashers ${NSL_FRONTEND_SOURCES})
        target_include_directories(nightslashers PRIVATE ports/sdl2)
        target_link_libraries(nightslashers PRIVATE nsl_runtime)
        if(TARGET SDL2::SDL2)
            if(WIN32 AND TARGET SDL2::SDL2main)
                target_link_libraries(nightslashers PRIVATE SDL2::SDL2main)   # WinMain -> SDL_main
            endif()
            target_link_libraries(nightslashers PRIVATE SDL2::SDL2)
        else()
            target_link_libraries(nightslashers PRIVATE PkgConfig::SDL2)
        endif()
        if(WIN32)
            set_target_properties(nightslashers PROPERTIES WIN32_EXECUTABLE ON)
        endif()
        set_target_properties(nightslashers PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${NSL_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${NSL_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${NSL_OUTPUT_DIR}")
    else()
        message(STATUS "SDL2 not found: building only the headless runner")
    endif()
endif()
