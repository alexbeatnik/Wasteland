# Wasteland Terminal v0.1

A highly secure, offline-first LLM chat application with a retro-futuristic TUI-like graphical interface.

## Overview

Wasteland is a local LLM inference client built in pure C with a Pip-Boy inspired amber-on-black CRT terminal aesthetic. It runs entirely offline after model download and uses Linux seccomp (where available) to physically block all network syscalls once a model is loaded.

## Features

- **Offline-first** — All inference happens locally via llama.cpp
- **Security** — Linux seccomp network lockdown after model load (no-op on macOS/Windows)
- **Retro UI** — Amber (`#FFB000`) monochrome CRT terminal aesthetic via Nuklear
- **Threaded** — Non-blocking UI at 60 FPS with background inference worker
- **Model Management** — Download from HuggingFace, load local `.gguf` files, delete models
- **Download Progress** — Real-time progress bar with filename, percent, and **cancel** support
- **File Sizes** — Model sizes displayed in human-readable format (GB/MB/KB)
- **Cross-Platform** — Linux, macOS, Windows (MinGW/MSVC)

## Tech Stack

- **Language:** Pure C (C11)
- **Inference:** llama.cpp (C API)
- **GUI:** Nuklear (immediate-mode) + SDL2 + OpenGL 2.1
- **Networking:** libcurl (isolated, blocked after lockdown)
- **Security:** Linux seccomp (`SCMP_ACT_KILL` on socket syscalls)
- **Build:** CMake 3.16+

## Building

### Linux

```bash
# Install deps (Debian/Ubuntu)
sudo apt install cmake libsdl2-dev libcurl4-openssl-dev libseccomp-dev

# Install deps (Arch)
sudo pacman -S cmake sdl2 curl libseccomp

# Build
./build.sh
```

### macOS

```bash
# Install deps (Homebrew)
brew install cmake sdl2 curl

# Build
git submodule update --init --recursive
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

### Windows (MSYS2 / MinGW)

```bash
# Install deps
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2 mingw-w64-x86_64-curl

# Build
git submodule update --init --recursive
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make -j$(nproc)
```

### Windows (MSVC)

```powershell
# Install deps via vcpkg
vcpkg install sdl2:x64-windows curl:x64-windows

# Build
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build . --config Release
```

## Cross-Compilation

### Windows exe from Linux

```bash
sudo apt install mingw-w64 cmake
mkdir build-win && cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake
cmake --build . -j$(nproc)
# Produces: Wasteland.exe
```

See `CROSS_COMPILE.md` for full details including MXE dependency setup.

### macOS dmg from Linux
**Not recommended.** Apple requires macOS SDK and code signing. Use GitHub Actions or build on a real Mac.

## Prebuilt Binaries

GitHub Actions automatically builds and releases for all platforms on every tag:

| Platform | Artifact |
|----------|----------|
| Linux | `Wasteland-linux.tar.gz` |
| macOS | `Wasteland.dmg` |
| Windows | `Wasteland-windows.zip` |

Push a tag to trigger a release:
```bash
git tag v0.1.0
git push origin v0.1.0
```

## Running

```bash
cd build
./Wasteland        # Linux / macOS
Wasteland.exe      # Windows
```

Place `.gguf` model files in the `models/` directory, or download them via the built-in HuggingFace panel.

## Project Structure

```
Wasteland/
├── CMakeLists.txt          # Cross-platform build configuration
├── build.sh                # One-shot build script (Linux auto-detect)
├── README.md               # This file
├── CLAUDE.md               # AI assistant context
├── AGENTS.md               # Agent conventions & style guide
├── SKILLS.md               # Domain skill reference
├── CROSS_COMPILE.md        # Cross-compilation guide
├── .gitignore              # Ignore build artifacts & models
├── cmake/
│   └── mingw-w64-x86_64.cmake  # MinGW toolchain for Windows builds
├── .github/
│   └── workflows/
│       └── build.yml       # CI/CD: builds Linux/macOS/Windows + releases
├── src/
│   ├── main.c              # Entry point, SDL loop, thread spawn
│   ├── ui.c / ui.h         # Nuklear layout, amber theme, model list
│   ├── inference.c / .h    # llama.cpp wrapper & worker thread
│   ├── network.c / .h      # libcurl downloader & seccomp lockdown
│   ├── nuklear_impl.c      # Nuklear + SDL/GL2 backend impl
│   └── nuklear_sdl_gl2.h   # Nuklear SDL2/OpenGL2 backend
├── include/                # nuklear.h
├── vendor/
│   └── llama.cpp/          # Git submodule
└── models/                 # Local .gguf storage (gitignored)
```

## UI Guide

### Left Panel
- **Hub Models** — 4 predefined HuggingFace repos with radio buttons
- **Custom ID or URL** — Enter any HF repo ID or full `/blob/` URL
- **Target** — Shows resolved download target before clicking `[ DOWNLOAD ]`
- **Progress** — Filename + percent during download, with `[ CANCEL ]` button
- **Local Vault** — List of `.gguf` files with size, `[ LOAD ]`, `[ DELETE ]`, `[ REFRESH ]`

### Right Panel
- **Chat History** — Terminal-style read-only log
- **Input** — `>` prompt with text field
- **Transmit** — Submit prompt to inference worker

## Security Model

1. App boots in offline state
2. Network is available only for downloading models
3. Once any model is loaded into RAM, `lockdown_network()` attempts to block networking:
   - **Linux:** installs seccomp-bpf filter with `SCMP_ACT_KILL` on socket syscalls
   - **macOS / Windows:** no-op (platform limitation), network remains available
4. On Linux, any subsequent `socket`, `connect`, `sendto`, `recvfrom`, etc. results in immediate `SIGKILL` by the kernel

## License

See LICENSE file.
