# Wasteland Terminal v0.1

A highly secure, offline-first LLM chat application with a retro-futuristic TUI-like graphical interface.

## Overview

Wasteland is a local LLM inference client built in pure C with a Pip-Boy inspired amber-on-black CRT terminal aesthetic. It runs entirely offline once a model is loaded, and uses Linux seccomp (where available) to physically prevent the process from opening any new IP socket for the rest of its lifetime.

## Features

- **Offline-first** — All inference happens locally via llama.cpp
- **Hard network lockdown** — Linux seccomp blocks creation of any new `AF_INET` / `AF_INET6` / `AF_PACKET` socket once a model is loaded (no-op on macOS/Windows)
- **Retro UI** — Amber (`#FFB000`) monochrome CRT terminal aesthetic via Nuklear
- **Threaded** — Non-blocking UI at 60 FPS with background inference and async model loading
- **Async model load** — UI stays responsive during multi-second GGUF mmap/decode
- **Model Management** — Download from HuggingFace, load / **unload** / delete local `.gguf` files
- **Stop generation** — `STOP` button cancels an in-flight response mid-token
- **Chat template** — Uses each model's built-in template (`llama_chat_apply_template`) so instruction-tuned models behave correctly
- **Reasoning-tag stripping** — `<think>…</think>` markers from Qwen-style models are dropped from the stream while the reasoning text itself is preserved
- **Auto-scroll + word wrap** — Chat pins to the bottom and wraps long lines to the panel width
- **Download Progress** — Real-time progress bar with filename, percent, and **cancel** support
- **File Sizes** — Model sizes displayed in human-readable format (GB/MB/KB)
- **Fast close** — Clicking X hides the window instantly; the process force-exits if a worker is still mid-decode
- **Cross-Platform** — Linux, macOS, Windows (MinGW/MSVC)

## Tech Stack

- **Language:** Pure C (C11)
- **Inference:** llama.cpp (C API, current — not deprecated aliases)
- **GUI:** Nuklear (immediate-mode) + SDL2 + OpenGL 2.1
- **Networking:** libcurl (download path only; isolated in `network.c`)
- **Security:** Linux seccomp-bpf — `SCMP_ACT_KILL_PROCESS` on `socket(AF_INET|AF_INET6|AF_PACKET, …)`
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

Place `.gguf` model files in the `models/` directory, or download them via the built-in HuggingFace panel. The application **does not** auto-load anything on boot — you pick the model explicitly so the network stays available for downloads until you commit.

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
│   ├── main.c              # Entry point, SDL loop, thread spawn, fast-shutdown
│   ├── ui.c / ui.h         # Nuklear layout, amber theme, scrollable chat with wrap
│   ├── inference.c / .h    # llama.cpp wrapper, async load, worker thread, <think> filter
│   ├── network.c / .h      # libcurl downloader & seccomp lockdown
│   ├── nuklear_impl.c      # Nuklear + SDL/GL2 backend impl
│   └── nuklear_sdl_gl2.h   # Nuklear SDL2/OpenGL2 backend
├── include/                # nuklear.h
├── third_party/
│   └── llama.cpp/          # Git submodule (vendored llama.cpp)
├── vendor/
│   └── llama.cpp -> ../third_party/llama.cpp  # symlink the CMake build uses
└── models/                 # Local .gguf storage (gitignored)
```

## UI Guide

### Left Panel

- **Hub Models** — 4 predefined HuggingFace repos with radio buttons
- **Custom ID or URL** — Enter any HF repo ID or full `/blob/` URL
- **Target** — Shows resolved download target before clicking `[ DOWNLOAD ]`
- **Progress** — Filename + percent during download, with `[ CANCEL ]` button
- **Local Vault** — List of `.gguf` files with size:
  - `[ LOAD: name | size ]` — start async load (UI stays responsive)
  - `[ LOADING: name | size ... ]` — in flight; other LOAD/DELETE buttons are disabled
  - `[ UNLOAD: name | size ]` — currently loaded model; click to free it
  - `[ DELETE ]` — remove the file from disk (disabled while a load is in flight or generation is running)
  - `[ REFRESH ]` — re-scan `models/`
- **Status footer** — "NET: LOCKDOWN ACTIVE" once a model is loaded; otherwise "NET: DISCONNECTED (READY)"

### Right Panel

- **Chat history** — Scrollable, word-wrapped, auto-pins to the bottom on new tokens. `<think>…</think>` markers from reasoning models are stripped automatically.
- **Input** — `>` prompt with text field
- **`TRANSMIT`** — submit the prompt (Enter also works)
- **`STOP`** — replaces TRANSMIT while the model is generating; cancels the current response

## Security Model

1. App boots in **offline-capable** state with the network reachable so the user can download models.
2. **Nothing is loaded automatically.** The user picks a model via the UI.
3. When the user clicks `[ LOAD ]` and the load succeeds, `lockdown_network()` runs:
   - **Linux:** installs a seccomp-bpf filter that kills the process if it ever calls `socket(AF_INET, …)`, `socket(AF_INET6, …)`, or `socket(AF_PACKET, …)`. The filter only gates **new** socket creation — already-open file descriptors (notably the X11 / Wayland Unix-domain socket the GUI uses every frame) keep working.
   - **macOS / Windows:** no-op (platform limitation), network remains available.
4. seccomp filters cannot be removed for the lifetime of the process. Unloading a model does not lift the lockdown — restart to download more models.
5. The download path lives entirely in `network.c` and is gated by `state->network_lockdown` in the UI, so the `[ DOWNLOAD ]` button hides the moment the lockdown is active.

## License

See LICENSE file.
