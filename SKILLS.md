# SKILLS.md — Wasteland v0.2

## Available Skills

This project does not use a formal skill system. The following domains are relevant for contributors and AI agents.

### Systems Programming

- POSIX / Windows threads (`pthreads`, `CreateThread`).
- Mutexes, condition variables, thread-safe ring buffers.
- One-way `volatile int` flags for cross-thread cancellation (`running`, `cancel_generation`, `loading`).
- Cross-platform file I/O (`dirent.h` vs `FindFirstFileA`).
- Signal-safe and **non-async-cancel-safe** thread design — llama.cpp must never be `pthread_cancel`'d.
- Process-level fast-exit (`_Exit`) vs graceful shutdown trade-offs.
- Fine-grained mutex discipline: `settings_mutex` in `inference_ctx_t` protects tunables (`pending_n_ctx`, `temperature`) read at model-load and sampler-build time without blocking prompt handling.

### Asynchronous UI Patterns

- Off-loading multi-second work (model load, libcurl download) onto background threads with a published-result polling pattern (`is_loading()` / `take_load_result()`).
- Hiding latency: `SDL_HideWindow` to make the close click feel instant while threads finish in the background.
- Gating UI buttons on cross-thread state (`load_busy`, `gen_busy`) so the user can't trigger conflicting operations.
- **Dispatcher-claims-flag pattern:** when spawning a worker that owns a "busy" flag, the dispatcher sets the flag *before* `pthread_create`. Setting it inside the worker leaves a window where a fast double-click sees the old value and double-spawns.
- **Clean shutdown signalling:** dedicated `inference_request_stop()` flips the running flag, raises a cancel flag, and broadcasts the prompt cond_var. Don't repurpose a "submit empty work item" path for shutdown.
- **Propagating `volatile` through APIs:** every function that takes the address of a `volatile int` field must declare the parameter as `volatile int *`. Silently dropping the qualifier triggers `-Wdiscarded-qualifiers` and hides a real race from the optimiser.

### Graphics & UI

- Nuklear immediate-mode GUI: layouts, groups, scrolled groups (`nk_group_scrolled_offset_begin`), property widgets.
- **Full theme coverage:** every Nuklear widget type must be styled in `ui_apply_amber_theme()`. Missing entries (e.g. `s->property`, `s->property.inc_button`) leave default grey gradients that clash with the amber CRT aesthetic.
- Custom auto-scroll-to-bottom via the `nk_uint` scroll-offset API (Nuklear clamps oversized values to max).
- Collapsible UI panels dynamically adjusting layout widths.
- **Per-turn box rendering:** the chat rendering loop splits on `\n> ` boundaries unconditionally (not only inside think blocks). This ensures each user/assistant exchange occupies its own `nk_edit_string` box and new user prompts never render inside previous assistant reply blocks.
- **Empty-section suppression:** before calling `nk_edit_string`, scan the section buffer for at least one visible character (codepoint > `' '`). Sections with only `\n`/spaces (stray whitespace between `-- THINK --` markers) are dropped entirely — no box, no "▒ thinking" label.
- Word wrap with measured row heights using the active font's `width()` callback (`wrap_text_into()`).
- Custom rendering of TTF fonts (`DejaVuSansMono`) mapped with specific Unicode ranges (e.g. Cyrillic `0x0400-0x04FF`).
- SDL2 windowing and event handling.
- OpenGL 2.1 fixed-function pipeline.

### Machine Learning Inference

- llama.cpp C API (current, not deprecated aliases).
- GGUF model format and `llama_model_load_from_file` / `llama_init_from_model`.
- Tokenisation (`llama_tokenize` with `parse_special=true`, `llama_token_to_piece`).
- **Probe-tokenise pattern:** call `llama_tokenize(vocab, text, len, NULL, 0, …)` to count tokens without allocation. The API returns the negated count when the buffer is too small — negate to get the real count. `INT32_MIN` signals overflow and is the only true error case.
- Chat template application: `llama_model_chat_template` + `llama_chat_apply_template` with `add_ass=true`.
- KV-cache lifecycle: `llama_get_memory(ctx)` + `llama_memory_clear(mem, true)` for per-prompt resets. The entire formatted conversation is re-tokenised from scratch each turn so there is no incremental KV reuse — clearing is correct and avoids bloat from prior turns.
- Auto-positioning batches via `llama_batch_get_one(tokens, n_tokens)`.
- **Sampler stack:** `penalties(last_n, repeat_penalty) → top_k → top_p → temp → dist`. Pure greedy sampling causes small models to loop into repeated paragraphs — a repetition penalty is mandatory. Temperature is read from a mutex-protected field so it can be tweaked between turns.
- Prompt chunking: when n\_ctx > n\_batch (shouldn't happen with our matching values, but defensive), feed the prompt in `n_batch`-sized chunks using consecutive `llama_decode` calls. Use `llama_n_batch(ctx)` to query the actual batch size rather than a hardcoded constant.
- Detection of end-of-generation tokens via `llama_vocab_is_eog`.

### Chat History Management

- **Two-buffer model:** `state->chat_history` (main thread, under `chat_mutex`) and `ictx->chat_history` (inference mirror, under `history_mutex`). Always call `ui_set_chat_history()` to sync before submitting a prompt and after manual compact.
- **Parser invariant:** `parse_chat_history()` discards a trailing user message that has no assistant reply — this is the current prompt, supplied separately by the caller. Failing to do this causes the user turn to appear twice in the message list, which confuses the chat template.
- **Compact pipeline:** `ui_compact_chat_history(state, n)` is the only correct call site. It locks `chat_mutex`, compacts, unlocks, syncs to inference, saves chat to disk, updates CTX stats, and writes a status message. Direct calls to `compact_chat_history()` from button handlers bypass the sync and persistence steps.
- **Context stats:** compute using `parse_chat_history` + `llama_chat_apply_template` + `llama_tokenize` (probe mode). Call after every generation finish and after every compact. Display as `CTX: used / max (pct%)` with colour-coded progress bar (orange > 75 %, red > 90 %).

### Stream Processing & Data Persistence

- Token-by-token streaming with carry buffers for partial-match safety.
- **Line-start `<think>` detection:** `emit_filtered_piece()` only converts a `<think>` / `</think>` tag to a think-block marker when the character immediately preceding it in the output stream is `'\n'` or `'\0'` (start of turn). The `tag_prev_char` field (reset to `'\n'` at turn start, updated by every `output_append_locked` call) tracks this. Without the guard, `` `<think>` `` in model prose triggers a false think block that leaks into all subsequent output.
- Intercepting reasoning markers (`<think>` / `</think>`) and rendering them differently (dimmed text, "▒ thinking" label).
- Custom UI string parsers to selectively skip reasoning blocks when copying chat text to the clipboard.
- Generating human-readable filenames (stripping filesystem-unsafe characters) from dynamic prompts.
- Persisting state across application runs using simple text files (`system_prompt.txt`, `chats/*.txt`, `wasteland.cfg`).
- `wasteland.cfg` key=value format for agent settings and inference tunables (`n_ctx`, `temperature`). Shadow-variable change detection avoids unnecessary rewrites on every frame.
- Magic-prefix + RC4 obfuscation for chat files (`WSTL` header + cipher) — cosmetic-only "encryption" honest about its threat model.
- Trailing-newline injection at end-of-generation so a streaming append + a literal user-prompt append can coexist on a single shared buffer without glueing onto the same line.
- Stable filesystem listings: `qsort` the result of `readdir`/`FindFirstFile` because directory iteration order is filesystem-defined and inconsistent across launches.

### Network & Security

- libcurl easy API for `.gguf` downloads with progress callbacks.
- HuggingFace API discovery (`/api/models/.../tree/main`) to resolve a repo to a concrete file URL.
- HF URL rewriting: `/blob/main/<file>` → `/resolve/main/<file>`. The replacement is `+3` bytes, so you must `memmove` the tail right by 3 *before* the `memcpy` overwrite.
- Linux seccomp-bpf with **argument filtering** (`SCMP_A0(SCMP_CMP_EQ, AF_INET)`) — narrow rules that gate `socket()` creation by address family, leaving X11 / Wayland Unix-domain traffic untouched.
- Why pointer-deref filtering (sockaddr family on `connect`/`bind`) is impossible in seccomp and what to do instead (gate at `socket()`).
- Cross-platform security model: features degrade gracefully on macOS / Windows.

### Prompt Engineering / System Prompts

- **Built-in base system prompt pattern:** define a `static const char BASE_SYSTEM_PROMPT[]` in the inference module and always prepend it to the user-configurable system prompt via a `build_system_prompt()` helper. This guarantees consistent model behaviour (plain-text output, language matching, context awareness) regardless of whether the user has set a custom prompt.
- Key rules for a terminal LLM client: suppress markdown (the UI renders text verbatim), enforce conciseness, and communicate the offline/sandboxed nature of the runtime so the model doesn't hallucinate internet access.
- Agent mode system prompt = base + user sys + tool instructions, concatenated in that order. Buffer size must accommodate all three (~16 KB in Wasteland).

### Build Engineering

- CMake target configuration.
- Cross-platform `find_package` / `find_library` handling.
- Git submodule management for vendored llama.cpp.
- Platform-specific linking (SDL2, OpenGL, curl, seccomp on Linux; ws2_32, winmm on Windows).
- MinGW cross-compilation from Linux toolchain files.
- **Per-file MSVC warning suppression:** use `set_source_files_properties(file.c PROPERTIES COMPILE_OPTIONS "/wd<N>")` to suppress a warning from a third-party header included only in that translation unit. Avoids masking the same warning in application code.
- **Dynamic `.deb` architecture:** map `CMAKE_SYSTEM_PROCESSOR` to Debian arch names (`x86_64`→`amd64`, `aarch64`→`arm64`) at configure time so the same `CMakeLists.txt` produces correctly-named packages on both amd64 and ARM64 CI runners without manual overrides.

## Version

Current version: **0.2**
