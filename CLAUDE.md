# CLAUDE.md — Wasteland v0.1

## Agent Context

This file provides context for AI coding assistants working on the Wasteland project.

## Architecture

### Thread Model

- **Main Thread:** SDL2 event loop + Nuklear rendering at 60 FPS. Drains the inference output buffer each frame and appends new bytes to `chat_history` (under `chat_mutex`).
- **Inference Worker Thread:** Long-lived. Blocks on `pthread_cond_wait` for prompts. For each prompt:
  1. Wipes the KV cache (`llama_memory_clear(llama_get_memory(ctx), true)`) so each turn is independent.
  2. Runs the model's chat template via `llama_model_chat_template` + `llama_chat_apply_template` (with `add_ass=true`).
  3. Tokenises with `parse_special=true` so `<|im_start|>` / `<|im_end|>` are single tokens.
  4. Decodes the prompt as one batch, then samples greedily token-by-token, polling `ictx->cancel_generation` every step.
  5. Streams each piece through `emit_filtered_piece()` which strips `<think>` / `</think>` literals (with a 7-byte carry buffer to handle tags split across token boundaries).
- **Async Load Thread:** Joinable, one-shot per click. Spawned by `inference_load_model_async()`. Calls the synchronous `inference_load_model()` (which can block for seconds on multi-GB GGUFs), then publishes a result via `volatile int load_result`. The UI polls `inference_is_loading()` each frame and consumes the result with `inference_take_load_result()`.
- **Download Thread:** Detached pthread spawned per click. Runs libcurl. Writes `download_progress` / `download_active` / `download_complete_flag` for the main thread to observe.

### State Sharing

All mutable cross-thread state is in `app_state_t` (defined in `ui.h`):

- `chat_mutex` protects `chat_history`.
- `inference_ctx_t` contains internal mutexes for prompt / output / model access.
- `download_progress` / `download_active` / `download_cancel` / `download_complete_flag` / `download_success` are **`volatile int`** one-way flags shared between the detached download pthread and the main UI thread. The UI claims `download_active = 1` *before* `pthread_create` — if we deferred to `network_download_model` setting it, a fast double-click would slip through and spawn two concurrent downloaders writing the same file.
- `loading_model_index` (UI-side) tracks which row is mid-load so the LOAD/DELETE buttons can be gated.
- `chat_scroll_x`, `chat_scroll_y`, `chat_last_len` drive the auto-scroll-to-bottom behaviour for the chat group.
- `system_prompt` stores user instructions and persists to `system_prompt.txt`.
- `chats` (2D array sized `WASTELAND_MAX_CHATS × WASTELAND_CHAT_NAME_LEN`), `chat_count`, `selected_chat` manage the multi-session functionality, saving state to `chats/*.txt`. Files are written with a 4-byte `WSTL` magic followed by RC4-obfuscated body (cosmetic only — the key lives in the binary; this just hides chat text from a casual file-manager preview). Files lacking the magic are read back as legacy plaintext.
- `left_panel_collapsed` controls the UI layout state.
- `scan_local_models()` / `scan_local_chats()` `qsort` their output lexicographically — `readdir` order is filesystem-defined and unstable, so the UI listing would otherwise jump around between launches.

### Inference Public API (`inference.h`)

```c
inference_ctx_t* inference_init(void);
void             inference_shutdown(inference_ctx_t*);

int  inference_load_model(inference_ctx_t*, const char *path);          /* sync */
void inference_unload_model(inference_ctx_t*);
int  inference_is_model_loaded(inference_ctx_t*);

int  inference_load_model_async(inference_ctx_t*, const char *path);    /* spawns load thread */
int  inference_is_loading(inference_ctx_t*);
int  inference_take_load_result(inference_ctx_t*);                      /* 1 ok / -1 fail / 0 none */

void   inference_submit_prompt(inference_ctx_t*, const char *sys_prompt, const char *prompt);
size_t inference_read_output(inference_ctx_t*, char *buf, size_t size);
int    inference_is_generating(inference_ctx_t*);
void   inference_cancel_generation(inference_ctx_t*);
void   inference_request_stop(inference_ctx_t*);  /* shutdown signal */

void* inference_worker_thread(void *arg);
```

The worker appends a trailing `\n` to the output buffer before clearing `generating`, so the next "> prompt" line in the chat history is never glued onto the last assistant token.

### Shutdown Protocol

1. `state.running = 0` (set on `SDL_QUIT`).
2. Persist the active chat with `save_chat_history()`.
3. `SDL_HideWindow(win)` — user sees instant close.
4. `inference_request_stop(ictx)` — sets `ictx->running = 0`, raises `cancel_generation`, broadcasts on `prompt_cond`. Do **not** wake the worker by submitting an empty prompt: with a model loaded the chat template would still produce non-empty tokens and the worker would burn a full generation cycle on the way out.
5. `platform_thread_join_timeout(worker_thread, 1500)`:
   - **Linux:** `pthread_timedjoin_np`.
   - **macOS / Windows:** polls `pthread_kill(thread, 0)` with 100 ms sleeps.
6. **If join succeeds:** call `inference_shutdown()` (which also joins any in-flight load thread), tear down SDL/GL, return from `main`.
7. **If join times out:** print a message and call `_Exit(0)` immediately. The OS reclaims the mmap'd model and kills the threads. Do **not** call `pthread_cancel` — llama.cpp contains C++ code (allocators, mutexes) that is not async-cancel-safe and will SIGABRT. `_Exit` is process-level termination, not thread cancellation, so no per-thread cleanup runs and no SIGABRT occurs.

## API Conventions

- llama.cpp uses **current API** (not deprecated aliases):
  - `llama_model_load_from_file()` (not `llama_load_model_from_file`)
  - `llama_init_from_model()` (not `llama_new_context_with_model`)
  - `llama_vocab_is_eog()` (not `llama_token_is_eog`)
  - `llama_batch_get_one(tokens, n_tokens)` (auto position tracking)
  - `llama_memory_clear(llama_get_memory(ctx), true)` to wipe the KV cache
  - `llama_model_chat_template(model, NULL)` + `llama_chat_apply_template(...)` for prompt formatting
- Nuklear uses feature macros before every include:
  - `NK_INCLUDE_FIXED_TYPES`, `NK_INCLUDE_DEFAULT_ALLOCATOR`, `NK_INCLUDE_VERTEX_BUFFER_OUTPUT`, `NK_INCLUDE_FONT_BAKING`, `NK_INCLUDE_DEFAULT_FONT`, `NK_INCLUDE_STANDARD_IO`, `NK_INCLUDE_STANDARD_VARARGS`
- Chat history is rendered as a scrolled group (`nk_group_scrolled_offset_begin`) with per-line `nk_label_colored_wrap`. Row height per logical chat line is computed from `count_wrap_lines()` (binary search via the active font's `width()` callback). Auto-scroll to bottom is achieved by setting `chat_scroll_y` to a large sentinel whenever `chat_history` grows; Nuklear clamps it to the actual max.

## `<think>` Tag Stripping & Rendering

`emit_filtered_piece()` in `inference.c` transforms the literal byte sequences `<think>` and `</think>` from the streaming output into `-- THINK --` and `-- END THINK --` markers. Implementation notes:

- `tag_carry` (7 bytes, `TAG_CARRY_MAX`) is the longest possible partial-tag suffix kept between calls. `</think>` is 8 bytes, so we reserve up to 7 trailing bytes that might be the start of the next tag.
- Carry is reset (`tag_carry_len = 0`) at the start of each prompt and flushed verbatim at end of generation via `emit_filtered_flush()`.
- The UI parser in `ui.c` looks for these `-- THINK --` markers to render reasoning text in a dimmer colour (`amber_dim`) and to explicitly exclude it from the `◈` copy-to-clipboard functionality.

## Cross-Platform Notes

- `platform_mkdir()` — `_mkdir` on Windows, `mkdir` on POSIX.
- `platform_sleep_ms()` — `Sleep()` on Windows, `usleep()` on POSIX.
- `platform_thread_join_timeout()` — `pthread_timedjoin_np` on Linux, polled `pthread_kill` elsewhere.
- `scan_local_models()` — `FindFirstFileA` on Windows, `dirent` on POSIX.
- `lockdown_network()` — Linux seccomp only; no-op on macOS / Windows.
- Compiler flags:
  - GCC/Clang: `-O3 -march=native -Wall -Wextra`
  - MSVC: `/O2 /W4`

## Hub Models & URL Rewrite

- `hub_models[]` in `ui.c` is a fixed-size array (`WASTELAND_MAX_HUB_MODELS = 4`) of HF repo IDs. Entries must be **real public GGUF repos** — the downloader resolves them through `hf_find_first_gguf()` against the live HF API, so a fictional ID fails with HTTP 404 at click time. Current set: `Qwen/Qwen2.5-0.5B-Instruct-GGUF`, `ggml-org/gemma-3-1b-it-GGUF`, `Qwen/Qwen2.5-1.5B-Instruct-GGUF`, `ggml-org/SmolLM2-1.7B-Instruct-GGUF`.
- Custom URLs of the form `…/blob/main/<file>` are rewritten to `…/resolve/main/<file>` because HF only serves raw bytes from the latter. The rewrite is `+3` bytes long, so `network_download_model` must `memmove` the tail right by 3 *before* the `memcpy` overwrite — doing it in the other order corrupts the first 3 bytes of the filename.

## seccomp Filter Scope

The Linux lockdown is **deliberately narrow**: only `socket(AF_INET, …)`, `socket(AF_INET6, …)`, and `socket(AF_PACKET, …)` are killed. We do **not** filter `connect`, `sendmsg`, `recvmsg`, `setsockopt`, etc., because:

1. seccomp cannot dereference user-space pointers, so a `sockaddr` argument can't be inspected — a blanket kill would block the existing X11 / Wayland Unix-domain socket the GUI uses every frame and SIGSYS the process.
2. Gating `socket()` is sufficient: the process can no longer obtain a new IP fd to call `connect`/`sendto` on, so all outbound networking is foreclosed at the source.
3. Already-open file descriptors (X11, Wayland, stdio, the model file) keep working unchanged.

## Build Notes

- `nuklear_impl.c` is the **single compilation unit** for Nuklear implementation.
- `nuklear_sdl_gl2.h` contains the SDL2 / OpenGL2 backend (both decls + impl, guarded by `NK_SDL_GL2_IMPLEMENTATION`).
- llama.cpp lives in `third_party/llama.cpp/` (with a `vendor/llama.cpp` symlink that the CMake build references) and is added as a CMake subdirectory with `LLAMA_BUILD_EXAMPLES=OFF`.
- `seccomp` is optional (Linux only); CMake skips it gracefully on other platforms.
- The application does **not** auto-load any model on boot — the user must click `[ LOAD ]`. Lockdown only triggers after a successful manual load.

## Style

- Pure C (C11), no C++ in application code.
- `snprintf()` for all string building.
- `pthread_mutex_lock/unlock` pairs always kept tight and symmetric.
- No globals except backend state in `nuklear_sdl_gl2.h`.
- Platform-specific code guarded by `#ifdef _WIN32`, `#ifdef __linux__`, etc.
- `volatile int` for cross-thread one-way flags (`running`, `cancel_generation`, `loading`, `load_result`); mutexes for everything else.
