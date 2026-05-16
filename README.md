# Wasteland Terminal v0.8

![Wasteland Terminal](assets/icon-512.png)

A highly secure, offline-first LLM chat application with a retro-futuristic TUI-like graphical interface.

## Overview

Wasteland is a local LLM inference client built in pure C with a vintage PC-inspired amber-on-black CRT terminal aesthetic. It runs entirely offline once a model is loaded, and uses Linux seccomp (where available) to physically prevent the process from opening any new IP socket for the rest of its lifetime.

## Features

- **Offline-first** — All inference happens locally via llama.cpp
- **Hard network lockdown** — Linux seccomp kills the process if it ever opens a new `AF_INET` / `AF_INET6` / `AF_PACKET` socket once a model is loaded (no-op on macOS/Windows)
- **Retro UI** — Amber (`#FFB000`) monochrome CRT terminal aesthetic via Nuklear; property widgets, scrollbars, and all controls share the same palette
- **Threaded** — Non-blocking UI at 60 FPS with background inference, async model loading, and a detached download thread
- **Async model load** — UI stays responsive during multi-second GGUF load
- **Model Management** — Download from HuggingFace, load / **unload** / delete local `.gguf` files; the local vault is sorted lexicographically for stable ordering
- **Stop generation** — `■` button cancels an in-flight response within one token; also interrupts the prompt prefill phase (no more waiting through a long context decode)
- **Chat template** — Uses each model's built-in template (`llama_chat_apply_template`) so instruction-tuned models behave correctly
- **Multi-turn memory** — Full conversation history is fed to the model on every turn so it remembers previous exchanges; context is managed automatically
- **Context management** — CTX bar shows live token usage. `[ COMPACT ]` runs an async **summarisation pass**: the model condenses everything older than the last 2 turns into a 3–6 sentence context note, the chat is rewritten as `[SUMMARY]...[/SUMMARY]\n` + the kept tail, and the worker injects the summary into the system prompt on the next turn. Each subsequent compact re-summarises (existing summary + newly-aged turns), so the prefix stays roughly constant length no matter how long the conversation runs. Auto-compact still triggers when usage exceeds 80 % after a generation; pre-send compact triggers when usage exceeds 75 %.
- **Configurable inference settings** — N\_CTX (512–262 144) and Temperature (0.01–5.0) are adjustable from the left panel and persist across sessions; N\_CTX takes effect on the next model load, Temperature applies from the next prompt
- **Repetition-penalty sampler** — Replaces the old greedy sampler with a stacked penalties → top\_k → top\_p → temperature → distribution chain that prevents small models from looping into repeated paragraphs
- **Multiple Chats** — Create, load, and switch between named chat sessions; auto-named from the first user message and then refined by the model into a contextual 3–5 word title (≤ 40 chars, language-matched, the title prompt explicitly tells the model the budget); persisted to `chats/*.txt` with authenticated XChaCha20-Poly1305 encryption
- **Built-in behaviour rules** — A base system prompt is always active: instructs the model to output plain text (no markdown), be concise, match the user's language, and understand it is running offline. The user-configurable system prompt is appended on top
- **System Prompt** — Configure and persist an additional system prompt to further guide model behaviour (`system_prompt.txt`)
- **Smart Reasoning** — `<think>` reasoning blocks are displayed dimmed in the UI (with a "▒ thinking" label) and automatically excluded from the `◈` copy-to-clipboard text; each turn is rendered in its own box so user prompts never appear inside assistant reply blocks; false-positive detection (e.g. `` `<think>` `` in prose) is suppressed via line-start guard
- **Auto-scroll + word wrap** — Chat pins to the bottom and wraps long lines to the panel width
- **Download Progress** — Real-time progress bar with filename, percent, and **cancel** support
- **Fast close** — Clicking X hides the window instantly, signals the worker via `inference_request_stop()`, joins with a 1.5 s timeout, and falls back to `_Exit` if the worker is still mid-decode
- **Auto-update** — On startup the app queries GitHub Releases in the background; if a newer version exists an orange banner appears under the header. One click downloads the platform artefact, a second click installs it and restarts:
  - **Linux** — `pkexec dpkg -i` (with `gksudo` / `kdesu` fallback). On non-Debian distros where `dpkg` is unavailable a `notify-send` / `zenity` notification surfaces the manual install command; the `.deb` is kept on disk for the user to install by hand.
  - **macOS** — `hdiutil attach` mounts the `.dmg`, `cp -R Wasteland.app /Applications/`, with `osascript with administrator privileges` for system locations; relaunches via `open`.
  - **Windows** — `copy /Y` over the running `.exe` once the parent process exits, with PowerShell `runas` elevation for Program Files paths.
- **Unicode & HiDPI** — DejaVu Sans Mono is embedded in the binary (no external font files needed); covers Basic Latin, Cyrillic (Ukrainian), and Geometric Shapes (▶ ■). Font scales automatically with display DPI so text is never tiny on Retina or Windows HiDPI displays
- **Cross-Platform** — Linux, macOS, Windows (MinGW/MSVC)

## Tech Stack

- **Language:** Pure C (C11)
- **Inference:** llama.cpp (C API, current — not deprecated aliases)
- **GUI:** Nuklear (immediate-mode) + SDL2 + OpenGL 2.1
- **Networking:** libcurl (download path only; isolated in `network.c`)
- **Security:** Linux seccomp-bpf — `SCMP_ACT_KILL_PROCESS` on `socket(AF_INET|AF_INET6|AF_PACKET, …)`
- **Build:** CMake 3.16+

## Building

### Linux

```bash
# Install deps (Debian/Ubuntu)
sudo apt install cmake libsdl2-dev libcurl4-openssl-dev libseccomp-dev

# Install deps (Arch)
sudo pacman -S cmake sdl2 curl libseccomp

# Build
./build.sh
```

### macOS

```bash
# Install deps (Homebrew)
brew install cmake sdl2 curl

# Build
git submodule update --init --recursive
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

### Windows (MSYS2 / MinGW)

```bash
# Install deps
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2 mingw-w64-x86_64-curl

# Build
git submodule update --init --recursive
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make -j$(nproc)
```

### Windows (MSVC)

```powershell
# Install deps via vcpkg
vcpkg install sdl2:x64-windows curl:x64-windows

# Build
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build . --config Release
```

## Cross-Compilation

### Windows exe from Linux

```bash
sudo apt install mingw-w64 cmake
mkdir build-win && cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake
cmake --build . -j$(nproc)
# Produces: Wasteland.exe
```

See `CROSS_COMPILE.md` for full details including MXE dependency setup.

### macOS dmg from Linux

**Not recommended.** Apple requires macOS SDK and code signing. Use GitHub Actions or build on a real Mac.

## Prebuilt Binaries

GitHub Actions automatically builds and releases for all platforms on every tag:

| Platform | Artifact |
|----------|----------|
| Linux x86\_64 (Ubuntu/Debian) | `wasteland_0.8_amd64.deb` — install with `sudo apt install ./wasteland_0.8_amd64.deb` |
| Linux ARM64 (Raspberry Pi 5, Ampere, etc.) | `wasteland_0.8_arm64.deb` — install with `sudo apt install ./wasteland_0.8_arm64.deb` |
| macOS (universal) | `Wasteland-macos.dmg` — one .app that runs natively on both Apple Silicon and Intel (deployment target 11.0+) |
| Windows | `Wasteland-windows.exe` — single self-contained binary (SDL2/curl statically linked) |

Push a tag to trigger a release:

```bash
git tag v0.8
git push origin v0.8
```

## Running

```bash
cd build
./Wasteland        # Linux / macOS
Wasteland.exe      # Windows
```

Place `.gguf` model files in the `models/` directory, or download them via the built-in HuggingFace panel. The application **does not** auto-load anything on boot — you pick the model explicitly so the network stays available for downloads until you commit.

## Project Structure

```
Wasteland/
├── CMakeLists.txt          # Cross-platform build configuration
├── build.sh                # One-shot build script (Linux auto-detect)
├── README.md               # This file
├── CLAUDE.md               # AI assistant context
├── AGENTS.md               # Agent conventions & style guide
├── SKILLS.md               # Domain skill reference
├── CROSS_COMPILE.md        # Cross-compilation guide
├── .gitignore              # Ignore build artifacts & models
├── cmake/
│   └── mingw-w64-x86_64.cmake  # MinGW toolchain for Windows builds
├── .github/
│   └── workflows/
│       └── build.yml       # CI/CD: builds Linux/macOS/Windows + releases
├── src/
│   ├── main.c              # Entry point, SDL loop, thread spawn, fast-shutdown, settings persistence
│   ├── ui.c / ui.h         # Nuklear layout, full amber theme, per-turn chat boxes, compact pipeline
│   ├── inference.c / .h    # llama.cpp wrapper, async load, worker thread, <think> filter, sampler stack, tunables
│   ├── network.c / .h      # libcurl downloader & seccomp lockdown
│   ├── agent.c / agent.h       # Tool-using ReAct agent loop (read_file, list_dir, write_file, apply_edit)
│   ├── agent_executor_main.c   # Sandboxed agent tool subprocess (Linux IPC)
│   ├── agent_protocol.c / .h   # Framed IPC protocol between main process and executor
│   ├── fs_sandbox.c / .h       # Filesystem sandbox (openat / O_NOFOLLOW on Linux, realpath fallback)
│   ├── platform_sandbox.c / .h # Platform sandbox query + apply (seccomp-bpf + Landlock on Linux)
│   ├── crypto_engine.c / .h    # XChaCha20-Poly1305 + SHA-256 wrappers
│   ├── verify.c / .h           # Background SHA-256 model verification
│   ├── capability.c / .h       # Agent capability preset definitions
│   ├── nuklear_impl.c      # Nuklear + SDL/GL2 backend impl
│   └── nuklear_sdl_gl2.h   # Nuklear SDL2/OpenGL2 backend
├── tests/
│   ├── test_framework.h     # Minimal assertion macros (no external deps)
│   ├── test_agent.c         # Tool-call parser, sandbox, real executor round-trips
│   ├── test_chat_history.c  # History parser (LF / CRLF / UTF-8) + system-prompt builder
│   ├── test_version.c       # Semver comparison + updater filename matrix
│       └── test_string_utils.c  # Authenticated cipher, SHA-256, HF URL rewrite, chat-name sanitisation
├── include/                # nuklear.h
├── third_party/
│   └── llama.cpp/          # Git submodule (vendored llama.cpp)
├── vendor/
│   ├── llama.cpp -> ../third_party/llama.cpp  # symlink the CMake build uses
│   ├── monocypher/         # XChaCha20-Poly1305 (embedded, no external dep)
│   └── sha256/             # Incremental SHA-256 for model verification
└── models/                 # Local .gguf storage (gitignored)
```

## Testing

Wasteland ships with a minimal C test framework (`tests/test_framework.h`) — zero external dependencies, only standard macros and `stdio.h`.

```bash
cd build
ctest --output-on-failure
```

Or run individual test binaries directly:

```bash
./test_agent         # agent_parse_calls + agent_resolve_path + tool executors
./test_chat_history  # parse_chat_history + build_system_prompt
./test_version       # version_newer_than + build_update_filename
./test_string_utils  # Authenticated cipher + SHA-256 + HF URL rewrite + chat-name sanitisation
```

Tests are compiled automatically by CMake when you configure the project. To add a new test suite:

1. Create `tests/test_<module>.c`.
2. Include `test_framework.h` and define `void run_<name>(void)` that calls `RUN_TEST(...)` for each case.
3. End with `TEST_MAIN(<name>)`.
4. Register it in `CMakeLists.txt` via `add_executable(test_<name> ...) + add_test(...)`.

### Current coverage

Four suites, **98 tests** total — all green on Linux / macOS / Windows CI runners. Filesystem-touching cases use `/tmp` scratch directories created at suite startup.

| Suite | What it tests |
|---|---|
| `test_agent` (35) | Tool-call markdown parsing (`read_file`, `list_dir`, `write_file`, `apply_edit`) including malformed `apply_edit`, multi-line SEARCH/REPLACE blocks, empty `write_file` body, non-tool fences, inline backticks · sandbox path resolution (escape attempts, absolute paths, new files, `./` prefix) · **executor round-trips:** real read / write / apply_edit / list_dir against a scratch workspace via IPC or in-process fs_sandbox, ambiguous-match refusal, delete-via-empty-replace, escape-blocked attempts · `agent_system_prompt` describes every tool |
| `test_chat_history` (24) | Flat `> prompt\nreply\n` → user/assistant message splitting · trailing-user discard · max-msg cap · CRLF (Windows) line-ending normalisation · UTF-8 (Cyrillic) round-trip · `> ` inside an assistant reply does NOT split a turn · **multi-line user prompts** (consecutive `> ` lines glued back into one user message) · NULL user-prompt for `build_system_prompt` · base-then-user concatenation order · **`extract_summary_prefix`** (no-prefix passthrough, basic `[SUMMARY]...[/SUMMARY]` round-trip, multiline body with numbers/paths preserved, malformed missing-close fallback, in-body `[/SUMMARY]` ignored when not at line start, fixed buffer truncates body but still returns correct skip offset) |
| `test_version` (15) | Semver comparison (`X.Y.Z` with optional `v` prefix) including release-tag-vs-runtime, two-component versions, multi-digit minors (`0.10` > `0.9`), empty / garbage-prefix inputs · platform-specific update-filename generation · version-different filenames differ on Linux but not on macOS / Windows |
| `test_string_utils` (24) | XChaCha20-Poly1305 chat cipher round-trip (ASCII, UTF-8, empty buffer, auth failure) · SHA-256 incremental and single-shot · HuggingFace `/blob/main/` → `/resolve/main/` URL rewrite (basic, already-resolve, too-small-buffer, false-substring matches) · chat-name sanitisation (punctuation strip, space-run collapse, leading/trailing trim, UTF-8 passthrough, 40-char cap) · **`strip_tool_fences`** (read_file / list_dir / apply_edit elided, plain `` ```c `` / `` ```json `` preserved, unclosed-fence tail dropped, inline backticks kept, multi-fence chains) |

## UI Guide

### Left Panel

- **Hub Models** — 11 predefined HuggingFace repos with radio buttons, sorted by parameter count. Each entry shows a one-line dim description under the radio so the trade-offs are visible before download:
  - `Qwen/Qwen2.5-0.5B-Instruct-GGUF` — tiny, smoke-test on weak hardware
  - `ggml-org/gemma-3-1b-it-GGUF` — 1B Google Gemma 3, ~0.8 GB Q4
  - `Qwen/Qwen2.5-1.5B-Instruct-GGUF` — improved tiny tier, ~1.0 GB Q4
  - `ggml-org/SmolLM2-1.7B-Instruct-GGUF` — HuggingFace SmolLM2 small generalist
  - `bartowski/google_gemma-4-E4B-it-GGUF` — compact Gemma 4 expert variant, ~2-3 GB
  - **`Qwen/Qwen2.5-7B-Instruct-GGUF`** — recommended default, ~4.4 GB Q4_K_M, the smallest entry that gives usable conversation quality
  - `bartowski/Qwen2.5.1-Coder-7B-Instruct-GGUF` — code specialist (Python / JS / C++)
  - `bartowski/OLMo-2-1124-7B-Instruct-GGUF` — Allen AI fully-open weights + training data
  - `bartowski/Meta-Llama-3.1-8B-Instruct-GGUF` — Meta Llama 3.1, strong general chat, ~4.9 GB
  - `bartowski/google_gemma-4-31B-it-GGUF` — flagship dense Gemma 4, needs 32+ GB RAM
  - `unsloth/Qwen3.6-35B-A3B-GGUF` — 35B MoE with only 3B active per token; laptop-friendly despite the ~18 GB on-disk size
  
  The list is defined in `ui.c` (struct `hub_model_t { repo_id, description }`) and resolved live via the HF API.
- **Custom ID or URL** — Enter any HF repo ID or full `/blob/main/` URL (the downloader auto-rewrites `/blob/main/` → `/resolve/main/`)
- **Target** — Shows resolved download target before clicking `[ DOWNLOAD ]`
- **Progress** — Filename + percent during download, with `[ CANCEL ]` button
- **Local Vault** — List of `.gguf` files with size:
  - `[ LOAD: name | size ]` — start async load (UI stays responsive)
  - `[ LOADING: name | size ... ]` — in flight; other LOAD/DELETE buttons are disabled
  - `[ UNLOAD: name | size ]` — currently loaded model; click to free it
  - `[ ✓ ]` — SHA-256 verify the file in a background thread
  - `[ DELETE ]` — remove the file from disk (disabled while a load is in flight or generation is running)
  - `[ REFRESH ]` — re-scan `models/`
- **Inference Settings** — Controls visible at all times; values persist in `wasteland.cfg`. Each control pairs a **drag-able slider** (left, fast coarse adjust) with a numeric **property widget** (right, click-to-step / type-exact-value). Both bind to the same field, so you can swipe across the full range in one motion or land on `4096` exactly:
  - **N\_CTX** (512–262 144, step 1024) — context window size; change takes effect on the next model load
  - **TEMP** (0.01–5.0, step 0.05) — sampling temperature; change takes effect immediately on the next prompt
- **System Prompt** — Multi-line input for system instructions, saved between sessions
- **Agent Mode** — Toggle tool-using ReAct loop with **capability presets** rendered as a 4-button selector (`OFF` / `READ` / `RW` / `CUST`):
  - `OFF` — agent ignores all tool calls
  - `READ` — only `read_file` and `list_dir` (auto-approved, read-only)
  - `RW` (default for new installs) — all four tools, `write_file` and `apply_edit` go through the approval gate
  - `CUST` — exposes per-tool checkboxes (`read_file`, `list_dir`, `write_file`, `apply_edit`)
  - The selected preset renders amber-filled with black text so it stays legible against the active background; non-selected ones use the standard amber-on-black theme
  - Persisted to `wasteland.cfg` as `capability_preset` + `capability_custom_bits`. Set a **workspace directory** below the preset row to scope sandboxed file access
- **Chats** — Manage multiple persistent chat sessions:
  - `[ NEW CHAT ]` — Reset to an empty buffer. The chat is **created lazily on the first message**, named from the prompt itself (UTF-8-safe, word-boundary truncation at 60 bytes), then refined into a contextual 3–5 word model-generated title after the first assistant reply. If you click `[ NEW CHAT ]` and then switch to another chat without typing, nothing is created — no orphan "New Chat" files.
  - `[ LOAD ]` / `[ ACTIVE ]` — Switch between chat sessions
  - `[ DEL ]` — Delete a chat session
- **Top-bar status row** — three labels along the header: `SYS: ONLINE` (always), `SEC: UNLOCKED` → `SEC: LOCKDOWN ACTIVE` once a model is loaded, and `NET: AVAILABLE` → `NET: DISCONNECTED` after the seccomp lockdown
- **Sandbox status indicator** — Persistent green/amber/red badge below the local vault showing available platform sandbox capabilities (seccomp, Landlock, process isolation). Text reads `Full Sandbox`, `Partial Sandbox — Network locked, FS open`, `Partial Sandbox — Process isolated, FS unconfined`, or `No Sandbox` depending on the runtime probe
- **Update banner** — If a newer release exists on GitHub, an orange banner appears under the app header with the available version. The check runs once at startup in a background thread before any network lockdown.

### Right Panel (Collapsible)

- **Chat history** — Scrollable, word-wrapped, auto-pins to the bottom on new tokens.
  - Each user/assistant exchange is rendered in its own edit box — user prompts never bleed into the previous assistant reply.
  - Reasoning blocks (`<think>`) are rendered in a separate dimmed box with a "▒ thinking" label.
  - Empty think boxes (e.g. from `<think></think>` or a cancelled mid-think generation) are suppressed.
  - In agent mode, the model's tool-call fences (`` ```read_file ``, `` ```list_dir ``, `` ```write_file ``, `` ```apply_edit ``) are **elided from the rendered view** — the `[ TOOL: name | path ]` marker emitted by the worker is the visible cue. The on-disk chat file keeps the raw fences so the agent can re-parse them on subsequent turns; this is purely a rendering transform.
- **Agent proposal panel** — when Agent Mode is on and the model emits a `write_file` or `apply_edit` tool call, a top-of-panel preview appears with a red/green diff palette matching [docs/index.html](docs/index.html):
  - SEARCH block — orange-red border + text (`#FF6020`)
  - REPLACE / `write_file` body — yellow-green border + text (`#AACC00`)
  - `[ APPLY ]` button — green; `[ REJECT ]` button — red
  - Worker is paused on the approval gate until you click one. The diff text is selectable so you can copy it for review.
- **CTX bar** — `CTX: used / max (pct%)` with a progress bar. Turns orange above 75 %, red above 90 %.
- **`[ COMPACT ]`** — Async **summarisation pass**. Sends every turn older than the last 2 to the model with a compression prompt (3–6 sentences, ≤ 600 chars, language-matched, preserves names/numbers/paths/decisions/open-questions). Shows `Compacting N older turn(s) into a summary...` while the pass runs. When the summary lands, history is rewritten as `[SUMMARY]\n<text>\n[/SUMMARY]\n` + the kept tail; the worker prepends the summary to the system prompt on the next turn so the model still has older context, just compressed. The send button is gated while a compact is in flight to avoid racing the worker. Disabled during generation; refuses if there are fewer than 3 turns or no model is loaded. Each subsequent compact re-summarises (existing summary + newly-aged turns) so the prefix doesn't grow unbounded.
- **Input** — `>` prompt with a **multi-line edit box**. Pasting a multi-paragraph clipboard works as expected — newlines are preserved and the box scrolls internally for long content. Multi-line prompts are reconstructed on the next turn so the model sees the original line breaks.
- **`⤡` (Expand)** — toggles the input box between its default ~34 px height and "fills most of the right panel" mode for composing long prompts comfortably. The chat view shrinks to a 60 px sliver while expanded so the Send button and CTX bar stay visible.
- **`▶` (Play)** — submit the prompt. Press Enter inside the input box to insert a newline; click ▶ (or its ■ replacement during generation) to actually send.
- **`■` (Stop)** — replaces Play while the model is generating; cancels the current response
- **Status message** — temporary non-intrusive notifications appear beneath the input box

## Security Model

1. App boots in **offline-capable** state with the network reachable so the user can download models.
2. **Nothing is loaded automatically.** The user picks a model via the UI.
3. When the user clicks `[ LOAD ]` and the load succeeds, `lockdown_network()` runs:
   - **Linux:** installs a seccomp-bpf filter that kills the process if it ever calls `socket(AF_INET, …)`, `socket(AF_INET6, …)`, or `socket(AF_PACKET, …)`. The filter only gates **new** socket creation — already-open file descriptors (notably the X11 / Wayland Unix-domain socket the GUI uses every frame) keep working.
   - **macOS / Windows:** no-op (platform limitation), network remains available.
4. seccomp filters cannot be removed for the lifetime of the process. Unloading a model does not lift the lockdown — restart to download more models.
5. The download path lives entirely in `network.c` and is gated by `state->network_lockdown` in the UI, so the `[ DOWNLOAD ]` button hides the moment the lockdown is active.

## License

See LICENSE file.
