# Cross-Compilation Guide

## Windows exe from Linux (MinGW)

### Install toolchain
```bash
# Debian/Ubuntu
sudo apt install mingw-w64 g++-mingw-w64 cmake

# Arch
sudo pacman -S mingw-w64-gcc mingw-w64-cmake
```

### Cross-compile dependencies
You need MinGW builds of SDL2 and curl. The easiest way is to use MXE:
```bash
git clone https://github.com/mxe/mxe.git
cd mxe
make sdl2 curl pthreads
# Then use the generated toolchain
```

Or download prebuilt libraries from SDL2/curl websites.

### Build with toolchain file
```bash
mkdir build-win && cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake
cmake --build . -j$(nproc)
# Produces: Wasteland.exe
```

## macOS dmg from Linux
**Not recommended.** Apple requires macOS SDK, Xcode, and code signing. Use one of:
- Build on a real Mac
- GitHub Actions macOS runner (see `.github/workflows/build.yml`)
- osxcross (very complex, breaks often with GUI/OpenGL apps)

## Recommended: GitHub Actions
See `.github/workflows/build.yml` — builds for Linux, macOS, and Windows automatically on every push.
