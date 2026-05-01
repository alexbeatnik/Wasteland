# Wasteland Terminal v0.1

A highly secure, offline-first LLM chat application with a retro-futuristic TUI-like graphical interface.

## Overview

Wasteland is a local LLM inference client built in pure C with a Pip-Boy inspired amber-on-black CRT terminal aesthetic. It runs entirely offline after model download and uses Linux seccomp to physically block all network syscalls once a model is loaded.

## Features

- **Offline-first** — All inference happens locally via llama.cpp
- **Security** — Linux seccomp network lockdown after model load
- **Retro UI** — Amber (#FFB000) monochrome CRT terminal aesthetic via Nuklear
- **Threaded** — Non-blocking UI at 60 FPS with background inference worker
- **Model Management** — Download from HuggingFace, load local .gguf files, delete models
- **Download Progress** — Real-time progress bar with cancel support
- **File Sizes** — Model sizes displayed in human-readable format (GB/MB/KB)

## Tech Stack

- **Language:** Pure C (C11)
- **Inference:** llama.cpp (C API)
- **GUI:** Nuklear (immediate-mode) + SDL2 + OpenGL 2.1
- **Networking:** libcurl (isolated, seccomp-blocked after lockdown)
- **Security:** Linux seccomp (SIGKILL on socket syscalls)
- **Build:** CMake

## Building

```bash
./build.sh
```

Or manually:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Running

```bash
cd build
./Wasteland
```

Place `.gguf` model files in the `models/` directory, or download them via the built-in HuggingFace browser.

## Project Structure

```
Wasteland/
├── CMakeLists.txt          # Build configuration
├── build.sh                # One-shot build script
├── src/
│   ├── main.c              # Entry point, SDL loop, thread spawn
│   ├── ui.c / ui.h         # Nuklear layout & amber theme
│   ├── inference.c / .h    # llama.cpp wrapper & worker thread
│   ├── network.c / .h      # libcurl downloader & seccomp lockdown
│   ├── nuklear_impl.c      # Nuklear + SDL/GL2 backend impl
│   └── nuklear_sdl_gl2.h   # Nuklear SDL2/OpenGL2 backend decls
├── include/                # nuklear.h
├── vendor/
│   └── llama.cpp/          # Git submodule
└── models/                 # Local .gguf storage
```

## Security Model

1. App boots in offline state
2. Network is available only for downloading models
3. Once any model is loaded into RAM, `lockdown_network()` installs a seccomp filter
4. Any subsequent `socket`, `connect`, `sendto`, `recvfrom`, etc. results in immediate `SIGKILL` by the kernel

## License

See LICENSE file.
