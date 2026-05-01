# CLAUDE.md — Wasteland v0.1

## Agent Context

This file provides context for AI coding assistants working on the Wasteland project.

## Architecture

### Thread Model
- **Main Thread:** SDL2 event loop + Nuklear rendering at 60 FPS. Reads from inference output buffer.
- **Worker Thread:** Blocks on `pthread_cond_wait` for prompts. Calls `llama_decode()` and generates tokens one-by-one into a mutex-protected ring buffer.
- **Download Thread:** Temporary detached pthread. Runs libcurl to fetch `.gguf` files.

### State Sharing
All mutable cross-thread state is in `app_state_t` (defined in `ui.h`):
- `chat_mutex` protects `chat_history`
- `inference_ctx_t` contains internal mutexes for prompt/output/model access
- `download_active` / `download_progress` / `download_cancel` are written by download thread, read by main

### Shutdown Protocol
1. `state.running = 0`
2. Submit empty prompt to wake worker from cond-wait
3. `platform_thread_join_timeout()` with 10s timeout:
   - **Linux:** uses `pthread_timedjoin_np`
   - **macOS / Windows:** polls `pthread_kill(thread, 0)` with 100ms sleeps
4. If timeout: `pthread_detach()` and exit cleanly (do **NOT** `pthread_cancel` — llama.cpp C++ code is not async-cancel-safe and will SIGABRT)

## API Conventions

- llama.cpp uses **current API** (not deprecated aliases):
  - `llama_model_load_from_file()` (not `llama_load_model_from_file`)
  - `llama_init_from_model()` (not `llama_new_context_with_model`)
  - `llama_vocab_is_eog()` (not `llama_token_is_eog`)
  - `llama_batch_get_one(tokens, n_tokens)` (auto position tracking)

- Nuklear uses feature macros before every include:
  - `NK_INCLUDE_FIXED_TYPES`, `NK_INCLUDE_DEFAULT_ALLOCATOR`, `NK_INCLUDE_VERTEX_BUFFER_OUTPUT`

## Cross-Platform Notes

- `platform_mkdir()` — `_mkdir` on Windows, `mkdir` on POSIX
- `platform_sleep_ms()` — `Sleep()` on Windows, `usleep()` on POSIX
- `scan_local_models()` — `FindFirstFileA` on Windows, `dirent` on POSIX
- `lockdown_network()` — seccomp on Linux only, no-op elsewhere
- Compiler flags:
  - GCC/Clang: `-O3 -march=native -Wall -Wextra`
  - MSVC: `/O2 /W4`

## Build Notes

- `nuklear_impl.c` is the **single compilation unit** for Nuklear implementation
- `nuklear_sdl_gl2.h` contains the SDL2/OpenGL2 backend (both decls + impl guarded by `NK_SDL_GL2_IMPLEMENTATION`)
- llama.cpp is added as a CMake subdirectory with `LLAMA_BUILD_EXAMPLES=OFF`
- `seccomp` is optional (Linux only); CMake skips it gracefully on other platforms

## Style

- Pure C (C11), no C++ in application code
- `snprintf()` for all string building
- `pthread_mutex_lock/unlock` pairs always kept tight and symmetric
- No globals except backend state in `nuklear_sdl_gl2.h`
- Platform-specific code guarded by `#ifdef _WIN32`, `#ifdef __linux__`, etc.
