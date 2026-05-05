# SKILLS.md — Wasteland v0.6

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
- **`nk_label_colored_wrap` does NOT compose with fixed-height parent groups.** The wrap label allocates its own layout rows via `nk_panel_alloc_row`, which extend past the group's reserved height and get clipped — visible result is an empty bordered box (the rectangle renders, the text doesn't). For diff-style rendering inside a fixed-height container, use `nk_edit_string(NK_EDIT_BOX | NK_EDIT_MULTILINE, …)` writing into a buffer that is repopulated each frame from the source-of-truth pointers (read-only feel without `NK_EDIT_READ_ONLY`, which would break mouse selection / copy). This is the canonical pattern for the chat history panel and the agent-proposal SEARCH/REPLACE blocks.
- **Per-widget colour tinting via snapshot-and-restore:** to colour a single button or edit box differently from the global theme, copy the relevant `nk->style.button` / `nk->style.edit` struct into a local, mutate the copy's `border_color` / `text_normal` / `text_hover` / `text_active` / `cursor_normal` / `hover` / `active` fields, render, then assign the saved copy back. Don't touch fields one-by-one without the restore — leaks to the next widget render. The agent-proposal panel uses this for both the diff blocks (red SEARCH / green REPLACE edit boxes) and the action buttons (green APPLY / red REJECT).
- **Multi-line text input — `NK_EDIT_BOX | NK_EDIT_MULTILINE` without `NK_EDIT_SIG_ENTER`.** A multi-line edit box where Enter is supposed to insert a newline (not commit) needs the `NK_EDIT_BOX | NK_EDIT_MULTILINE` flag pair without `NK_EDIT_SIG_ENTER`. The `SIG_ENTER` flag makes Enter both insert a newline AND raise `NK_EDIT_COMMITED`, so any commit-on-enter path defeats the multi-line composition use case. Submit policy is then button-only — pair the edit box with a `▶` glyph button in the same row.
- **Slider+property pairing for big-range numeric controls.** A property widget alone (`nk_property_int / nk_property_float`) requires hundreds of `+` clicks to traverse a 4 K → 256 K range. A slider alone (`nk_slider_int / nk_slider_float`) lacks precision for typing exact values. Pair both in the same row, bound to the same field — slider on the left for fast coarse drag, property on the right for click-to-step / type-exact-value. The two stay in sync because they share the underlying integer/float pointer; whichever the user touches last wins.
- **Two-mode panel-height split via a single `int` toggle.** When a UI region needs an "expand to fill the rest of the panel" mode, the cleanest pattern is a single `int state->foo_expanded` flag plus a single height formula `target_h = total - reserved - other_h - dynamic_offset` that covers both modes — set `other_h` to the small/big value depending on the toggle, then floor `target_h` to a minimum so the expanded mode still shows context. Avoid two-branch height code — invariably one branch drifts off-by-N pixels when constants change.
- **Pick toggle glyphs from a guaranteed-baked Unicode range.** Embedded fonts (especially via `xxd -i` + a hand-curated `nk_rune ranges[]`) typically cover Basic Latin + Latin-1 + a regional script + Geometric Shapes (`U+25A0..U+25FF`). Anything else — Arrows (`U+2190..U+21FF`), Supplemental Arrows (`U+2900..U+297F`), Mathematical Operators — silently renders as `?` boxes. Default to Geometric Shapes (`▲ ▼ ◀ ▶ ■ ◆ ◊ ▦`) for icon buttons; if you need a richer glyph set, extend the font ranges and pay the atlas-size cost (~1 KB per 64 baked glyphs).
- Word wrap with measured row heights using the active font's `width()` callback (`wrap_text_into()`).
- **Embedded font pattern:** `xxd -i font.ttf | sed 's/.../static const unsigned char name[]/' > src/font.h` bakes a TTF into a C byte array. Use `nk_font_atlas_add_from_memory(atlas, data, len, size, cfg)` to load it — no file path required, works identically on Linux/macOS/Windows.
- **DPI-aware font scaling:** after window creation, compute `dpi_scale = SDL_GL_GetDrawableSize(w) / SDL_GetWindowSize(w)`. Multiply base font size (15 px) by this ratio and pass to the font-baking call. On macOS Retina (2×) and Windows HiDPI (1.25–2×) this prevents text from appearing physically tiny.
- **Windows DPI fallback:** when `SDL_GL_GetDrawableSize` returns the same value as `SDL_GetWindowSize` (observed on some Windows configs with per-monitor DPI awareness), fall back to `SDL_GetDisplayDPI / 96.0f` so text stays readable at 125–200 % scaling.
- **Font bake retry:** if `nk_font_atlas_bake` fails (usually because 2×2 oversampling produces a texture too large for the GPU at high DPI), clear the atlas and retry with 1×1 oversampling. This is the canonical fix for missing Cyrillic / Geometric Shapes on HiDPI displays.
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
- **Auto-generated chat titles:** after the first assistant reply in a newly-created chat, run a short secondary inference pass (max ~20 tokens) with a dedicated title-generation prompt. Use a `title_mode` flag to redirect output into a separate `title_buffer` so the title never appears in chat. Sanitise the result (strip newlines, quotes, markdown, `<>`) and rename the chat file on disk. The UI polls `inference_take_title()` each frame after generation ends.

### Filename Generation from User Input

- **Word-boundary cuts on byte caps:** when truncating a free-form string into a filename with a hard byte limit, scan back to the last space within the trailing third of the buffer rather than cutting mid-word. The "trailing third" floor avoids producing a one-or-two-word stub when the user wrote a long sentence with the first space far from the cap.
- **UTF-8 tail safety after byte cap:** any cap measured in bytes can split a multi-byte codepoint. Always run a tail pass that (a) drops continuation bytes (`(b & 0xC0) == 0x80`) and (b) drops a lead byte without its full sequence (compute expected length from the top bits: `0xC0`→2, `0xE0`→3, `0xF0`→4). Without this, a 2-byte Cyrillic char halved by the cap renders as `?` and breaks case-insensitive filesystems on normalisation.
- **Lazy chat creation:** the `[ NEW CHAT ]` button does not create a file on disk; it just resets the buffer and sets the "no active chat" sentinel. The submit handler creates the chat with a prompt-derived name on the first message, so there is no permanent default name. Eager creation produced ugly `New Chat` / `New Chat_2` filenames when the auto-rename branch never fired.
- **View-only transforms vs. persistent edits:** when the model output contains repetitive markup (e.g. agent tool fences with separate `[ TOOL: ... ]` markers), strip it from the rendered view per-frame, not from the underlying buffer — agent re-parsing on the next turn must still see the original fences. Drive this by copying `state->chat_history` → `view_buf` and applying the transform on the copy.

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
- **GitHub Releases API:** `api.github.com/repos/{owner}/{repo}/releases/latest` returns JSON with `tag_name`. Parse via simple text scan (`strstr(..., "\"tag_name\":\"v")`) rather than pulling in a JSON library. Strip the leading `v` to get the semver string.
- **Auto-update thread pattern:** spawn a detached pthread at startup (before any network lockdown) with a 5-second curl timeout. Write the result into a `char update_version[32]` field; the UI polls it every frame and renders an orange banner if non-empty.
- **Self-replace via generated script (the in-place upgrade pattern):** the running process can't overwrite its own binary on Windows (sharing violation) and shouldn't on POSIX (mid-decode threads). Solution: write a tiny shell/batch file to `/tmp` / `%TEMP%`, fork+exec it detached, then exit. The script polls `kill -0 $PID` / `tasklist /FI` until the parent dies, then performs the install + relaunches the new binary + self-deletes. No external supervisor needed.
- **Package format vs binary format:** the install method depends on what the platform artefact actually is. macOS `.dmg` needs `hdiutil attach` + `cp -R Wasteland.app`. Linux `.deb` is an `ar` archive (NOT an ELF) — it needs `dpkg -i`, never `cp` over the running binary. Windows `.exe` IS the binary, so `copy /Y` over the file works directly. Picking the wrong method (e.g. `cp` on a `.deb`) bricks the install in a way that's hard for the user to recover from.
- **Privilege escalation tools reset CWD.** `pkexec` runs with CWD `/root` (or `$PWD` if exported, but unreliable), `osascript with administrator privileges` runs from `/`, Windows `ShellExecute` starts batch files in `%TEMP%`. Before passing any path to a generated script that will invoke these tools, resolve it to absolute via `realpath()` (POSIX) or `_fullpath()` (Windows) — relative paths silently fail to resolve in the privileged subshell.
- **Linux GUI elevation fallback chain:** PolicyKit's `pkexec` is the modern default (Ubuntu/Debian/Mint/Fedora with desktop), but absent on minimal/server setups and on some Arch installs. Fall back to `gksudo` (deprecated GNOME tool, still around), then `kdesu` (KDE), then a desktop notification (`notify-send` → `zenity` → `xmessage`) telling the user the manual command. Don't error out silently — surface what the user can do instead.
- **Keep the artefact on install failure.** If the download succeeded but the install was rejected (user clicked Cancel on the pkexec prompt, dpkg returned non-zero, no elevation tool available), leave the downloaded `.deb`/`.dmg`/`.exe` on disk so the user can follow the manual-install message. Only `rm -f` after `RC == 0`.
- Linux seccomp-bpf with **argument filtering** (`SCMP_A0(SCMP_CMP_EQ, AF_INET)`) — narrow rules that gate `socket()` creation by address family, leaving X11 / Wayland Unix-domain traffic untouched.
- Why pointer-deref filtering (sockaddr family on `connect`/`bind`) is impossible in seccomp and what to do instead (gate at `socket()`).
- Cross-platform security model: features degrade gracefully on macOS / Windows.

### Prompt Engineering / System Prompts

- **Built-in base system prompt pattern:** define a `static const char BASE_SYSTEM_PROMPT[]` in the inference module and always prepend it to the user-configurable system prompt via a `build_system_prompt()` helper. This guarantees consistent model behaviour (plain-text output, language matching, context awareness) regardless of whether the user has set a custom prompt.
- Key rules for a terminal LLM client: suppress markdown (the UI renders text verbatim), enforce conciseness, and communicate the offline/sandboxed nature of the runtime so the model doesn't hallucinate internet access.
- Agent mode system prompt = base + user sys + tool instructions, concatenated in that order. Buffer size must accommodate all three (~16 KB in Wasteland).

### Unit Testing

- **Zero-dependency framework:** `tests/test_framework.h` provides `ASSERT`, `ASSERT_EQ_INT`, `ASSERT_EQ_STR`, `ASSERT_TRUE`, `ASSERT_FALSE`, `ASSERT_NULL`, `ASSERT_NOT_NULL`, `RUN_TEST`, and `TEST_MAIN` macros. No GoogleTest, no Unity, no external libs.
- **Testable-without-SDL rule:** anything that can be tested without an SDL window or a loaded GGUF model should have a suite. Pure string parsers (chat history splitting, system-prompt concatenation, RC4 cipher, HF URL rewrite, chat-name sanitisation), semver comparison, sandbox path resolution, and filesystem-touching tool executors are all ideal candidates.
- **Two strategies for reaching internals:**
  - **Compile-time `#ifdef TESTING`:** wrap the function signature in `#ifdef TESTING ... #else static ...` to drop the `static` keyword for test builds (used in `parse_chat_history`, `build_system_prompt`, `version_newer_than`, `build_update_filename`).
  - **Hand-copied source duplication:** when the host TU drags SDL / llama.cpp / curl, copy the pure function into the test file with an `origin: src/<file>.c` reference comment (used in `test_chat_history.c`, `test_version.c`, `test_string_utils.c`). Sync the copies whenever the source is edited — the AGENTS.md test-parity rule covers this.
  - **Direct linking:** when the host TU is dependency-free (e.g. `src/agent.c` has no SDL/llama.cpp), include it as a source in the test executable: `add_executable(test_agent tests/test_agent.c src/agent.c)`. This lets the suite call the real executors end-to-end.
- **Filesystem test setup:** suites that exercise `agent_resolve_path` or `agent_exec_*` must create their scratch directories (e.g. `wst_test_ws/`, `wst_test_exec_ws/`) before calling `RUN_TEST`, because `realpath()` requires the parent to exist. Use a separate workspace per concern so failure artefacts can be inspected without polluting other tests.
- **Round-trip pattern for ciphers:** for symmetric streams (RC4), the canonical test is `encrypt + encrypt = identity` plus a `ciphertext != plaintext` sanity check (so a NOP encrypt slipping in still fails). Add a determinism check (two encrypts of the same input produce the same bytes) to catch accidental time-based seeding.
- **Buffer-overflow contract testing:** for in-place transforms with bounds (e.g. the HF URL `+3` rewrite), pass an artificially tight buffer and assert the function refuses (returns -1) rather than truncating or corrupting the buffer.
- **CI integration:** `cmake --build build && ctest --output-on-failure` runs all suites automatically on every push. Currently 81 tests across 4 suites.

### Build Engineering

- CMake target configuration.
- Cross-platform `find_package` / `find_library` handling.
- Git submodule management for vendored llama.cpp.
- Platform-specific linking (SDL2, OpenGL, curl, seccomp on Linux; ws2_32, winmm on Windows).
- MinGW cross-compilation from Linux toolchain files.
- **Per-file MSVC warning suppression:** use `set_source_files_properties(file.c PROPERTIES COMPILE_OPTIONS "/wd<N>")` to suppress a warning from a third-party header included only in that translation unit. Avoids masking the same warning in application code.
- **Dynamic `.deb` architecture:** map `CMAKE_SYSTEM_PROCESSOR` to Debian arch names (`x86_64`→`amd64`, `aarch64`→`arm64`) at configure time so the same `CMakeLists.txt` produces correctly-named packages on both amd64 and ARM64 CI runners without manual overrides.
- **GitHub Actions release pipeline:** matrix builds for `ubuntu-22.04`, `ubuntu-22.04-arm`, `macos-14`, `windows-latest`. A separate `release` job (depending on all builds) downloads all artifacts and creates a single GitHub Release with `softprops/action-gh-release@v2`. Tags are created via a separate `workflow_dispatch` workflow (`tag.yml`).
- **`needs:` declarations gate cross-job output access.** A job that reads `needs.X.outputs.foo` MUST list `X` in its own `needs:` array — otherwise the expression silently evaluates to null. The fallback chain `${{ A || B || C }}` then walks past your intended value to whatever's last in the chain (`github.ref_name` is a common foot-gun: on a branch push it's the *branch name*, which softprops-style actions will happily create a tag from). Resolve cross-job values in a dedicated step with explicit priority + a sanity-regex; fail loudly when nothing matches instead of falling through to a branch name.
- **`GITHUB_TOKEN` cannot trigger recursive workflow runs.** A tag pushed via the auto-generated `${{ secrets.GITHUB_TOKEN }}` does NOT fire a `push: tags:` workflow run — GitHub blocks this to prevent infinite loops. Consequences: a `create-tag` job in the same workflow as `build`/`release` must publish in the same run; you can't refactor to "wait for the tag-push event to come back around". To unlock recursive triggering, use a Personal Access Token (PAT) stored as a secret — but that's an extra credential to rotate.
- **Release-on-version-bump pattern.** Use the source-of-truth file (here `src/ui.h::WASTELAND_VERSION`) as the version oracle: an `extract-version` job reads it, computes `tag=v${VER}`, AND probes `git ls-remote --tags origin` to set a `tag_exists` output. The release job consumes both — version goes into `tag_name`, `tag_exists` becomes a gate so cosmetic pushes (README fixes, doc tweaks) skip the release path and don't overwrite published artefacts or hand-edited release notes. Bumping the version file is the single ceremony that publishes a release.

## Version

Current version: **0.6**
