# SKILLS.md — Wasteland v0.1

## Available Skills

This project does not use a formal skill system. The following domains are relevant for contributors:

### Systems Programming
- POSIX / Windows threads (`pthreads`, `CreateThread`)
- Mutexes, condition variables, thread-safe queues
- Cross-platform file I/O (`dirent.h` vs `FindFirstFileA`)
- Signal-safe and async-cancel-safe thread design

### Graphics & UI
- Nuklear immediate-mode GUI
- SDL2 windowing and event handling
- OpenGL 2.1 fixed-function pipeline
- Font atlas baking and vertex buffer rendering

### Machine Learning Inference
- llama.cpp C API
- GGUF model format
- Tokenization (`llama_tokenize`, `llama_token_to_piece`)
- KV-cache management and batch decoding
- Sampling strategies (greedy, temperature, top-p)

### Network & Security
- libcurl multi/easy API
- HTTP file download with progress callbacks
- HuggingFace API discovery (`/api/models/.../tree/main`)
- Linux seccomp-bpf filter installation (`SCMP_ACT_KILL`)
- Cross-platform security model (feature degrade gracefully)

### Build Engineering
- CMake target configuration
- Cross-platform `find_package` / `find_library` handling
- Git submodule management
- Platform-specific linking (SDL2, OpenGL, curl, seccomp, ws2_32, winmm)

## Version

Current version: **0.1**
