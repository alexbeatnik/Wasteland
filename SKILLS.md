# SKILLS.md — Wasteland v0.1

## Available Skills

This project does not use a formal skill system. The following domains are relevant for contributors:

### Systems Programming
- Linux pthreads, mutexes, condition variables
- POSIX file I/O and directory scanning (`dirent.h`, `sys/stat.h`)
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

### Network & Security
- libcurl multi/easy API
- HTTP file download with progress callbacks
- Linux seccomp-bpf filter installation
- Syscall interception and process sandboxing

### Build Engineering
- CMake target configuration
- Git submodule management
- Cross-platform library linking (SDL2, OpenGL, curl, seccomp)

## Version

Current version: **0.1**
