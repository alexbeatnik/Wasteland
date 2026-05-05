# CLAUDE.md — Wasteland v0.6

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
- **Update Check Thread:** Detached pthread spawned once at startup (before any model load / seccomp lockdown). Calls `network_check_update()` to query GitHub Releases API; if a newer version exists, writes `update_version` into `app_state_t` so the UI can display an orange banner.

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
  - GCC/Clang: `-O3 -Wall -Wextra`; `-march=native` is **only** added for local dev builds (`$CI` env var unset) because it produces binaries that crash with `Illegal instruction` on ARM64 devices when built on a server-class CI runner.
  - MSVC: `/O2 /W4`
- **CI ARM64 builds:** GitHub Actions `ubuntu-22.04-arm` runner passes `-DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8-a` to CMake so the resulting `.deb` runs on Raspberry Pi / ClockworkPi and other baseline ARMv8-A devices.

## Hub Models & URL Rewrite

- `hub_models[]` in `ui.c` is a fixed-size array (`WASTELAND_MAX_HUB_MODELS = 5`) of HF repo IDs. Entries must be real public GGUF repos. Current set: `Qwen/Qwen2.5-0.5B-Instruct-GGUF`, `ggml-org/gemma-3-1b-it-GGUF`, `Qwen/Qwen2.5-1.5B-Instruct-GGUF`, `ggml-org/SmolLM2-1.7B-Instruct-GGUF`, `unsloth/Qwen3.6-35B-A3B-GGUF`.
- `/blob/main/<file>` → `/resolve/main/<file>` rewrite is `+3` bytes: `memmove` tail right by 3 *before* `memcpy`.

## seccomp Filter Scope

The Linux lockdown is **deliberately narrow**: only `socket(AF_INET, …)`, `socket(AF_INET6, …)`, and `socket(AF_PACKET, …)` are killed. We do **not** filter `connect`, `sendmsg`, `recvmsg`, `setsockopt`, etc., because:

1. seccomp cannot dereference user-space pointers — a blanket kill would SIGSYS the X11/Wayland socket the GUI uses every frame.
2. Gating `socket()` is sufficient: no new IP fd can be opened.
3. Already-open file descriptors keep working unchanged.

## Auto-Update

The auto-update flow is split across three threads + a generated shell/batch script:

1. **Startup probe (`update_check_thread` in `main.c`):** detached pthread launched **before** any model load (so seccomp lockdown can't kill it). Calls `network_check_update()` which hits `https://api.github.com/repos/alexbeatnik/wasteland/releases/latest` with a 5 s timeout. Parses `tag_name` via simple `strstr(...,"\"tag_name\":\"v")`. If `version_newer_than(latest, WASTELAND_VERSION)` returns true, writes `latest` into `state->update_version`. UI polls this every frame and renders an orange banner under the header when non-empty.
2. **Download (`update_download_thread` in `main.c`):** spawned when the user clicks `[ DOWNLOAD UPDATE ]`. Computes the platform-appropriate artifact name via `build_update_filename()` (`Wasteland-windows.exe`, `Wasteland-macos.dmg`, `wasteland_<ver>_amd64.deb`, `wasteland_<ver>_arm64.deb`) and downloads it into `downloads/<file>` via `network_download_model`. Sets `state->update_state` to 1 on success, 2 on failure.
3. **Restart (`launch_updater` in `main.c`):** when the user clicks `[ RESTART TO UPDATE ]`, this generates a small shell script (POSIX) or batch file (Windows) into `/tmp` / `%TEMP%`, then exits the app. The script polls until our PID is gone, then replaces the binary in place — using `pkexec`/`osascript`/`runas` to elevate when the install path isn't user-writable. After the install it relaunches the new binary and self-deletes.

### Linux install path uses `dpkg`, NOT `cp`

The Linux artefact is a Debian package archive (`wasteland_<ver>_<arch>.deb`), **not** an ELF binary. The naïve `cp $NEW $OLD` would replace the running executable with a tarball and brick the install. The script must invoke `dpkg -i $NEW` (with elevation), which extracts the .deb and lays the binary down at `/usr/bin/Wasteland` via the package manager. The script tries `pkexec` first (PolicyKit, ships on Ubuntu/Debian/Mint with a desktop), falls back to `gksudo` / `kdesu`, and surfaces a `notify-send` / `zenity` / `xmessage` notification with the manual install command on failure. The `.deb` is **kept** on disk if install failed, so the user can follow the notification's `sudo dpkg -i …` instruction.

### CI release flow — five jobs, two gating outputs

`.github/workflows/build.yml` has five jobs in a strict dependency chain:

```
extract-version  →  create-tag        ┐
                                      ├─→  build  →  release
       test ─────────────────────────┘
```

- **`extract-version`** runs only on `push` to `main`. It reads `WASTELAND_VERSION` from `src/ui.h` and emits two outputs: `version=v0.6` (the tag string) and `tag_exists=true|false` (a probe of `git ls-remote --tags`). `tag_exists` is the gate that prevents cosmetic main-pushes from regenerating an already-published release.
- **`create-tag`** pushes `v0.6` to origin if it doesn't already exist. Runs on main pushes and `workflow_dispatch`. Note: tags pushed via `GITHUB_TOKEN` do NOT trigger a new workflow run (GitHub safety against recursive events) — so the same workflow run must continue to build + release.
- **`test`** is unconditional — every event runs the 87-test ctest suite before anything else proceeds.
- **`build`** is the 4-platform matrix (Linux amd64, Linux arm64, macOS universal, Windows). Depends on `test` + `create-tag`.
- **`release`** depends on **both** `build` AND `extract-version` — the `extract-version` dependency is critical because that's the only path through which the resolved tag reaches the release step on a main-branch push.

### Why `release` reads its tag from a dedicated step, not directly from `tag_name:`

The earlier shape `tag_name: ${{ ... || needs.extract-version.outputs.version || github.ref_name }}` looks correct but had a subtle bug: when `extract-version` wasn't in `needs`, the middle expression silently evaluated to null and the OR fell through to `github.ref_name`. On a main-branch push, `github.ref_name` is the literal string `main` — so `softprops/action-gh-release` would create both a release AND a tag named `main` at the current commit, while ignoring the `v0.6` tag that `create-tag` had just published.

Current shape: a dedicated `Resolve release tag` step does the priority resolution (`inputs.version` → `extract-version.outputs.version` → `ref_name` only-if-tag), validates the result against `^v[0-9]+\.[0-9]+(\.[0-9]+)?$`, and fails the job loudly if no usable version is found instead of falling through to a branch name. That regex is the canonical version-tag shape — `extract-version`, `create-tag`, and `release` all assume it.

### Why `release` skips on a main-push when the tag already exists

`needs.extract-version.outputs.tag_exists == 'false'` is part of the release `if`. Without it, every cosmetic push to `main` (README typo, doc tweak, version stays at `0.6`) would rerun the whole build matrix, upload the same `.deb`/`.dmg`/`.exe` to the existing `v0.6` release, and **wipe any hand-edited release notes** from the GitHub UI. With the gate: if `v0.6` is already tagged, the release job is silently skipped on main pushes — you only get a release when `WASTELAND_VERSION` is bumped. Manual `workflow_dispatch` and direct tag pushes still publish unconditionally.

### Absolute-path resolution before script generation

`pkexec` resets CWD to `/root`, `osascript with administrator privileges` to `/`, Windows ShellExecute to `%TEMP%` — none of them inherit the parent's CWD. A relative `downloads/wasteland_0.6_amd64.deb` passed verbatim into the generated script would not resolve. `launch_updater()` always pre-resolves `new_file` to an absolute path before writing the script: POSIX uses `realpath()` with a `getcwd()/relative` fallback; Windows uses `_fullpath()`. If you ever bypass this layer (e.g. a hot-patch that calls the script directly), make sure the path you pass in is already absolute.

### Filename matrix (`build_update_filename`)

| Platform | Artifact |
|---|---|
| Windows | `Wasteland-windows.exe` |
| macOS | `Wasteland-macos.dmg` |
| Linux x86_64 | `wasteland_<ver>_amd64.deb` |
| Linux aarch64 | `wasteland_<ver>_arm64.deb` |

`version_newer_than` accepts `X.Y` or `X.Y.Z` with optional leading `v`. Both the API tag (`v0.6`) and `WASTELAND_VERSION` (`0.6`) are normalised by skipping non-digit prefix chars before `sscanf`.

## Chat Creation Lifecycle (lazy)

`[ NEW CHAT ]` does **not** create an entry on disk. It saves the active chat (if any), clears `state->chat_history`, and sets `state->selected_chat = -1`. The chat is materialised lazily inside the prompt-submit handler when the user actually types something:

1. User types and submits the first prompt.
2. The submit handler observes `selected_chat == -1`, calls `generate_chat_name_from_prompt(input, ...)` which produces a filename like `Прочитай рідмі і скажи.txt` (word-boundary trim, UTF-8-safe, 60-byte cap).
3. A new entry is appended to `state->chats[]`, `selected_chat` is set, an empty file is created, and `inference_set_needs_title(ictx, 1)` schedules the model-generated 3–5 word title pass after the assistant reply.

Result: there is no permanent "New Chat" filename. Hitting `[ NEW CHAT ]` and then immediately switching to another chat creates nothing — no orphan files. The same code path handles both "first ever message" (selected_chat starts at -1) and "user clicked NEW CHAT" (selected_chat reset to -1).

`generate_chat_name_from_prompt` cap is **60 bytes** (≈ 30 Cyrillic chars or 60 ASCII), with two safety passes: (a) if cap was hit, scan back to the last space within the trailing third of the buffer to avoid mid-word cuts; (b) `trim_partial_utf8()` drops any incomplete UTF-8 continuation/lead bytes left at the tail. Without (b), a Cyrillic char split across the cap would persist on disk as an invalid sequence and break some filesystems on import/export.

## Multi-Line Prompt Input

The bottom-of-right-panel prompt input is `nk_edit_string(NK_EDIT_BOX | NK_EDIT_MULTILINE)` — a true multi-line text box. Pasting a multi-paragraph clipboard preserves newlines; the box scrolls internally for long content. Submit is button-only (`▶` glyph) — Enter inserts a newline, never commits, so multi-line composition Just Works.

### `state->input_expanded` toggle

A small `⤡` / `⤢` button next to `▶` flips the input between two layouts:

- **Collapsed (default):** input height = `34` px, chat scroll group fills the rest of the right panel (`height - 200 - pending_panel_h`).
- **Expanded:** input grows to `height - 220 - pending_panel_h` (with `80` px floor), chat shrinks to a `60` px sliver. Useful for composing or pasting long prompts where you need to see the whole thing before sending.

The 60 px chat sliver is intentional — keeps the last assistant reply visible so you can refer to it while composing the follow-up.

### Storage format: consecutive `> ` lines

When the user submits a multi-line prompt (`input_buffer` contains `\n`s), the UI walks the buffer line-by-line and emits **one `> {line}\n` segment per source-line** into `chat_history`. So a 3-line paste lands as:

```
> first paragraph line
> second paragraph line
> third line
```

`parse_chat_history` then has a continuation loop that glues consecutive `> ` lines back into a single user message with embedded `\n`s, so the chat template sees the user's original line breaks on the next turn.

This format keeps the existing `\n> ` boundary detection (compact, render-split, `parse_chat_history`'s "next user prompt" scan) working without changes. The naïve alternative — write `> {raw_input_buffer}\n` with embedded `\n`s — would split the user's prompt across multiple "turns" because the parser's boundary scanner sees a `\n` followed by `> `.

## Tool-Fence Stripping in Chat View

In agent mode the model emits markdown fences (` ```read_file `, ` ```list_dir `, ` ```write_file `, ` ```apply_edit `) which the worker also annotates with a `[ TOOL: name | path ]` line. The fence body adds 3–6 visual lines per call to the chat panel for information already conveyed by the TOOL marker.

`strip_tool_fences(in, out, out_size)` in `ui.c` is a per-frame view-only transform that copies `raw_hist` → `local_hist`, eliding any line-anchored ` ``` ` fence whose header matches one of the four agent tool names. Everything else (prose, `-- THINK --` markers, `\n> ` user prompts, `[ TOOL: ... ]` markers) is preserved byte-for-byte. The chat file on disk is untouched — agent re-parsing on the next turn still sees the raw fences.

Mid-stream behaviour: when the model has emitted the opening fence but no closing yet, the entire tail is skipped — the user sees the prose before the fence, then nothing new until the closing ` ``` ` is emitted, at which point the next visible byte is the `[ TOOL: ... ]` line. This is intentional — it spares the user a typewriter view of a fence body they're going to lose anyway.

`is_tool_fence_header(p, hlen)` is the canonical recogniser; it matches exactly the four tool names with optional trailing spaces / `\r`. Plain code fences (` ```c `, ` ```json `, ` ```python `) pass through unchanged.

## Auto-Generated Chat Titles

When the user sends the first message in a newly-created chat, the UI sets `inference_set_needs_title(ictx, 1)`. After the normal assistant reply finishes, the worker runs a short secondary inference pass (max 20 tokens) with a dedicated title-generation prompt:

```
System: Generate very short chat titles. Reply with ONLY the title text...
User: [original prompt]
User: Give this conversation a short 3-5 word title. Output ONLY the title text, nothing else.
```

- `title_mode` flag in `inference_ctx_t` redirects token output from `output_buffer` to `title_buffer` so the title never appears in chat.
- The title is stripped of newlines, quotes, markdown, and `<>` characters.
- The UI polls `inference_take_title()` each frame after generation ends; when ready, it renames the chat file (`chats/*.txt`) and updates the label in the left panel.

## Agent Mode — History & Tool Output

- **Multi-turn history in agent mode:** the worker now calls `parse_chat_history` in the agent branch too, capped at `AGENT_HISTORY_SLOTS = 8` messages (4 turns) to leave room for tool-call turns. `AGENT_MAX_MSGS = 30` (was 23).
- **Tool output in chat UI:** `read_file` and `list_dir` results are no longer echoed to the output buffer. The `[ TOOL: read_file | path ]` header is still emitted so the user can see what the model is doing. The file contents go only to `result_out` (the model's next-turn context).

## Agent Pending-Approval Panel — Diff Palette

When a `write_file` or `apply_edit` is awaiting user approval, `ui.c` renders a top-of-right-panel preview with a deliberate red/green diff palette mirroring [docs/index.html](docs/index.html)'s agent-proposal block:

- **`rej_c = #FF6020`** — orange-red for SEARCH (delete side) and the `[ REJECT ]` button.
- **`add_c = #AACC00`** — yellow-green for REPLACE (add side), `write_file` body, and the `[ APPLY ]` button.
- **`warn = #FFB000`** — amber heading colour for `▼ AGENT PROPOSAL — REVIEW` and the `// SEARCH:` / `// REPLACE WITH:` labels.

### Why `nk_edit_string`, not `nk_group` + `nk_label_colored_wrap`

The first iteration tried bordered `nk_group` containers with `nk_label_colored_wrap` text inside. Result: the boxes rendered with the right colours but the text was invisible — `nk_label_colored_wrap` does its own layout allocation that doesn't compose with a fixed-height parent group (the wrapped lines were being clipped past the group's reserved height). The chat-history panel doesn't hit this because it uses `nk_edit_string` with `NK_EDIT_BOX | NK_EDIT_MULTILINE` and pre-computed row heights.

Current implementation: each diff block IS an `nk_edit_string(NK_EDIT_BOX | NK_EDIT_MULTILINE, …)` writing into a static buffer that is repopulated from `p_search` / `p_replace` / `p_content` every frame (so any stray edits are clobbered immediately — read-only-ish). The buffer's text + border + cursor colours are tinted via a snapshot-and-restore on `nk->style.edit`:

```c
struct nk_style_edit saved_edit = nk->style.edit;
nk->style.edit.border_color = rej_c;
nk->style.edit.text_normal  = rej_c;
/* ... render edit_string ... */
nk->style.edit = saved_edit;
```

The action buttons use the same snapshot-and-restore pattern on `nk->style.button` (border / text / hover / active) per click target, so only those two buttons carry the diff palette — the global amber theme stays untouched. Hover state inverts text → black, background → tint, so buttons feel "armed" before the click commits.

`pending_panel_h` is `230` for `write_file` (one green 130 px block) and `290` for `apply_edit` (two 80 px blocks: red SEARCH + green REPLACE). Chat area height is reduced by this amount so the input field and CTX bar never get pushed offscreen.

## Font & DPI

- **Embedded font:** `src/font_dejavu.h` is a generated C byte array of DejaVu Sans Mono (343 KB). It is always available — no file-system lookup required. Covers Basic Latin, Cyrillic (Ukrainian U+0400–U+04FF), and Geometric Shapes (▶ ■ U+25A0–U+25FF).
- **Loading order in `nk_sdl_init(win, font_size)`:** (1) embedded data via `nk_font_atlas_add_from_memory` (using `sizeof(wst_dejavu_ttf)` to guarantee correctness), (2) system TTF paths from `nk_sdl_font_candidates[]` (including `/System/Library/Fonts/Menlo.ttc` on macOS), (3) built-in Proggy Clean (ASCII-only last resort).
- **Bake failure retry:** If `nk_font_atlas_bake` returns NULL (usually because 2×2 oversampling produces an atlas too large for the GPU on HiDPI), the atlas is cleared and retried with `oversample_h = oversample_v = 1`. This fixes missing Cyrillic / Geometric Shapes glyphs on Windows/macOS HiDPI.
- **DPI scaling:** `main.c` computes `dpi_scale = SDL_GL_GetDrawableSize / SDL_GetWindowSize` after window creation. The font is baked at `15.0f * dpi_scale` so text appears the same logical size on standard (1×), Retina/HiDPI (2×), and Windows-scaled (1.25×–2×) displays. Scale is clamped to [1, 4].
- **Windows DPI fallback:** On Windows with per-monitor DPI awareness, `SDL_GL_GetDrawableSize` sometimes returns the same value as `SDL_GetWindowSize` even when scaling is active. If the computed ratio is ≤ 1.01, we fall back to `SDL_GetDisplayDPI / 96.0f` to ensure readable text.
- **`memmem` on Windows:** MSVC does not provide `memmem`. A `static` fallback is defined inside `#ifdef _WIN32` in `agent.c`, placed before any call site. No forward declaration — a prior implicit `int ()` declaration from MSVC would conflict and trigger C2040.

## Testing

- Tests live in `tests/` and use a **zero-dependency** macro framework (`tests/test_framework.h`). No external test library is required.
- `ctest --output-on-failure` (or `make test`) runs all suites after the main build.
- To test `static` functions without exposing them in public headers, use `#ifdef TESTING` to drop the `static` keyword and provide a forward-declaration header for the test file (e.g. `inference_test.h`).
- Suites that depend on the filesystem (sandbox path resolution, agent executors, list_dir) must create their scratch directories inside `run_<suite>` *before* the first `RUN_TEST` call. The agent suite uses two separate trees (`wst_test_ws/` for resolver tests, `wst_test_exec_ws/` for read/write/edit/list executors) so failure artefacts can be inspected without polluting the resolver fixtures.
- Suites that depend on SDL / llama.cpp / curl **must not** link the whole application binary. Instead, copy the pure functions into a self-contained `tests/test_*.c` file (see `tests/test_chat_history.c`, `tests/test_version.c`, `tests/test_string_utils.c`). Each copied block carries an "origin: src/*.c" reference so the duplicate can be hand-synced when the source is touched.
- The agent suite **does** link `src/agent.c` directly (`add_executable(test_agent tests/test_agent.c src/agent.c)`) because it has no SDL / llama.cpp dependency — that lets us exercise the real `agent_exec_*` executors end-to-end against a scratch workspace.
- All tests run automatically in CI via `cmake --build build && ctest --output-on-failure`.

### Suite manifest (89 tests as of v0.6)

| Suite | Tests | Targets |
|---|---|---|
| `test_agent` | 35 | `agent_parse_calls` (16 cases inc. malformed `apply_edit`, multi-line blocks, non-tool fences, inline backticks), `agent_resolve_path` (5 sandbox cases), `agent_exec_read_file/write_file/apply_edit/list_dir` (13 round-trip + escape-block cases), `agent_system_prompt` smoke test |
| `test_chat_history` | 18 | `parse_chat_history` (LF, CRLF, UTF-8, leading newlines, trailing pending prompt, `> ` inside reply, three-turn pending-last, **multi-line user prompt**, **two consecutive multi-line turns**) · `build_system_prompt` (with / without / NULL user prompt, base-then-user order) |
| `test_version` | 15 | `version_newer_than` (X.Y vs X.Y.Z, `v` prefix mix, multi-digit minor, empty / garbage prefix, release-tag-vs-runtime) · `build_update_filename` (current platform + version-difference matrix) |
| `test_string_utils` | 21 | RC4 chat cipher round-trip (ASCII, UTF-8, empty, determinism) · HF URL rewrite (`/blob/main/` → `/resolve/main/`, already-resolve, too-small-buffer, false-substring) · chat-name sanitisation (punctuation, space runs, trim, UTF-8 passthrough, 40-char cap) · **`strip_tool_fences`** (read_file / list_dir / apply_edit elided, plain code fences preserved, unclosed-fence tail dropped, inline backticks kept, multi-fence chains) |

## Build Notes

- `nuklear_impl.c` is the **single compilation unit** for Nuklear implementation.
- `nuklear_sdl_gl2.h` contains the SDL2 / OpenGL2 backend (both decls + impl, guarded by `NK_SDL_GL2_IMPLEMENTATION`).
- `src/font_dejavu.h` is generated by `xxd -i` from DejaVuSansMono.ttf. Regenerate with: `xxd -i DejaVuSansMono.ttf | sed 's/unsigned char .*/static const unsigned char wst_dejavu_ttf[]/; s/unsigned int .*/static const unsigned int wst_dejavu_ttf_len/' > src/font_dejavu.h`.
- llama.cpp lives in `third_party/llama.cpp/` (with a `vendor/llama.cpp` symlink) and is added as a CMake subdirectory with `LLAMA_BUILD_EXAMPLES=OFF`.
- `seccomp` is optional (Linux only); CMake skips it gracefully on other platforms.
- The application does **not** auto-load any model on boot.
- Package version: **0.6** (`CPACK_PACKAGE_VERSION` in `CMakeLists.txt`).
- MSVC: `nuklear_impl.c` is compiled with `/wd4701`; `ui.c` with `/wd5287`; `_CRT_SECURE_NO_WARNINGS` applied globally for MSVC builds.
- Linux `.deb` architecture is detected at CMake configure time from `CMAKE_SYSTEM_PROCESSOR` (`x86_64` → `amd64`, `aarch64` → `arm64`). CI builds both via `ubuntu-22.04` and `ubuntu-22.04-arm` runners.

## Style

- Pure C (C11), no C++ in application code.
- `snprintf()` for all string building.
- `pthread_mutex_lock/unlock` pairs always kept tight and symmetric.
- No globals except backend state in `nuklear_sdl_gl2.h`.
- Platform-specific code guarded by `#ifdef _WIN32`, `#ifdef __linux__`, etc.
- `volatile int` for cross-thread one-way flags (`running`, `cancel_generation`, `loading`, `load_result`); mutexes for everything else.
