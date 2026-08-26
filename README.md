# LibRFB

Library for Remote Frame Buffer protocol.

## Supported Encodings

1. Raw
2. ZRLE
3. H264

## rfb_server

`rfb_server` is the server half of the protocol: it exports the local desktop
and replays the keyboard and pointer events a viewer sends back. It is aimed
at Windows 11, where it captures through GDI and injects input with
`SendInput`. On other platforms it serves an animated test pattern instead,
which is what the Linux build is for.

```
rfb_server [--port 5900] [--listen 127.0.0.1] [--password <pw>] [--name <text>]
           [--view-only] [--fps 20] [--all-monitors] [--no-cursor]
```

- Protocol versions 3.3, 3.7 and 3.8, security types None and VNC
  Authentication, Raw and ZRLE encodings.
- Only 8 characters of `--password` are significant: VNC Authentication uses
  the password as a DES key. It is not transport encryption, so tunnel the
  connection (`ssh -L 5900:127.0.0.1:5900 host`) and pair it with
  `--listen 127.0.0.1` when the network is not trusted.
- Clipboard transfers both ways on Windows.
- Multiple viewers can connect at once; each gets its own thread, encoder and
  zlib stream.
- `rfb_server` deliberately does not link the `rfb` library, so it needs only
  zlib and OpenSSL — no ffmpeg.

### Windows: it must run in the interactive session

A process started over SSH, or by a service, lands in **session 0**, which has
no visible desktop: capture comes back empty and injected input goes nowhere.
`rfb_server` warns when it detects this. Start it inside the logged-in session
instead, e.g. with Sysinternals PsExec:

```bat
PsExec64.exe -accepteula -d -i 1 -s C:\path\to\rfb_server.exe --port 5900 --password secret
```

`-i 1` is the session ID from `query session`. A scheduled task created with
`/it` works too.

PsExec gives the server its own console in the target session and does **not**
relay its output back, so to keep a log, redirect inside the target session:

```bat
PsExec64.exe -accepteula -d -i 1 -s cmd /c "C:\path\to\rfb_server.exe --port 5900 --password secret > C:\path\to\rfb_server.log 2>&1"
```

### Planned: H264 encoding

The server currently negotiates Raw and ZRLE only; a client offering H264 (50)
is served ZRLE. Adding it means, in dependency order:

1. **Optional dependency.** Guard the encoder with a CMake option
   (`RFB_SERVER_H264`, default `OFF`) that pulls in libavcodec/libavutil/
   libswscale and defines `RFB_SERVER_HAS_H264`. The default Windows build has
   to stay ffmpeg-free — that is the reason `rfb_server` does not link `rfb`.
2. **Encoder.** A `server_h264.hpp` with an `h264_encoder` mirroring
   `zrle_encoder`'s interface: per-session libx264 (`veryfast`,
   `zerolatency`, no B-frames) plus libswscale converting the internal 32bpp
   BGRX buffer to YUV420P. `recorder.cpp` already does that conversion.
3. **Wire format.** After the usual 12-byte rect header with encoding 50, the
   client reads a big-endian `uint32` length and a big-endian `uint32` flags
   word, then that many bytes of H264 fed straight to avcodec
   (`rfb.hpp`). `flags` is currently parsed and ignored; its meaning needs
   defining, and is the natural place to mark a keyframe.
4. **Rect geometry.** H264 cannot use the 64x64 dirty rectangles
   `collect_dirty()` produces, because the encoder needs a stream of fixed
   size. Send one full-screen rect per update and keep dirty detection only to
   decide whether to send a frame at all. Force an IDR on the first frame of a
   session and on any desktop size change.
5. **Negotiation.** Replace the `m_zrle_supported` flag with an ordered
   preference list so 50 can win over 16 and 0, and only when the build has the
   encoder.
6. **Colour space.** The client's H264 colour space is still wrong (see the
   hand-written coefficient table in `h264.hpp`), so pin range and matrix on
   both sides before trusting any output.
7. **Verification.** Same standard the ZRLE path was held to: decode a known
   pattern and count mismatching pixels, then compare a capture against a ZRLE
   capture of the same static screen, expecting only lossy-codec noise.

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
| `rfb_server` | test pattern | yes |
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

The `zlib` and `x264` ffmpeg features are not optional (`rfb_server` needs
neither — it only links zlib and OpenSSL):

- `zlib` provides the **PNG encoder**, which `rfb_input --capture` needs.
- `x264` provides the **H.264 encoder**, which `rfb_recorder` needs. It pulls
  in ffmpeg's `gpl` feature, matching a typical Linux ffmpeg
  (`--enable-gpl --enable-libx264`). Use `openh264` instead to avoid GPL.

Without them ffmpeg still builds, but the tools fail at runtime with
`png encoder not found in this ffmpeg build` or the H.264 equivalent.

## Reference

[rfbproto](https://github.com/rfbproto/rfbproto)
