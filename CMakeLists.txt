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

# On Windows, embed src/wasteland.rc → Wasteland.exe carries the icon for
# Explorer / taskbar / Alt-Tab. MSVC and MinGW both accept .rc directly when
# compiled with the appropriate language enabled.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    enable_language(RC)
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/wasteland.rc")
endif()

# On macOS, wrap the binary in a .app bundle so Finder shows the icon and the
# app behaves like a normal Mac app instead of a CLI binary in /Applications.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(MACOSX_BUNDLE_ICON_FILE icon.icns)
    set(APP_ICON_MACOSX "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns")
    set_source_files_properties(${APP_ICON_MACOSX} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    add_executable(Wasteland MACOSX_BUNDLE ${SOURCES} ${APP_ICON_MACOSX})
    set_target_properties(Wasteland PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist")
else()
    add_executable(Wasteland ${SOURCES})
endif()

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

# ---------------------------------------------------------------------------
# Install / packaging — used by CPack to produce a .deb on Ubuntu CI.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
# Tag every install rule with COMPONENT Runtime so CPack can ship just our
# files and drop llama.cpp's unconditional install rules (dev headers, static
# libs, ggml-config.cmake, convert_hf_to_gguf.py) on the floor.
install(TARGETS Wasteland
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}     COMPONENT Runtime
        BUNDLE  DESTINATION /Applications              COMPONENT Runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Hicolor icon set + .desktop entry so the .deb integrates with the system
    # menu (GNOME / KDE / XFCE / etc.) and the dock shows the amber CRT logo.
    foreach(_size 16 32 48 64 128 256 512)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-${_size}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
                RENAME "wasteland.png"
                COMPONENT Runtime)
    endforeach()
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/wasteland.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            COMPONENT Runtime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps"
            RENAME "wasteland.png"
            COMPONENT Runtime)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_PACKAGE_NAME            "wasteland")
    set(CPACK_PACKAGE_VERSION         "0.1.0")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Offline-first amber-CRT LLM chat client built on llama.cpp")
    set(CPACK_PACKAGE_CONTACT         "Wasteland Maintainers")
    set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/alexbeatnik/Wasteland")

    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Wasteland Maintainers <noreply@example.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    # Let dpkg-shlibdeps figure out concrete versioned deps from the linked
    # SOs (libSDL2, libcurl, libseccomp, libgomp, libstdc++, libgcc_s, libc).
    # That avoids hard-coding distro-specific versions like libsdl2-2.0-0.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)
    set(CPACK_PACKAGE_FILE_NAME
        "wasteland_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")

    # Only ship our Runtime component; llama.cpp's unconditional install rules
    # land in the default 'Unspecified' component which CPack drops in
    # component-install mode. Override the per-component file naming so we
    # still emit a single `wasteland_<version>_amd64.deb` instead of the
    # default `<base>-Runtime.deb`.
    set(CPACK_COMPONENTS_ALL Runtime)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "wasteland")
    set(CPACK_DEBIAN_RUNTIME_FILE_NAME
        "wasteland_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)

    include(CPack)
endif()
