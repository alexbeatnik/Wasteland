cmake_minimum_required(VERSION 3.16)
project(Wasteland C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# llama.cpp submodule
# ---------------------------------------------------------------------------
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

add_subdirectory(vendor/llama.cpp)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(Wasteland ${SOURCES})

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(Wasteland PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/ggml/include
)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(Wasteland PRIVATE _GNU_SOURCE)
endif()

if(NOT MSVC)
    target_compile_options(Wasteland PRIVATE
        -O3
        -march=native
        -Wall
        -Wextra
    )
else()
    target_compile_options(Wasteland PRIVATE
        /O2
        /W4
    )
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# seccomp is Linux-only
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(SECCOMP_LIBRARY seccomp)
endif()

# SDL2 target compatibility.
#
# Homebrew on macOS ships SDL2Config.cmake but historically some bottles have
# shipped a target without INTERFACE_INCLUDE_DIRECTORIES populated, which makes
# `#include <SDL2/SDL.h>` fail with "file not found" even though SDL2 is
# installed. Detect that case and fall back to pkg-config (which always works
# on macOS / Linux when sdl2.pc is present) so the include path is restored.
if(TARGET SDL2::SDL2)
    get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _sdl2_inc OR _sdl2_inc STREQUAL "_sdl2_inc-NOTFOUND")
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2_PC QUIET sdl2)
            if(SDL2_PC_FOUND)
                set_target_properties(SDL2::SDL2 PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_PC_INCLUDE_DIRS}")
            endif()
        endif()
    endif()
    set(SDL2_LINK_TARGET SDL2::SDL2)
else()
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES      "${SDL2_LIBRARIES}")
    set(SDL2_LINK_TARGET SDL2::SDL2)
endif()

# CURL target compatibility (older FindCURL modules)
if(TARGET CURL::libcurl)
    set(CURL_LINK_TARGET CURL::libcurl)
else()
    set(CURL_LINK_TARGET ${CURL_LIBRARIES})
    target_include_directories(Wasteland PRIVATE ${CURL_INCLUDE_DIRS})
endif()

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(Wasteland PRIVATE
    llama
    OpenGL::GL
    ${SDL2_LINK_TARGET}
    ${CURL_LINK_TARGET}
    Threads::Threads
)

if(SECCOMP_LIBRARY)
    target_link_libraries(Wasteland PRIVATE ${SECCOMP_LIBRARY})
    target_compile_definitions(Wasteland PRIVATE WASTELAND_HAS_SECCOMP=1)
endif()

# Platform-specific libs
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Wasteland PRIVATE m)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # macOS frameworks are handled by find_package
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(Wasteland PRIVATE
        ws2_32
        winmm
    )
endif()
