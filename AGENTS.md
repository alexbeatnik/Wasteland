# AGENTS.md — Wasteland v0.1

## Agent Instructions

This file contains conventions and preferences for AI agents working on Wasteland.

## Build System

- **CMake** is the only supported build system
- Run `./build.sh` to build; it auto-detects local venv cmake
- Never modify generated files in `build/`

## Code Conventions

1. **Language:** Pure C (C11 standard). No C++ in `src/*.c` files.
2. **Includes:** Order matters — SDL2/OpenGL headers before Nuklear macros before `nuklear.h`
3. **Thread Safety:** Every mutable shared field must have an explicit mutex. Prefer fine-grained locks over coarse ones.
4. **String Buffers:** Always `sizeof(buf) - 1` for `strncpy`. Prefer `snprintf` for formatted output.
5. **Error Handling:** Print to `stderr` with `[module]` prefix, then return error code. Never silently ignore failures.

## Security Rules

- `seccomp` filter must use `SCMP_ACT_KILL` (not `ERRNO`) for network syscalls
- `lockdown_network()` must be called **immediately** after model load succeeds
- Download code is isolated in `network.c` only

## UI Rules

- Amber monochrome palette only: `#FFB000` on dark charcoal/black
- Buttons use square brackets: `[ DOWNLOAD ]`, `[ REFRESH ]`, `[ DELETE ]`
- CRT terminal aesthetic — no rounded corners, no gradients
- All UI text must be visible against the dark background

## Testing Protocol

Before declaring a task complete:
1. Build succeeds: `./build.sh`
2. No new compiler warnings
3. Binary starts without segfault
4. Window closes cleanly (no SIGABRT / hang)

## Version

Current version: **0.1**
