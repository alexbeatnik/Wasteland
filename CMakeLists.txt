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
target_compile_definitions(Wasteland PRIVATE _GNU_SOURCE)

target_compile_options(Wasteland PRIVATE
    -O0
    -g
    -march=native
    -Wall
    -Wextra
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_library(SECCOMP_LIBRARY seccomp REQUIRED)

# SDL2 target compatibility (older FindSDL2 modules)
if(TARGET SDL2::SDL2)
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
    ${SECCOMP_LIBRARY}
    Threads::Threads
    m
)
