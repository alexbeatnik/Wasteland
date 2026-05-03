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

# Detect universal-binary builds (macOS only — CMAKE_OSX_ARCHITECTURES has
# more than one entry). When fat-building for arm64+x86_64 we cannot use
# -march=native or any per-arch ISA flag, since each arch slice needs its
# own codegen — ggml otherwise hard-fails the configure step.
list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_arch_count)
if(_osx_arch_count GREATER 1)
    set(WASTELAND_UNIVERSAL_BUILD ON)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
else()
    set(WASTELAND_UNIVERSAL_BUILD OFF)
endif()

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
    target_compile_options(Wasteland PRIVATE -O3 -Wall -Wextra)
    # `-march=native` produces an ISA bound to the current build host. That
    # blows up universal macOS builds (each arch slice needs its own ISA flag)
    # and silently degrades portability of cross-distro binaries on Linux.
    # Only keep it for single-arch dev builds where the user is the consumer.
    if(NOT WASTELAND_UNIVERSAL_BUILD)
        target_compile_options(Wasteland PRIVATE -march=native)
    endif()
else()
    target_compile_options(Wasteland PRIVATE
        /O2
        /W4
    )
    # nuklear.h is a single-header library compiled inside nuklear_impl.c.
    # MSVC C4701 ("potentially uninitialized variable") fires inside nuklear.h's
    # own code at /W4 level and is a known false-positive in that codebase.
    # Suppress it only for that translation unit to keep our own code clean.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/nuklear_impl.c"
        PROPERTIES COMPILE_OPTIONS "/wd4701"
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
    # SDL2 on Windows redefines main → SDL_main via a header macro; the real
    # WinMain entry point that translates back lives in SDL2main.lib. Link it
    # or the linker fails with "unresolved external symbol main".
    if(TARGET SDL2::SDL2main)
        target_link_libraries(Wasteland PRIVATE SDL2::SDL2main)
    endif()

    # MSVC has no <pthread.h>; vcpkg's `pthreads` package (pthreads4w) supplies
    # the POSIX threading API. find_package(Threads) doesn't pick it up by
    # itself on MSVC, so wire it explicitly via the vcpkg-provided config.
    find_package(PThreads4W CONFIG QUIET)
    if(TARGET PThreads4W::PThreads4W)
        target_link_libraries(Wasteland PRIVATE PThreads4W::PThreads4W)
    else()
        # Fallback: vcpkg also leaves pthreadVC3.lib in the lib search path.
        target_link_libraries(Wasteland PRIVATE pthreadVC3)
    endif()
    # When linking pthreads4w statically (x64-windows-static triplet),
    # PTW32_STATIC_LIB must be defined or its headers mark every symbol as
    # __declspec(dllimport) and the linker re-fails on the same names.
    target_compile_definitions(Wasteland PRIVATE
        PTW32_STATIC_LIB
        HAVE_STRUCT_TIMESPEC)

    target_link_libraries(Wasteland PRIVATE
        ws2_32
        winmm
        version    # SDL2 static needs GetFileVersionInfo
        imm32      # SDL2 IME support
        setupapi   # SDL2 device enumeration
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
    set(CPACK_PACKAGE_VERSION         "0.2.0")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Offline-first amber-CRT LLM chat client built on llama.cpp")
    set(CPACK_PACKAGE_CONTACT         "Wasteland Maintainers")
    set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/alexbeatnik/Wasteland")

    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Wasteland Maintainers <noreply@example.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")

    # Map CMake's CMAKE_SYSTEM_PROCESSOR to the Debian architecture name so
    # the same CMakeLists.txt produces the correct .deb on both amd64 and arm64
    # CI runners without any manual override.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_deb_arch "amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_deb_arch "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7l|armhf)$")
        set(_deb_arch "armhf")
    else()
        set(_deb_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_deb_arch}")

    # Let dpkg-shlibdeps figure out concrete versioned deps from the linked
    # SOs (libSDL2, libcurl, libseccomp, libgomp, libstdc++, libgcc_s, libc).
    # That avoids hard-coding distro-specific versions like libsdl2-2.0-0.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)
    set(CPACK_PACKAGE_FILE_NAME
        "wasteland_${CPACK_PACKAGE_VERSION}_${_deb_arch}")

    # Only ship our Runtime component; llama.cpp's unconditional install rules
    # land in the default 'Unspecified' component which CPack drops in
    # component-install mode. Override the per-component file naming so we
    # still emit a single `wasteland_<version>_<arch>.deb` instead of the
    # default `<base>-Runtime.deb`.
    set(CPACK_COMPONENTS_ALL Runtime)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "wasteland")
    set(CPACK_DEBIAN_RUNTIME_FILE_NAME
        "wasteland_${CPACK_PACKAGE_VERSION}_${_deb_arch}.deb")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)

    include(CPack)
endif()
