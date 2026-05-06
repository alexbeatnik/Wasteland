# AGENTS.md — Wasteland v0.7

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
- The `hub_models[]` array contains `hub_model_t { repo_id, description }` structs. Each `repo_id` must be a real, public, reachable HF GGUF repo — fictional IDs fail at HF API resolution time with HTTP 404. When adding a new entry, click DOWNLOAD on it once before merging. Always include at least one mid-tier (~7-8B) model — leaving only sub-2B tinies makes the app feel dumb on first use even though the issue is the model. Descriptions are one-line ≤ 64 chars and use ASCII-only characters (`·` is fine, but no emoji); they render in dim amber under each radio button. Current set: `Qwen/Qwen2.5-0.5B-Instruct-GGUF`, `ggml-org/gemma-3-1b-it-GGUF`, `Qwen/Qwen2.5-1.5B-Instruct-GGUF`, `ggml-org/SmolLM2-1.7B-Instruct-GGUF`, `bartowski/google_gemma-4-E4B-it-GGUF`, `Qwen/Qwen2.5-7B-Instruct-GGUF`, `bartowski/Qwen2.5.1-Coder-7B-Instruct-GGUF`, `bartowski/OLMo-2-1124-7B-Instruct-GGUF`, `bartowski/Meta-Llama-3.1-8B-Instruct-GGUF`, `bartowski/google_gemma-4-31B-it-GGUF`, `unsloth/Qwen3.6-35B-A3B-GGUF`.
- The `/blob/main/` → `/resolve/main/` URL rewrite in `network_download_model` must `memmove` the tail right by 3 bytes **before** the `memcpy` overwrite — the obvious order corrupts the first 3 bytes of the filename.

## Platform Sandbox (`src/platform_sandbox.c`)

- `platform_sandbox_query_caps()` returns a bitmask of available capabilities. On Linux this includes `SANDBOX_CAP_NETWORK_LOCKDOWN` (seccomp) and `SANDBOX_CAP_FS_RESTRICT` (Landlock, runtime-probed). On macOS/Windows only `SANDBOX_CAP_PROCESS_ISOLATE` is reported.
- `platform_sandbox_apply()` installs seccomp-bpf network lockdown; `platform_sandbox_restrict_fs_to()` installs Landlock ruleset restricting the process to a single workspace directory. Both are no-ops on non-Linux platforms.
- The UI renders a persistent sandbox status indicator (green/amber/red) driven by `platform_sandbox_query_caps()` — never falsely claim security.

## Filesystem Sandbox (`src/fs_sandbox.c`)

- **Linux:** `openat()`-based traversal with `O_NOFOLLOW` at every step. Explicit `".."` is rejected. After opening, `/proc/self/fd/<n>` is verified via `readlink` to still lie under the workspace root. This mitigates symlink traversal, TOCTOU races, and path-escape attempts.
- **macOS/Windows:** `realpath()` + `path_is_inside()` fallback — honest about weaker guarantees.
- `fs_sandbox.c` is the **only** place where agent tool I/O (`read_file`, `list_dir`, `write_file`, `apply_edit`) touches the filesystem. `agent.c` delegates to it; do not re-add direct `fopen` paths in agent executors.

## Agent Executor Subprocess (`src/agent_executor_main.c`)

- A separate binary `wasteland-agent-executor` is spawned via `posix_spawn` (Linux) and communicates with the main process over a framed IPC protocol (`src/agent_protocol.c`).
- The executor applies `platform_sandbox_apply()` + `platform_sandbox_restrict_fs_to()` **before** entering the service loop, so even if the main process is compromised the filesystem surface is minimised.
- On non-Linux platforms the IPC path is disabled and agent tools fall back to in-process `fs_sandbox.c` calls.
- **Do not** call `pthread_cancel` on the executor — use `AGENT_IPC_TOOL_SHUTDOWN` for graceful termination.

## Auto-Update Rules

- **Linux artefact is a `.deb`, not an ELF.** `launch_updater()` MUST install it with `dpkg -i`, not `cp` — copying a Debian package archive over the running binary bricks the install. The script tries `pkexec` → `gksudo` → `kdesu` for elevation; on failure, it surfaces a `notify-send` / `zenity` / `xmessage` notification with the manual `sudo dpkg -i $NEW` command. The `.deb` is **kept on disk** when install fails so the user can follow that instruction; only `rm -f` it after `RC == 0`.
- **Always pre-resolve `new_file` to an absolute path before writing the script.** `pkexec` resets CWD to `/root`, `osascript with administrator privileges` resets to `/`, Windows `ShellExecute` runs the batch file from `%TEMP%`. None inherit the parent's CWD, so a relative `downloads/foo.deb` will silently fail with "file not found" in the privileged subshell. Use `realpath()` (POSIX) / `_fullpath()` (Windows) at the top of `launch_updater()` — both with a `getcwd()`-based fallback for the case where the file just got deleted.
- **Do not call `pthread_cancel` on the update download thread.** It owns a libcurl handle; libcurl is not async-cancel-safe. Use the `state->update_cancel` flag — `progress_cb` polls it and returns 1, which makes curl abort the transfer cleanly.
- **The startup probe runs BEFORE seccomp lockdown.** Moving `update_check_thread`'s `pthread_create` after model load would silently break the banner — the lockdown SIGKILLs any new `socket(AF_INET, …)`. The probe must complete (or its 5-second curl timeout must fire) before the user clicks `[ LOAD ]`.
- **Banner gating:** `state->update_version[0] != '\0'` is the single source of truth. The probe only writes to it when `version_newer_than(latest, WASTELAND_VERSION)` returns true — so on equal or older releases, no banner. Don't add a "force show banner" toggle for testing; just temporarily lower `WASTELAND_VERSION` in `ui.h` and rebuild.
- **GitHub release `tag_name` parsing is a `strstr` scan, not a JSON library.** Format must stay `vX.Y` or `vX.Y.Z`; the leading `v` is stripped before comparison. If GitHub ever changes the JSON shape, the probe will silently fail (returns -1, no banner) — fine for graceful degradation, but spot-check after any GitHub API breakage.

## CI Release Workflow Rules

- **`release` job MUST list `extract-version` in its `needs:`.** Without it, `needs.extract-version.outputs.version` evaluates to null on a main-branch push, the `||` fallback drops to `github.ref_name` which IS the literal string `main`, and `softprops/action-gh-release` then creates a release AND a tag named `main` at the current commit — silently bypassing the `v0.7` tag that `create-tag` just published.
- **Resolve the release tag in a dedicated step, not inline in `tag_name:`.** A `bash` step with priority `inputs.version → extract-version.outputs.version → github.ref_name (only if ref_type == tag)` plus a `^v[0-9]+\.[0-9]+(\.[0-9]+)?$` regex validation makes the resolution explicit and fails loudly when nothing matches. Don't rewrite this back into a single-line YAML expression — silent fallthrough to a branch name is exactly the bug we're guarding against.
- **`release` is gated by `tag_exists == 'false'` on main-push.** Cosmetic pushes to `main` (README tweak, doc fix, anything that doesn't bump `WASTELAND_VERSION` in `src/ui.h`) must NOT regenerate the published release — that would overwrite the `.deb`/`.dmg`/`.exe` artefacts and wipe hand-edited release notes from the GitHub UI. The `extract-version` job probes `git ls-remote --tags` and writes `tag_exists`; the release job's `if:` consults it. `workflow_dispatch` and direct tag-pushes still publish unconditionally.
- **`GITHUB_TOKEN`-pushed tags do NOT trigger workflow runs.** GitHub safety against recursive events. So the build + release MUST happen in the same workflow run as `create-tag` — don't refactor to "wait for the tag-push event" expecting a second run, because that second run never fires.
- **Bumping `WASTELAND_VERSION` is the single ceremony that publishes a release.** Edit `src/ui.h`, commit, push to `main`. CI extracts the version, creates the tag, builds for 4 targets, publishes the release. No manual tagging, no release-please-style PR. If you also need to change `CPACK_PACKAGE_VERSION` in `CMakeLists.txt` and the hardcoded filenames in `build.yml`, do it in the same commit.

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
- **Agent diff palette is canonical:** `[ APPLY ]` and the REPLACE block must be `#AACC00` (yellow-green); `[ REJECT ]` and the SEARCH block must be `#FF6020` (orange-red); the `▼ AGENT PROPOSAL — REVIEW` heading and `// SEARCH:` / `// REPLACE WITH:` labels are amber `#FFB000`. These four exact RGB values match the `docs/index.html` `agent-proposal` mock — keep them in sync if either side changes. Use the snapshot-and-restore pattern (`saved_btn = nk->style.button; ...; nk->style.button = saved_btn;`) so per-button tinting never leaks into the global amber theme.
- **Active-state buttons must flip text colour.** The amber theme uses `text_normal = amber` against a dark background. When you fill a button background with amber to mark it as "selected" (e.g. the active capability preset), you MUST also set `text_normal` / `text_hover` / `text_active` to black — otherwise it's amber-on-amber and the label disappears. Same pattern as the diff buttons: copy `nk->style.button` into a local, override `normal/hover/active` (background) AND `text_normal/text_hover/text_active`, render, restore. Override all three text states because Nuklear picks `text_active` while the user is mid-click and `text_hover` while the cursor is over the button — leaving any of them at amber gives a one-frame "label vanished" flicker.
- **Diff blocks use `nk_edit_string`, NOT `nk_group + nk_label_colored_wrap`.** The wrap label allocates its own layout rows that don't compose with a fixed-height parent group — text gets clipped past the reserved height, leaving an empty bordered box (the user sees the rectangle, not the content). The proven pattern is `nk_edit_string(NK_EDIT_BOX | NK_EDIT_MULTILINE, …)` writing into a static buffer that's repopulated from the pending-tool pointers each frame. Tint border / text / cursor through a `saved_edit = nk->style.edit; ... ; nk->style.edit = saved_edit;` snapshot-and-restore. Do not switch this to `NK_EDIT_READ_ONLY` — read-only mode disables mouse selection, which kills the user's ability to copy the diff for review.
- **Pending-panel reserved height:** `pending_panel_h` is `230` for `write_file` (heading 18 + title 16 + label 14 + body 130 + buttons 28 + group padding) and `290` for `apply_edit` (one extra label + one extra body). If you grow either block, update the constants — chat area height is computed as `height - 200 - pending_panel_h`, so under-reserving pushes the input field offscreen.
- **Prompt input is multi-line `nk_edit_string(NK_EDIT_BOX | NK_EDIT_MULTILINE)`.** Do NOT pass `NK_EDIT_SIG_ENTER` — that would commit on every Enter press, defeating multi-line composition (Enter must insert a newline). Submit is button-only via the `▶` glyph. Pasting a multi-paragraph clipboard works because the box accepts `\n` natively; long content scrolls within the box.
- **`state->input_expanded` toggle drives chat/input height split.** Collapsed default: `input_h = 100` (≈5 visible rows) — short paragraphs compose without needing to expand, longer content scrolls inside the box. Expanded: `input_h = height - 226 - pending_panel_h` (with `80` px floor), `chat_h` floors to 60 px. Single formula `chat_h = height - 166 - input_h - pending_panel_h` covers both modes. The toggle button uses **U+25B2** (`▲`, expand) / **U+25BC** (`▼`, collapse) from the Geometric Shapes block — confirmed inside the embedded DejaVu Sans Mono atlas range (`0x25A0..0x25FF` in `nk_sdl_unicode_ranges[]`). Do NOT use U+2921/U+2922 (Supplemental Arrows-B) or any other arrow from U+2190..U+21FF — those ranges are NOT baked into the atlas and render as `?` boxes.
- **Multi-line user prompts are stored as consecutive `> ` lines.** When the user submits a prompt containing `\n`, the UI emits `> line1\n> line2\n...` to `chat_history`. The `parse_chat_history` parser glues consecutive `> ` lines back into a single user message with embedded `\n`s so the chat template sees the original line breaks. Don't simplify the UI write path to a single `> %s\n` snprintf — it would dump the raw `\n` into chat_history and break the parser invariant on the next turn.
- **Settings sliders pair with property widgets, not replace them.** N_CTX and TEMP each render as `nk_slider_int/float` (left, ~62 % width, drag for fast coarse adjust) PLUS `nk_property_int/float` (right, ~38 %, click-to-step or type exact value). Both bind to the same `state->settings_*` field. Going from 4 K to 256 K through the property's `+` button alone is ~250 clicks; the slider makes the UI usable for big jumps. Do NOT collapse to slider-only — the property is the only way to type an exact value or land on a power-of-two.

## Chat Creation & Naming

- **`[ NEW CHAT ]` is lazy.** It saves the active chat, clears history, and sets `selected_chat = -1`. It does **not** create a file on disk or append to `state->chats[]`. The submit handler does that on the first prompt, with a name derived from the prompt itself. Don't reintroduce eager creation — it permanently named chats `New Chat` / `New Chat_2` / etc., because the rename branch (`selected_chat == -1`) never fired.
- **`generate_chat_name_from_prompt` byte cap is 60.** Lower (40) produced ugly mid-word cuts on Cyrillic; higher (80+) overflows the left-panel chat label. If you change the cap, also adjust the word-boundary scan window — currently the trailing third of the buffer is the floor for the last-space search.
- **UTF-8 tail trimming is mandatory.** `trim_partial_utf8(base, &b)` runs after the cap and word-boundary pass to drop any incomplete continuation/lead byte. A Cyrillic codepoint cut across the cap stored as raw bytes would render as `?` and could break filesystems on case-insensitive normalisation.
- **Auto-rename to model-generated title** still runs after the first assistant reply (`needs_title = 1` set inside the same submit handler that creates the chat). Cleaning the title (strip `*`, `\`, `<>`, newlines, quotes) happens in the worker before publishing — UI just renames the file.

## Chat View Transforms

- **`strip_tool_fences(raw, out, sz)` is a render-time view, not a mutation.** The on-disk chat file and `state->chat_history` always carry the raw fences (so subsequent agent turns can re-parse them if needed). The strip pass writes into a separate static buffer that is what the chat panel renders. Don't strip into `state->chat_history` directly — you'll erase context the model needs.
- **Recognised tool-fence headers** are exactly the four agent tools: `read_file`, `list_dir`, `write_file`, `apply_edit`. Plain code fences (` ```c `, ` ```json `, ` ```python `, ` ```rust `, …) are deliberately NOT stripped — they're real code the user wants to see. Keep the recogniser list in lockstep with `agent.c::header_to_kind()` and `tests/test_string_utils.c::is_tool_fence_header()`.
- **Mid-stream fences (opening emitted, closing not yet)** elide the entire tail. This is intentional — the user gets the prose lead-in, then the next visible character is the `[ TOOL: ... ]` line once the worker emits it. Don't try to "preview" half-emitted fences; it just churns visible state every token for a payload that's about to disappear.

## Chat History & Context

- `state->chat_history` (main thread) and `ictx->chat_history` (inference mirror, protected by `history_mutex`) are two separate buffers. Call `ui_set_chat_history(state)` to sync them before submitting a prompt or after compacting.
- `parse_chat_history()` in `inference.c` produces user/assistant pairs. A trailing user message with no assistant reply (i.e. the current prompt in mid-submission) is **discarded** — the worker appends the current prompt separately. This prevents duplicating the user turn in the message list.
- **`[SUMMARY]...[/SUMMARY]\n` is a magic prefix** managed by the compact pipeline. It MUST live at byte 0 of `chat_history` (or be absent). Both the inference worker and `inference_get_context_stats` call `extract_summary_prefix()` first to peel it off, then hand the remainder to `parse_chat_history`. Do not invent your own placement (mid-history, between turns, etc.) — the parser does not expect it there. If you add new code that mutates `chat_history` (compact variants, "clear from this turn", import/export, …), preserve the prefix or rebuild it from scratch; never half-strip it (e.g. drop `[SUMMARY]` but leave `[/SUMMARY]`).
- **The summary body is appended to the system message, not injected as a fake turn.** The worker prepends `\n\nPrevious conversation summary (older turns compacted from this chat):\n<text>` to the system message before the user/assistant turns. This means a model that sees `[SUMMARY]...[/SUMMARY]` rendered as raw text in the chat panel is harmless — only the worker's system-message construction makes it semantic.
- **Agent mode includes history:** the agent ReAct branch calls `parse_chat_history` with a cap of `AGENT_HISTORY_SLOTS = 8` messages before appending the current user prompt. This gives the model multi-turn memory in agent mode at the cost of fewer available tool-call slots (still 10 turns, governed by `AGENT_MAX_MSGS = 30`). The summary block is folded into `combined_sys` here too — same protocol.
- **Agent tool output:** `read_file` and `list_dir` results are NOT printed to the chat UI — only the `[ TOOL: name | path ]` header is shown. File contents are still passed to the model via `result_out`. Do not re-add the `emit_raw_str(ictx, result_out)` call — it floods the chat with raw file contents.
- `ui_compact_chat_history(state, n)` is the **only** correct way to call compact from UI code. It is **async** (v0.7+): it snapshots the verbatim tail to keep, calls `inference_request_summary` with the older portion, sets `state->compact_pending = 1`. Don't try to use the result on the same frame — `ui_finalize_compact()` (called from the main loop) splices in the summary once the worker publishes it. The `pairs` argument is now informational; the actual cut keeps `COMPACT_KEEP_RECENT_TURNS` (= 2) turns verbatim.
- **SEND must be gated by `compact_pending`.** If the user clicks SEND while a compact is in flight, the prompt would be submitted with the pre-compact history (because mirroring happens in `ui_finalize_compact`). The send button's enable check includes `!state->compact_pending`; pre-send compact uses `goto skip_send` to drop the SEND that frame so the user re-clicks after the summary lands.
- `inference_get_context_stats()` calls `llama_tokenize()` with `tokens=NULL` / `n_tokens_max=0` to count without allocating. The API returns **negative** count in this case (buffer-too-small convention) — negate it to get the real count. Do not treat negative as failure. It also strips the `[SUMMARY]` prefix and prepends a synthetic `system: Summary: …` message before tokenising, so the CTX bar reflects what the worker will actually feed the model.

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

Runtime tunables (N\_CTX, Temperature, Capability Preset) are stored in `wasteland.cfg` alongside agent settings:

```
agent_mode=0
capability_preset=2
capability_custom_bits=0
agent_workspace=/path/to/ws
n_ctx=8192
temperature=0.800
```

- Load on startup in `main.c`; push to `inference_set_n_ctx` / `inference_set_temperature` immediately.
- Persist on every change (compare to `last_*` shadow variables in the main loop).
- Valid ranges: `n_ctx` 512–262 144, `temperature` 0.01–5.0.
- Capability presets: `0=OFF`, `1=READ_ONLY`, `2=READ_WRITE`, `3=CUSTOM`. When preset is `CUSTOM`, `capability_custom_bits` stores the per-tool bitmask (bits 1..4 correspond to `read_file`, `list_dir`, `write_file`, `apply_edit`).

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
18. All CTest suites pass (`ctest --output-on-failure`) — currently **98 tests** across 4 suites (`agent` 35, `chat_history` 24, `version` 15, `string_utils` 24).
19. New logic must have a matching `tests/test_*.c` suite if it is testable without SDL/llama.cpp (pure string / file / math functions). Filesystem-touching tests use `/tmp`-style scratch directories created in `run_<suite>()` *before* `RUN_TEST` calls.
20. Existing tests must not be broken by the change — run `ctest` before every commit.
21. **Test parity rule:** when a function lives in `src/*.c` and has been copy-extracted into `tests/test_*.c` (e.g. `parse_chat_history`, `version_newer_than`, `crypto_seal` / `crypto_open`, the HF URL rewrite), edits to the production version MUST be mirrored into the test copy. The duplication is intentional — it lets the test binaries link without SDL2 / llama.cpp / curl — but you have to keep both halves in lockstep or the tests start passing the wrong code.

## Font & DPI Rules

- **Never remove `src/font_dejavu.h`** from the repo — it is the only reliable cross-platform source of Cyrillic + symbol glyphs. If you need to regenerate it: `xxd -i DejaVuSansMono.ttf | sed ...` (see CLAUDE.md Build Notes).
- **`nk_sdl_init` signature is `(SDL_Window *win, float font_size)`** — always pass the DPI-scaled size computed in `main.c`. Passing 0 falls back to 15 px.
- **`memmem` on MSVC:** provide a `static` implementation inside `#ifdef _WIN32` at the top of the TU, before any call site. A separate forward declaration triggers C2040 because MSVC already has an implicit `int ()` entry.
- **Windows POSIX macros:** `S_ISDIR` is not defined by MSVC. Add `#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)` inside the `#ifdef _WIN32` include block of any TU that uses it.

## Version

Current version: **0.7**
