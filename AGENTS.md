# AGENTS.md — Wasteland v0.1

## Agent Instructions

This file contains conventions and preferences for AI agents working on Wasteland.

## Build System

- **CMake** is the only supported build system.
- Run `./build.sh` to build on Linux; it auto-detects local venv `cmake`.
- Build artifacts live in `build/` — never edit them and never commit them.
- Cross-platform: Linux, macOS, Windows (MinGW / MSVC).

## Code Conventions

1. **Language:** Pure C (C11). No C++ in `src/*.c` files.
2. **Includes:** Order matters — SDL2 / OpenGL headers before Nuklear macros before `nuklear.h`.
3. **Thread Safety:** Every mutable shared field has either an explicit mutex or is a `volatile int` flag used as a one-way signal. Prefer fine-grained locks over coarse ones. Always release mutexes before any blocking call (`llama_decode`, `llama_token_to_piece`, etc.).
4. **String Buffers:** Always size via `sizeof(buf) - 1` for `strncpy`. Prefer `snprintf` for formatted output.
5. **Error Handling:** Print to `stderr` with a `[module]` prefix, then return an error code. Never silently ignore failures. UI status text goes through `state->status_msg`.
6. **Cross-Platform:** Wrap platform-specific code in `#ifdef _WIN32`, `#ifdef __linux__`, `#ifdef __APPLE__`. Provide a working stub for non-Linux platforms when a feature is Linux-only (e.g. seccomp).
7. **Backwards-compat shims:** Don't add them. We're at v0.1 — break the API and update callers.

## Long-Running Operations

Anything that can take >100 ms must not run on the UI thread. The UI thread is the SDL event loop and must keep pumping at 60 FPS or the OS marks the window "Not Responding".

- **Model load:** use `inference_load_model_async()` and poll `inference_is_loading()` from `ui_render`. The synchronous `inference_load_model()` is only safe before the SDL window exists (and we no longer call it that way).
- **Token generation:** the worker thread already runs in the background. The per-token loop polls `ictx->cancel_generation` so a `STOP` click breaks out within one token.
- **Download:** existing detached pthread pattern; communicate progress via `state->download_*`.

## Security Rules

- The seccomp filter (Linux only) uses `SCMP_ACT_KILL_PROCESS` and is **deliberately narrow**: only `socket(AF_INET, …)`, `socket(AF_INET6, …)`, and `socket(AF_PACKET, …)` are killed. **Do not** add kill rules for `connect`, `sendmsg`, `recvmsg`, `setsockopt`, etc. — those run on the existing X11 / Wayland Unix-domain socket every frame and a blanket kill SIGSYSes the GUI. Gating `socket()` is sufficient because no new IP fd can be opened.
- `lockdown_network()` is called **only** from the LOAD button handler in `ui.c`, after a successful async load. It is **not** called from `main.c` at startup. seccomp cannot be undone for the lifetime of the process — applying it pre-UI is a one-way trap that blocks all subsequent downloads.
- Download code lives only in `network.c`. Do not duplicate libcurl logic elsewhere.
- On non-Linux platforms, `lockdown_network()` is a no-op — do not add fake implementations.
- The UI hides the entire HUB MODELS section while `state->network_lockdown` is true so the user cannot accidentally trigger a doomed `connect()`.

## UI Rules

- Amber monochrome palette only: `#FFB000` on dark charcoal / black.
- Buttons use square brackets and verbs: `[ DOWNLOAD ]`, `[ REFRESH ]`, `[ DELETE ]`, `[ CANCEL ]`, `[ LOAD: name | size ]`, `[ UNLOAD: name | size ]`, `[ LOADING: name | size ... ]`.
- The right-panel send control has two states: `TRANSMIT` when idle, `STOP` while `state->is_generating` is true. Enter is gated through the same path so it never fires during generation.
- CRT terminal aesthetic — no rounded corners, no gradients.
- Long chat lines must wrap to the chat-panel width. Use `count_wrap_lines()` + `nk_label_colored_wrap` with a row height of `font_h * wraps + 2`. Hardcoded character counts will mis-wrap on resize and on non-monospace fonts.
- New chat content auto-scrolls to the bottom by setting `state->chat_scroll_y` to a large sentinel before `nk_group_scrolled_offset_begin`. Do not introduce `nk_edit_string` for the chat — its scrollbar position is not exposed.

## Stream Filtering

- `<think>` and `</think>` literals are stripped by `emit_filtered_piece()` in `inference.c`. The reasoning text between them is preserved.
- The filter uses a 7-byte carry buffer (`TAG_CARRY_MAX`) to handle tags that get split across two token pieces. If you change the set of stripped literals, update both the carry size (longest literal − 1) and the search loops.
- Reset `tag_carry_len = 0` at the start of every new prompt and flush via `emit_filtered_flush()` at end of generation.

## Testing Protocol

Before declaring a task complete:

1. Build succeeds: `./build.sh` (or `cmake --build build -j$(nproc)`).
2. No new compiler warnings.
3. Binary starts without segfault.
4. Window closes immediately when X is clicked — the user should not see "Not Responding".
5. UI stays responsive while a model is loading (no frozen amber rectangle).
6. `STOP` interrupts an in-flight response within ~1 token.
7. Sending "hello" to an instruction-tuned model produces a coherent greeting (not raw-text continuation) — confirms the chat template path.
8. Cross-platform changes must not break the Linux build.

## Version

Current version: **0.1**
