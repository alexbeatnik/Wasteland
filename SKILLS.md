# SKILLS.md — Wasteland v0.1

## Available Skills

This project does not use a formal skill system. The following domains are relevant for contributors and AI agents.

### Systems Programming

- POSIX / Windows threads (`pthreads`, `CreateThread`).
- Mutexes, condition variables, thread-safe ring buffers.
- One-way `volatile int` flags for cross-thread cancellation (`running`, `cancel_generation`, `loading`).
- Cross-platform file I/O (`dirent.h` vs `FindFirstFileA`).
- Signal-safe and **non-async-cancel-safe** thread design — llama.cpp must never be `pthread_cancel`'d.
- Process-level fast-exit (`_Exit`) vs graceful shutdown trade-offs.

### Asynchronous UI Patterns

- Off-loading multi-second work (model load, libcurl download) onto background threads with a published-result polling pattern (`is_loading()` / `take_load_result()`).
- Hiding latency: `SDL_HideWindow` to make the close click feel instant while threads finish in the background.
- Gating UI buttons on cross-thread state (`load_busy`, `gen_busy`) so the user can't trigger conflicting operations.

### Graphics & UI

- Nuklear immediate-mode GUI: layouts, groups, scrolled groups (`nk_group_scrolled_offset_begin`).
- Custom auto-scroll-to-bottom via the `nk_uint` scroll-offset API (Nuklear clamps oversized values to max).
- Collapsible UI panels dynamically adjusting layout widths.
- Word wrap with measured row heights using the active font's `width()` callback.
- Custom rendering of TTF fonts (`DejaVuSansMono`) mapped with specific Unicode ranges (e.g. Cyrillic `0x0400-0x04FF`).
- SDL2 windowing and event handling.
- OpenGL 2.1 fixed-function pipeline.
- Font atlas baking and vertex buffer rendering.

### Machine Learning Inference

- llama.cpp C API (current, not deprecated aliases).
- GGUF model format and `llama_model_load_from_file` / `llama_init_from_model`.
- Tokenisation (`llama_tokenize` with `parse_special=true`, `llama_token_to_piece`).
- Chat template application: `llama_model_chat_template` + `llama_chat_apply_template` with `add_ass=true`.
- KV-cache lifecycle: `llama_get_memory(ctx)` + `llama_memory_clear(mem, true)` for per-prompt resets.
- Auto-positioning batches via `llama_batch_get_one(tokens, n_tokens)`.
- Greedy sampling with `llama_sampler_chain` + `llama_sampler_init_greedy`.
- Detection of end-of-generation tokens via `llama_vocab_is_eog`.

### Stream Processing & Data Persistence

- Token-by-token streaming with carry buffers for partial-match safety.
- Intercepting reasoning markers (`<think>` / `</think>`) and rendering them differently (dimmed text).
- Custom UI string parsers to selectively skip reasoning blocks when copying chat text to the clipboard.
- Generating human-readable filenames (stripping filesystem-unsafe characters) from dynamic prompts.
- Persisting state across application runs using simple text files (e.g. `system_prompt.txt` and `chats/*.txt`).

### Network & Security

- libcurl easy API for `.gguf` downloads with progress callbacks.
- HuggingFace API discovery (`/api/models/.../tree/main`) to resolve a repo to a concrete file URL.
- Linux seccomp-bpf with **argument filtering** (`SCMP_A0(SCMP_CMP_EQ, AF_INET)`) — narrow rules that gate `socket()` creation by address family, leaving X11 / Wayland Unix-domain traffic untouched.
- Why pointer-deref filtering (sockaddr family on `connect`/`bind`) is impossible in seccomp and what to do instead (gate at `socket()`).
- Cross-platform security model: features degrade gracefully on macOS / Windows.

### Build Engineering

- CMake target configuration.
- Cross-platform `find_package` / `find_library` handling.
- Git submodule management for vendored llama.cpp.
- Platform-specific linking (SDL2, OpenGL, curl, seccomp on Linux; ws2_32, winmm on Windows).
- MinGW cross-compilation from Linux toolchain files.

## Version

Current version: **0.1**
