# AGENTS.md — Wasteland v0.1

## Agent Instructions

This file contains conventions and preferences for AI agents working on Wasteland.

## Build System

- **CMake** is the only supported build system
- Run `./build.sh` to build on Linux; it auto-detects local venv cmake
- Never modify generated files in `build/`
- Cross-platform: Linux, macOS, Windows (MinGW / MSVC)

## Code Conventions

1. **Language:** Pure C (C11 standard). No C++ in `src/*.c` files.
2. **Includes:** Order matters — SDL2/OpenGL headers before Nuklear macros before `nuklear.h`
3. **Thread Safety:** Every mutable shared field must have an explicit mutex. Prefer fine-grained locks over coarse ones.
4. **String Buffers:** Always `sizeof(buf) - 1` for `strncpy`. Prefer `snprintf` for formatted output.
5. **Error Handling:** Print to `stderr` with `[module]` prefix, then return error code. Never silently ignore failures.
6. **Cross-Platform:** Wrap platform-specific code in `#ifdef _WIN32`, `#ifdef __linux__`, `#ifdef __APPLE__`.

## Security Rules

- `seccomp` filter (Linux only) must use `SCMP_ACT_KILL` (not `ERRNO`) for network syscalls
- `lockdown_network()` must be called **immediately** after model load succeeds
- Download code is isolated in `network.c` only
- On non-Linux platforms, `lockdown_network()` is a no-op — do not add fake implementations

## UI Rules

- Amber monochrome palette only: `#FFB000` on dark charcoal/black
- Buttons use square brackets: `[ DOWNLOAD ]`, `[ REFRESH ]`, `[ DELETE ]`, `[ CANCEL ]`
- CRT terminal aesthetic — no rounded corners, no gradients
- All UI text must be visible against the dark background
- Show file sizes in human-readable format next to model names

## Testing Protocol

Before declaring a task complete:
1. Build succeeds: `./build.sh`
2. No new compiler warnings
3. Binary starts without segfault
4. Window closes cleanly (no SIGABRT / hang)
5. Cross-platform changes must not break Linux build

## Version

Current version: **0.1**
