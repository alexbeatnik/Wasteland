# CLAUDE.md — Wasteland v0.3

## Agent Context

This file provides context for AI coding assistants working on the Wasteland project.

## Architecture

### Thread Model

- **Main Thread:** SDL2 event loop + Nuklear rendering at 60 FPS. Drains the inference output buffer each frame and appends new bytes to `chat_history` (under `chat_mutex`).
- **Inference Worker Thread:** Long-lived. Blocks on `pthread_cond_wait` for prompts. For each prompt:
  1. Wipes the KV cache (`llama_memory_clear(llama_get_memory(ctx), true)`) — the entire formatted conversation is re-tokenised from scratch each turn.
  2. Reads tunables under `settings_mutex`: `pending_n_ctx` (used at model-load time) and `temperature` (used to build the sampler chain).
  3. Runs the model's chat template via `llama_model_chat_template` + `llama_chat_apply_template` (with `add_ass=true`).
  4. Tokenises with `parse_special=true` so `<|im_start|>` / `<|im_end|>` are single tokens.
  5. Decodes the prompt in `n_batch`-sized chunks (queried via `llama_n_batch(ctx)`), then samples token-by-token with the penalty/top_k/top_p/temp/dist chain, polling `ictx->cancel_generation` every step.
  6. Streams each piece through `emit_filtered_piece()` which converts `<think>` / `</think>` literals to `\n-- THINK --\n` / `\n-- END THINK --\n` markers (with a 7-byte carry buffer to handle tags split across token boundaries).
- **Async Load Thread:** Joinable, one-shot per click. Spawned by `inference_load_model_async()`. Calls the synchronous `inference_load_model()` (which can block for seconds on multi-GB GGUFs), reads `pending_n_ctx` from `settings_mutex` to set `cparams.n_ctx` and `cparams.n_batch`, then publishes a result via `volatile int load_result`. The UI polls `inference_is_loading()` each frame and consumes the result with `inference_take_load_result()`.
- **Download Thread:** Detached pthread spawned per click. Runs libcurl. Writes `download_progress` / `download_active` / `download_complete_flag` for the main thread to observe.

### State Sharing

All mutable cross-thread state is in `app_state_t` (defined in `ui.h`):

- `chat_mutex` protects `chat_history`.
- `inference_ctx_t` contains internal mutexes for prompt / output / model / agent / pending / history / settings access.
- `download_progress` / `download_active` / `download_cancel` / `download_complete_flag` / `download_success` are **`volatile int`** one-way flags. The UI claims `download_active = 1` *before* `pthread_create`.
- `loading_model_index` (UI-side) tracks which row is mid-load so the LOAD/DELETE buttons can be gated.
- `chat_scroll_x`, `chat_scroll_y`, `chat_last_len` drive auto-scroll-to-bottom.
- `system_prompt` stores user instructions and persists to `system_prompt.txt`.
- `chats` (2D array sized `WASTELAND_MAX_CHATS × WASTELAND_CHAT_NAME_LEN`), `chat_count`, `selected_chat` manage multi-session functionality, saving state to `chats/*.txt` (4-byte `WSTL` magic + RC4-obfuscated body; files without the magic are read as legacy plaintext).
- `left_panel_collapsed` controls UI layout state.
- `settings_n_ctx` / `settings_temperature` — UI-side copies of the tunables, pushed to `inference_set_n_ctx` / `inference_set_temperature` every frame and persisted to `wasteland.cfg`.
- `context_tokens` / `context_max` — updated by `ui_update_context_stats()` after every generation and after compact.
- `scan_local_models()` / `scan_local_chats()` `qsort` their output lexicographically.

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

void   inference_set_chat_history(inference_ctx_t*, const char *history);
int    inference_get_context_stats(inference_ctx_t*, const char *history,
                                   int *tokens_out, int *max_out);

/* Tunables — n_ctx read at model-load time; temperature at sampler-build time */
void   inference_set_n_ctx(inference_ctx_t*, int n_ctx);          /* 512–262144 */
void   inference_set_temperature(inference_ctx_t*, float temp);   /* 0.01–5.0  */
int    inference_get_n_ctx(inference_ctx_t*);
float  inference_get_temperature(inference_ctx_t*);

/* Agent mode */
void   inference_set_agent(inference_ctx_t*, int mode, const char *workspace);
int    inference_get_pending(inference_ctx_t*, const char **path_out,
                             const char **content_out, const char **search_out,
                             const char **replace_out);
void   inference_set_pending_approval(inference_ctx_t*, int decision);

void* inference_worker_thread(void *arg);
```

The worker appends a trailing `\n` to the output buffer before clearing `generating`, so the next `> prompt` line in the chat history is never glued onto the last assistant token.

### Shutdown Protocol

1. `state.running = 0` (set on `SDL_QUIT`).
2. Persist the active chat with `save_chat_history()`.
3. `SDL_HideWindow(win)` — user sees instant close.
4. `inference_request_stop(ictx)` — sets `ictx->running = 0`, raises `cancel_generation`, broadcasts on `prompt_cond`. Do **not** wake the worker by submitting an empty prompt.
5. `platform_thread_join_timeout(worker_thread, 1500)`:
   - **Linux:** `pthread_timedjoin_np`.
   - **macOS / Windows:** polls `pthread_kill(thread, 0)` with 100 ms sleeps.
6. **If join succeeds:** call `inference_shutdown()` (joins any in-flight load thread), tear down SDL/GL, return from `main`.
7. **If join times out:** call `_Exit(0)`. Do **not** call `pthread_cancel` — llama.cpp C++ internals are not async-cancel-safe and will SIGABRT.

## API Conventions

- llama.cpp uses **current API** (not deprecated aliases):
  - `llama_model_load_from_file()` (not `llama_load_model_from_file`)
  - `llama_init_from_model()` (not `llama_new_context_with_model`)
  - `llama_vocab_is_eog()` (not `llama_token_is_eog`)
  - `llama_batch_get_one(tokens, n_tokens)` (auto position tracking)
  - `llama_memory_clear(llama_get_memory(ctx), true)` to wipe the KV cache
  - `llama_model_chat_template(model, NULL)` + `llama_chat_apply_template(...)` for prompt formatting
  - `llama_n_batch(ctx)` to query the configured batch size (do not hardcode)
- `llama_tokenize(vocab, text, len, NULL, 0, add_special, parse_special)` — passing `tokens=NULL` / `n_tokens_max=0` returns the **negated** required count (buffer-too-small convention). Negate to get the real token count. `INT32_MIN` is the only true overflow error.
- Nuklear uses feature macros before every include:
  - `NK_INCLUDE_FIXED_TYPES`, `NK_INCLUDE_DEFAULT_ALLOCATOR`, `NK_INCLUDE_VERTEX_BUFFER_OUTPUT`, `NK_INCLUDE_FONT_BAKING`, `NK_INCLUDE_DEFAULT_FONT`, `NK_INCLUDE_STANDARD_IO`, `NK_INCLUDE_STANDARD_VARARGS`

## Base System Prompt

`BASE_SYSTEM_PROMPT` is a `static const char[]` defined at the top of `inference.c`. It is **always** included as the first part of the system message — the user-configurable system prompt (if any) is appended after it via `build_system_prompt(user_sys, out, out_size)`.

Contents: plain-text output requirement (no markdown), conciseness, language matching, offline context awareness.

- In normal mode: `combined_sys_plain[MAX_PROMPT_LEN + 2048]` holds the combined string; it is a `static` local in the worker loop (not on the per-call stack).
- In agent mode: `combined_sys[16384]` holds base + user sys + `agent_system_prompt()` concatenated.
- `build_system_prompt()` always emits a non-empty string — there is no path where the system message is absent.

## `<think>` Tag Stripping & Rendering

`emit_filtered_piece()` in `inference.c` transforms `<think>` / `</think>` byte sequences into `\n-- THINK --\n` / `\n-- END THINK --\n` markers.

- `tag_carry` (7 bytes, `TAG_CARRY_MAX`) is the longest possible partial-tag suffix kept between calls.
- `tag_prev_char` (new field in `inference_ctx_t`) records the last character emitted via `output_append_locked`. Reset to `'\n'` at the start of each turn to simulate "after a newline".
- **Line-start guard:** a `<think>` / `</think>` match is only accepted as a real tag if the character immediately before it in the stream is `'\n'` or `'\0'` (start of turn). If not, the `<` is emitted as plain text and scanning continues. This prevents `` `<think>` `` in model prose from triggering a false think block.
- Carry is reset (`tag_carry_len = 0`) at the start of each prompt and flushed verbatim at end of generation via `emit_filtered_flush()`.
- The UI rendering loop in `ui.c` splits chat history on these markers and on `\n> ` boundaries (user prompts), creating a separate `nk_edit_string` box per section.
- Sections with only whitespace characters are suppressed — no box and no "▒ thinking" label. This avoids empty boxes from stray `\n` between adjacent markers.

## Chat History Rendering

The right panel chat group iterates `local_hist` (a per-frame snapshot of `state->chat_history`) and emits one `nk_edit_string` box per logical section:

1. Search for the nearest of: next `-- THINK --`, next `-- END THINK --`, next `\n> ` (user prompt).
2. If `\n> ` comes first (regardless of think state), flush the current section and reset `in_think = 0` — this ensures each user/assistant turn is its own box.
3. If a think-start marker comes first, flush (closing any in-progress normal section) and enter think mode.
4. If a think-end marker comes first, flush the think section and return to normal mode.
5. At end of input, flush any remaining content.
6. `FLUSH_SECTION` checks for at least one visible byte before rendering — pure whitespace sections are silently discarded.

## Context Management

- `inference_get_context_stats()` formats the full conversation through the chat template, then probe-tokenises (NULL buf, 0 max → negated count) to measure usage without allocating. Returns 0 on success, -1 if no model is loaded.
- CTX display: `CTX: used / max (pct%)`. Colour: amber < 75 %, orange 75–90 %, red > 90 %.
- Auto-compact: triggered after generation when usage > 80 %. Uses `ui_compact_chat_history()`.
- Pre-send compact: triggered before submitting a prompt when usage > 75 %. Uses `compact_chat_history()` directly (simpler, mirrors happen in the subsequent `ui_set_chat_history()` call).
- Manual compact: `[ COMPACT ]` button calls `ui_compact_chat_history(state, 1)`. Refuses during generation.

## Sampler Stack

Built in `run_one_turn()` per prompt:

```c
llama_sampler_chain_add(smpl, llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp_now));   /* from settings_mutex */
llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
```

Pure greedy is not used — it causes repetition loops on small models.

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

- `hub_models[]` in `ui.c` is a fixed-size array (`WASTELAND_MAX_HUB_MODELS = 4`) of HF repo IDs. Entries must be real public GGUF repos. Current set: `Qwen/Qwen2.5-0.5B-Instruct-GGUF`, `ggml-org/gemma-3-1b-it-GGUF`, `Qwen/Qwen2.5-1.5B-Instruct-GGUF`, `ggml-org/SmolLM2-1.7B-Instruct-GGUF`.
- `/blob/main/<file>` → `/resolve/main/<file>` rewrite is `+3` bytes: `memmove` tail right by 3 *before* `memcpy`.

## seccomp Filter Scope

The Linux lockdown is **deliberately narrow**: only `socket(AF_INET, …)`, `socket(AF_INET6, …)`, and `socket(AF_PACKET, …)` are killed. We do **not** filter `connect`, `sendmsg`, `recvmsg`, `setsockopt`, etc., because:

1. seccomp cannot dereference user-space pointers — a blanket kill would SIGSYS the X11/Wayland socket the GUI uses every frame.
2. Gating `socket()` is sufficient: no new IP fd can be opened.
3. Already-open file descriptors keep working unchanged.

## Agent Mode — History & Tool Output

- **Multi-turn history in agent mode:** the worker now calls `parse_chat_history` in the agent branch too, capped at `AGENT_HISTORY_SLOTS = 8` messages (4 turns) to leave room for tool-call turns. `AGENT_MAX_MSGS = 30` (was 23).
- **Tool output in chat UI:** `read_file` and `list_dir` results are no longer echoed to the output buffer. The `[ TOOL: read_file | path ]` header is still emitted so the user can see what the model is doing. The file contents go only to `result_out` (the model's next-turn context).

## Font & DPI

- **Embedded font:** `src/font_dejavu.h` is a generated C byte array of DejaVu Sans Mono (343 KB). It is always available — no file-system lookup required. Covers Basic Latin, Cyrillic (Ukrainian U+0400–U+04FF), and Geometric Shapes (▶ ■ U+25A0–U+25FF).
- **Loading order in `nk_sdl_init(win, font_size)`:** (1) embedded data via `nk_font_atlas_add_from_memory`, (2) system TTF paths from `nk_sdl_font_candidates[]`, (3) built-in Proggy Clean (ASCII-only last resort).
- **DPI scaling:** `main.c` computes `dpi_scale = SDL_GL_GetDrawableSize / SDL_GetWindowSize` after window creation. The font is baked at `15.0f * dpi_scale` so text appears the same logical size on standard (1×), Retina/HiDPI (2×), and Windows-scaled (1.25×–2×) displays. Scale is clamped to [1, 4].
- **`memmem` on Windows:** MSVC does not provide `memmem`. A `static` fallback is defined inside `#ifdef _WIN32` in `agent.c`, placed before any call site. No forward declaration — a prior implicit `int ()` declaration from MSVC would conflict and trigger C2040.

## Build Notes

- `nuklear_impl.c` is the **single compilation unit** for Nuklear implementation.
- `nuklear_sdl_gl2.h` contains the SDL2 / OpenGL2 backend (both decls + impl, guarded by `NK_SDL_GL2_IMPLEMENTATION`).
- `src/font_dejavu.h` is generated by `xxd -i` from DejaVuSansMono.ttf. Regenerate with: `xxd -i DejaVuSansMono.ttf | sed 's/unsigned char .*/static const unsigned char wst_dejavu_ttf[]/; s/unsigned int .*/static const unsigned int wst_dejavu_ttf_len/' > src/font_dejavu.h`.
- llama.cpp lives in `third_party/llama.cpp/` (with a `vendor/llama.cpp` symlink) and is added as a CMake subdirectory with `LLAMA_BUILD_EXAMPLES=OFF`.
- `seccomp` is optional (Linux only); CMake skips it gracefully on other platforms.
- The application does **not** auto-load any model on boot.
- Package version: **0.3.0** (`CPACK_PACKAGE_VERSION` in `CMakeLists.txt`).
- MSVC: `nuklear_impl.c` is compiled with `/wd4701`; `ui.c` with `/wd5287`; `_CRT_SECURE_NO_WARNINGS` applied globally for MSVC builds.
- Linux `.deb` architecture is detected at CMake configure time from `CMAKE_SYSTEM_PROCESSOR` (`x86_64` → `amd64`, `aarch64` → `arm64`). CI builds both via `ubuntu-22.04` and `ubuntu-22.04-arm` runners.

## Style

- Pure C (C11), no C++ in application code.
- `snprintf()` for all string building.
- `pthread_mutex_lock/unlock` pairs always kept tight and symmetric.
- No globals except backend state in `nuklear_sdl_gl2.h`.
- Platform-specific code guarded by `#ifdef _WIN32`, `#ifdef __linux__`, etc.
- `volatile int` for cross-thread one-way flags (`running`, `cancel_generation`, `loading`, `load_result`); mutexes for everything else.
