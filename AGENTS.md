# AGENTS.md — Wasteland v0.5

## Agent Instructions

This file contains conventions and preferences for AI agents working on Wasteland.

## Build System

- **CMake** is the only supported build system.
- Run `./build.sh` to build on Linux; it auto-detects local venv `cmake`.
- Build artifacts live in `build/` — never edit them and never commit them.
- Cross-platform: Linux (amd64 + arm64), macOS (universal arm64+x86_64), Windows (MinGW / MSVC).
- Linux `.deb` architecture is auto-detected at configure time (`CMAKE_SYSTEM_PROCESSOR` → `amd64` / `arm64`). CI uses `ubuntu-22.04` and `ubuntu-22.04-arm` runners.
- MSVC: `nuklear_impl.c` is compiled with `/wd4701` to suppress C4701 false-positives from `nuklear.h`. Do not add `/wd4701` to the global target flags — keep it scoped to that translation unit via `set_source_files_properties`.

## Code Conventions

1. **Language:** Pure C (C11). No C++ in `src/*.c` files.
2. **Includes:** Order matters — SDL2 / OpenGL headers before Nuklear macros before `nuklear.h`.
3. **Thread Safety:** Every mutable shared field has either an explicit mutex or is a `volatile int` flag used as a one-way signal. Prefer fine-grained locks over coarse ones. Always release mutexes before any blocking call (`llama_decode`, `llama_token_to_piece`, etc.). Any pointer-to-int passed across thread boundaries must be `volatile int *` in the signature too — silently dropping the qualifier hides races.
4. **String Buffers:** Always size via `sizeof(buf) - 1` for `strncpy`. Prefer `snprintf` for formatted output. Capacity macros (`WASTELAND_MAX_CHATS`, `WASTELAND_CHAT_NAME_LEN`, `WASTELAND_MAX_MODELS`, `WASTELAND_MAX_HUB_MODELS`) live in `ui.h` — use them instead of literal integers.
5. **Error Handling:** Print to `stderr` with a `[module]` prefix, then return an error code. Never silently ignore failures. UI status text goes through `state->status_msg`.
6. **Cross-Platform:** Wrap platform-specific code in `#ifdef _WIN32`, `#ifdef __linux__`, `#ifdef __APPLE__`. Provide a working stub for non-Linux platforms when a feature is Linux-only (e.g. seccomp).
7. **Backwards-compat shims:** Don't add them. We're pre-1.0 — break the API and update callers.
8. **Race-free flag claims:** When dispatching a background pthread that owns a state flag (e.g. `download_active`), the **dispatcher** sets the flag to 1 *before* `pthread_create`. Setting it inside the worker leaves a window where a second click sees the flag still 0 and spawns a duplicate.

## Long-Running Operations

Anything that can take >100 ms must not run on the UI thread. The UI thread is the SDL event loop and must keep pumping at 60 FPS or the OS marks the window "Not Responding".

- **Model load:** use `inference_load_model_async()` and poll `inference_is_loading()` from `ui_render`. The synchronous `inference_load_model()` is only safe before the SDL window exists.
- **Token generation:** the worker thread already runs in the background. The per-token loop polls `ictx->cancel_generation` so a `STOP` click breaks out within one token.
- **Download:** existing detached pthread pattern; communicate progress via `volatile int state->download_*`. The UI claims `download_active = 1` before `pthread_create` to avoid the double-click race.
- **Update check:** a detached pthread queries the GitHub Releases API at startup (before seccomp lockdown). Result is written to `state->update_version`; UI shows an orange banner if non-empty. Timeout is 5 s so a slow connection never blocks the window.
- **Shutdown:** `inference_request_stop()` (defined in `inference.c`) is the canonical way to wake the worker for exit. Do **not** abuse `inference_submit_prompt(ictx, "", "")` for this — with a model loaded the chat template still emits non-empty tokens and the worker burns a generation cycle on the way out.

## Security Rules

- The seccomp filter (Linux only) uses `SCMP_ACT_KILL_PROCESS` and is **deliberately narrow**: only `socket(AF_INET, …)`, `socket(AF_INET6, …)`, and `socket(AF_PACKET, …)` are killed. **Do not** add kill rules for `connect`, `sendmsg`, `recvmsg`, `setsockopt`, etc. — those run on the existing X11 / Wayland Unix-domain socket every frame and a blanket kill SIGSYSes the GUI. Gating `socket()` is sufficient because no new IP fd can be opened.
- `lockdown_network()` is called **only** from the LOAD button handler in `ui.c`, after a successful async load. It is **not** called from `main.c` at startup.
- Download code lives only in `network.c`. Do not duplicate libcurl logic elsewhere.
- On non-Linux platforms, `lockdown_network()` is a no-op — do not add fake implementations.
- The UI hides the entire HUB MODELS section while `state->network_lockdown` is true.
- The `hub_models[]` array must contain real, public, reachable HF GGUF repos — fictional IDs fail at HF API resolution time with HTTP 404. When adding a new entry, click DOWNLOAD on it once before merging. Current set: `Qwen/Qwen2.5-0.5B-Instruct-GGUF`, `ggml-org/gemma-3-1b-it-GGUF`, `Qwen/Qwen2.5-1.5B-Instruct-GGUF`, `ggml-org/SmolLM2-1.7B-Instruct-GGUF`, `unsloth/Qwen3.6-35B-A3B-GGUF`.
- The `/blob/main/` → `/resolve/main/` URL rewrite in `network_download_model` must `memmove` the tail right by 3 bytes **before** the `memcpy` overwrite — the obvious order corrupts the first 3 bytes of the filename.

## UI Rules

- Amber monochrome palette only: `#FFB000` on dark charcoal / black. `amber_dim` used for reasoning text. Every Nuklear widget type (button, edit, checkbox, progress, scrollbar, property) must be themed in `ui_apply_amber_theme()` — do not leave any control with default grey gradients.
- Buttons use square brackets and verbs: `[ DOWNLOAD ]`, `[ NEW CHAT ]`, `[ ACTIVE ]`, `[ LOAD ]`, `[ DEL ]`.
- The right-panel send control has two states: `▶` (Play) when idle, `■` (Stop) while `state->is_generating` is true. Enter is gated through the same path so it never fires during generation.
- CRT terminal aesthetic — no rounded corners, no gradients.
- Long chat lines must wrap to the chat-panel width via `wrap_text_into()`. Hardcoded character counts will mis-wrap on resize and on non-monospace fonts.
- New chat content auto-scrolls to the bottom by setting `state->chat_scroll_y` to a large sentinel; Nuklear clamps it to real max.
- Left panel is collapsible (`«` / `»` toggle in the header). When collapsed, chat takes the full width.
- Status messages are shown below the input buffer and auto-cleared using `SDL_GetTicks()`.
- The `◈` icon copies assistant responses and explicitly skips content inside `-- THINK --` blocks.
- Each user/assistant turn is rendered in its own `nk_edit_string` box. The rendering loop splits on `\n> ` boundaries unconditionally (not only inside think blocks) so a new user prompt always starts a fresh box. Empty or whitespace-only sections (stray `\n` between markers) are suppressed and produce no box.

## Chat History & Context

- `state->chat_history` (main thread) and `ictx->chat_history` (inference mirror, protected by `history_mutex`) are two separate buffers. Call `ui_set_chat_history(state)` to sync them before submitting a prompt or after compacting.
- `parse_chat_history()` in `inference.c` produces user/assistant pairs. A trailing user message with no assistant reply (i.e. the current prompt in mid-submission) is **discarded** — the worker appends the current prompt separately. This prevents duplicating the user turn in the message list.
- **Agent mode includes history:** the agent ReAct branch calls `parse_chat_history` with a cap of `AGENT_HISTORY_SLOTS = 8` messages before appending the current user prompt. This gives the model multi-turn memory in agent mode at the cost of fewer available tool-call slots (still 10 turns, governed by `AGENT_MAX_MSGS = 30`).
- **Agent tool output:** `read_file` and `list_dir` results are NOT printed to the chat UI — only the `[ TOOL: name | path ]` header is shown. File contents are still passed to the model via `result_out`. Do not re-add the `emit_raw_str(ictx, result_out)` call — it floods the chat with raw file contents.
- `ui_compact_chat_history(state, n)` is the **only** correct way to call compact from UI code. It locks `chat_mutex`, runs `compact_chat_history()`, pushes the result to inference via `ui_set_chat_history()`, saves the active chat to disk, updates the CTX bar, and writes a status message. Never call `compact_chat_history()` directly from button handlers.
- `inference_get_context_stats()` calls `llama_tokenize()` with `tokens=NULL` / `n_tokens_max=0` to count without allocating. The API returns **negative** count in this case (buffer-too-small convention) — negate it to get the real count. Do not treat negative as failure.

## Sampler Stack

The worker builds a sampler chain in `run_one_turn()` on every prompt:

```
penalties(last_n=64, repeat=1.1) → top_k(40) → top_p(0.95) → temp(ictx->temperature) → dist(seed)
```

- Pure greedy (`llama_sampler_init_greedy`) is **not used** — small models (≤2B) degenerate into looping repetition without a repetition penalty.
- Temperature is read from `ictx->temperature` (under `settings_mutex`) at sampler-build time so it can be tweaked between turns without reloading the model.
- N\_CTX is read from `ictx->pending_n_ctx` (under `settings_mutex`) at model-load time. Changing it requires an unload + reload cycle.

## Base System Prompt

`BASE_SYSTEM_PROMPT` in `inference.c` is always prepended to the system message (before any user-configured system prompt). It enforces:

- **Plain-text output** — no markdown (`**bold**`, `# headings`, `* bullets`, ` ``` ` fences) unless the user explicitly requests formatted output. The terminal renders text as-is; markdown appears as raw characters.
- **Conciseness** — no padding or unnecessary preambles.
- **Language matching** — reply in whatever language the user writes in.
- **Offline awareness** — model knows it has no internet access unless Agent Mode is active.

`build_system_prompt(user_sys, out, out_size)` handles the concatenation. Never skip the base prompt to "give the model more freedom" — it directly fixes the most common output quality problem (markdown flooding the terminal).

## Stream Filtering

- `<think>` and `</think>` are intercepted by `emit_filtered_piece()` and replaced with `\n-- THINK --\n` / `\n-- END THINK --\n` markers.
- The filter uses a 7-byte carry buffer (`TAG_CARRY_MAX`) to handle tags split across two token pieces.
- **Line-start guard:** a tag is only treated as a real think marker if the character immediately before it in the stream is `'\n'` or `'\0'` (start of turn). This prevents `` `<think>` `` in model prose from creating a spurious think block. The `tag_prev_char` field in `inference_ctx_t` (reset to `'\n'` at turn start, updated by `output_append_locked`) tracks this.
- Reset `tag_carry_len = 0` and `tag_prev_char = '\n'` at the start of every new prompt; flush carry via `emit_filtered_flush()` at end of generation.

## Settings Persistence

Runtime tunables (N\_CTX, Temperature) are stored in `wasteland.cfg` alongside agent settings:

```
agent_mode=0
agent_workspace=/path/to/ws
n_ctx=8192
temperature=0.800
```

- Load on startup in `main.c`; push to `inference_set_n_ctx` / `inference_set_temperature` immediately.
- Persist on every change (compare to `last_*` shadow variables in the main loop).
- Valid ranges: `n_ctx` 512–262 144, `temperature` 0.01–5.0.

## Testing Protocol

Before declaring a task complete:

1. Build succeeds: `./build.sh` (or `cmake --build build -j$(nproc)`).
2. No new compiler warnings.
3. Binary starts without segfault.
4. Window closes immediately when X is clicked — the user should not see "Not Responding".
5. UI stays responsive while a model is loading (no frozen amber rectangle).
6. `STOP` interrupts an in-flight response within ~1 token.
7. Sending "hello" to an instruction-tuned model produces a coherent greeting (not raw-text continuation) — confirms the chat template path.
8. A second prompt in the same session must reference the first exchange — confirms multi-turn history is working.
9. CTX bar shows a non-zero percentage after one exchange.
10. `[ COMPACT ]` visibly shrinks the chat and shows a status message; clicking it again when only one turn remains shows "Nothing to compact".
11. Cross-platform changes must not break the Linux build.
12. Cyrillic text (Ukrainian) must render correctly — the font covers U+0400–U+04FF via the embedded DejaVu Sans Mono array in `src/font_dejavu.h`.
13. ▶ and ■ buttons must render (not show `?`) — these are in Geometric Shapes U+25A0–U+25FF, also covered by the embedded font.
14. On a HiDPI display (or Windows at 125%/150% scaling) text must not be tiny — the font is scaled by `SDL_GL_GetDrawableSize / SDL_GetWindowSize`.
15. Auto-update banner must appear when `state->update_version` is non-empty (can be tested by temporarily hardcoding a different version string).
16. A new chat's file must be renamed by the model-generated title after the first assistant reply completes (check `chats/` directory).
17. ARM64 `.deb` built in CI must run on Raspberry Pi 4/5 without `Illegal instruction`.
18. All CTest suites pass (`ctest --output-on-failure`) — currently **81 tests** across 4 suites (`agent`, `chat_history`, `version`, `string_utils`).
19. New logic must have a matching `tests/test_*.c` suite if it is testable without SDL/llama.cpp (pure string / file / math functions). Filesystem-touching tests use `/tmp`-style scratch directories created in `run_<suite>()` *before* `RUN_TEST` calls.
20. Existing tests must not be broken by the change — run `ctest` before every commit.
21. **Test parity rule:** when a function lives in `src/*.c` and has been copy-extracted into `tests/test_*.c` (e.g. `parse_chat_history`, `version_newer_than`, `rc4_crypt_buffer`, the HF URL rewrite), edits to the production version MUST be mirrored into the test copy. The duplication is intentional — it lets the test binaries link without SDL2 / llama.cpp / curl — but you have to keep both halves in lockstep or the tests start passing the wrong code.

## Font & DPI Rules

- **Never remove `src/font_dejavu.h`** from the repo — it is the only reliable cross-platform source of Cyrillic + symbol glyphs. If you need to regenerate it: `xxd -i DejaVuSansMono.ttf | sed ...` (see CLAUDE.md Build Notes).
- **`nk_sdl_init` signature is `(SDL_Window *win, float font_size)`** — always pass the DPI-scaled size computed in `main.c`. Passing 0 falls back to 15 px.
- **`memmem` on MSVC:** provide a `static` implementation inside `#ifdef _WIN32` at the top of the TU, before any call site. A separate forward declaration triggers C2040 because MSVC already has an implicit `int ()` entry.
- **Windows POSIX macros:** `S_ISDIR` is not defined by MSVC. Add `#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)` inside the `#ifdef _WIN32` include block of any TU that uses it.

## Version

Current version: **0.5**
