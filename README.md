# LibRFB

Library for Remote Frame Buffer protocol.

## Supported Encodings

1. Raw
2. ZRLE
3. H264

## rfb window demo

`rfb_window_demo` renders to a Wayland surface using DRM (amdgpu BOs exported as
dma-buf) shared with ROCm/HIP via external memory. Vulkan is no longer used.

## Platforms

| Target | Linux | Windows |
|---|---|---|
| `rfb` (library) | yes | yes |
| `rfb_demo` | yes | yes |
| `rfb_input` | yes | yes |
| `rfb_recorder` | yes | yes |
| `rfb_window_demo` | yes | no |
| `test_hip` | yes | no |

`rfb_window_demo` and `test_hip` are Linux only: they render through Wayland,
DRM/amdgpu and HIP external memory, which have no Windows equivalent. CMake
skips them automatically on Windows.

### Building on Windows

Requires MSVC (Visual Studio 2022 or Build Tools) and the dependencies below.
The CMake files use plain `find_package`, so any provider works; vcpkg is the
easiest:

```bat
vcpkg install zlib:x64-windows openssl:x64-windows ^
    "ffmpeg[avcodec,avformat,avdevice,swscale,zlib,x264]:x64-windows"

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

TBB is not needed on Windows. `h264.hpp` uses
`std::for_each(std::execution::par, ...)`, and only libstdc++ delegates its
parallel algorithms to TBB; the MSVC STL implements them natively.

The `zlib` and `x264` ffmpeg features are not optional:

- `zlib` provides the **PNG encoder**, which `rfb_input --capture` needs.
- `x264` provides the **H.264 encoder**, which `rfb_recorder` needs. It pulls
  in ffmpeg's `gpl` feature, matching a typical Linux ffmpeg
  (`--enable-gpl --enable-libx264`). Use `openh264` instead to avoid GPL.

Without them ffmpeg still builds, but the tools fail at runtime with
`png encoder not found in this ffmpeg build` or the H.264 equivalent.

## Reference

[rfbproto](https://github.com/rfbproto/rfbproto)
