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
           [--view-only] [--fps 20] [--quality 18] [--no-adapt-quality]
           [--bandwidth <kbit>] [--all-monitors] [--no-cursor]
```

- Protocol versions 3.3, 3.7 and 3.8, security types None and VNC
  Authentication, Raw and ZRLE encodings, plus H264 in builds configured with
  `-DRFB_SERVER_H264=ON`.
- Only 8 characters of `--password` are significant: VNC Authentication uses
  the password as a DES key. It is not transport encryption, so tunnel the
  connection (`ssh -L 5900:127.0.0.1:5900 host`) and pair it with
  `--listen 127.0.0.1` when the network is not trusted.
- Clipboard transfers both ways on Windows.
- Multiple viewers can connect at once; each gets its own thread, encoder and
  zlib stream.
- `rfb_server` deliberately does not link the `rfb` library, so by default it
  needs only zlib and OpenSSL — no ffmpeg. Turning on `RFB_SERVER_H264` is the
  one thing that changes that.

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

### H264 encoding

`rfb_server` can serve encoding 50, but only when the build was configured with
`-DRFB_SERVER_H264=ON`. That is what defines `RFB_SERVER_HAS_H264` and links
libavcodec/libavutil/libswscale; the default build stays ffmpeg-free, which is
the whole reason `rfb_server` does not link the `rfb` library. When the encoder
is not built in, a client offering 50 is served ZRLE as before.

The encoder lives in `server_h264.hpp`, one per session: libx264 at `veryfast`
and `zerolatency` with no B-frames, fed by libswscale converting the internal
32bpp BGRX capture to YUV420P.

**Negotiation.** The order in `SetEncodings` is a priority hint, so the first
entry the server can produce wins — 50, 16 and 0 are all ranked by the client.
Switching encoding discards the server's copy of the client's screen and forces
an IDR.

**Rect geometry.** H264 needs a stream of one constant size, so it cannot use
the 64x64 dirty rectangles `collect_dirty()` produces. Every update is a single
full-screen rectangle, and dirty detection only decides whether a frame is worth
encoding at all. Odd screen sizes are padded up to even dimensions for YUV420P
(the last column and row are duplicated); the rectangle the client is told about
stays the real size, so the padding is never displayed.

**Wire format.** After the usual 12-byte rect header carrying encoding 50:

    u32 length         bytes of H264 that follow
    u32 flags          see below
    u8  data[length]   Annex B byte stream, fed straight to avcodec

The payload is one access unit: everything the encoder produced for a single
capture, so a viewer can hand it to `avcodec_send_packet()` unmodified. `flags`
is a bit field; every bit not listed is reserved and sent as zero:

| Bit | Name | Meaning |
|---|---|---|
| `0x1` | keyframe | The payload starts with an IDR, so a viewer can start decoding here. |
| `0x2` | stream reset | A new H264 sequence, with its own SPS/PPS and possibly new dimensions. Flush the decoder first. |

Older clients that parse the flags word and ignore it keep working.

**Adaptive quality.** A client can rank the quality level pseudo-encodings
(-32 for level 0 up to -23 for level 9) in its `SetEncodings` list, which pins
the QP and is taken as final. A client that ranks none of them has left the
trade-off to the server, and the server spends it on latency: it raises the QP
above `--quality` whenever an update takes longer to write than one capture
interval. With a blocking socket that write time *is* the time the link needed
to swallow the payload, so exceeding the interval means updates are queueing
and the viewer is falling behind. Encode time is deliberately excluded — it is
real latency, but it barely moves with the QP, so degrading the picture to fix
a slow CPU would buy nothing.

H264 bitrate is roughly geometric in the QP, near enough half the bits per six
steps, so the correction is computed in that unit and capped at +12 per update.
Recovery is one step at a time, on a four times longer timer, and only after a
run of four comfortable updates: a single quick update is as likely to mean the
screen barely changed as it is to mean the link has room, and acting on it
produces a permanent one-step oscillation. The QP never goes below `--quality`
and never above 45. `--no-adapt-quality` turns the whole thing off.

Whether a change costs a keyframe depends on the encoder: libx264 is retuned in
place (ffmpeg's wrapper re-reads its `crf` option per frame and calls
`x264_encoder_reconfig`), while everything else is reopened, which forces an
IDR. Changes are rationed for that reason — no more than one per 250 ms.

**Simulating a slow link.** `--bandwidth <kbit>` caps what each session may
write to its socket, through a token bucket with a 20 ms burst. It is a test
knob for exercising the above on a loopback connection, not a fair-sharing
policy: every client gets its own budget of that size.

Measured on a 1920x1080 Windows desktop (water-coffee, `h264_amf`) showing
continuous animation, capped at 500 kbit/s with `--fps 20`, recording for 25
seconds with `rfb_recorder`:

| | frames delivered | update latency |
|---|---|---|
| adaptive (default) | 27-28 per 5 s segment | 158 ms, from 510 ms at qp 18 |
| `--no-adapt-quality` | 1 per 7.6 s segment | over 600 ms |

The QP walked 18 -> 30 -> 42 -> 45 in three steps and stayed there; at a fixed
qp 18 the same stream is effectively frozen. On a 1024x768 Linux test pattern
at 500 kbit/s the effect is milder but the same shape: 240 frames against 95
over 20 seconds. Uncapped, the QP never leaves 18 — the controller only ever
degrades a link that is demonstrably too slow.

**Colour space.** Pinned to BT.709, limited range, and signalled in the VUI,
where `h264.hpp` reads it back from `frame->colorspace`/`frame->color_range`.
Limited range rather than full: a full range stream makes avcodec hand the
viewer the deprecated `YUVJ420P` pixel format, which viewers routinely fail to
recognise. The client accepts it anyway.

Note that the client's decoder needs `FF_THREAD_SLICE`: with avcodec's default
frame threading, finished pictures are held back by roughly the thread count, so
a session that opens with a single keyframe displays nothing at all.

**Verification.** Two viewers on one server, one asking for ZRLE and one for
H264, with `--fps 1` so both are served the identical capture. Over eight
rounds (one keyframe and seven inter frames) the decoded images differ by a mean
of 1.2 to 1.7 out of 255 per channel, with 1 to 3.5% of channels off by more
than 8 — lossy-codec noise on the band edges of the test pattern, and nothing
resembling a colour space error. Encoding an odd-sized (1023x767) gradient
directly and comparing the decoded luma against the BT.709 limited-range
expectation gives a mean error of 0.76 and a worst case of 12.

## rfb window demo

`rfb_window_demo` renders to a Wayland surface using DRM (amdgpu BOs exported as
dma-buf) shared with ROCm/HIP via external memory. Vulkan is no longer used.

## Platforms

| Target | Linux | Windows |
|---|---|---|
| `rfb` (library) | yes | yes |
| `rfb_demo` | yes | yes |
| `rfb_input` | yes | yes |
| `rfb_capture` | yes | yes |
| `rfb_clipboard` | yes | yes |
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

Add `-DRFB_SERVER_H264=ON` to the configure line to build `rfb_server` with
H264; it reuses the same vcpkg ffmpeg the client tools need.

TBB is not needed on Windows. `h264.hpp` splits each frame into horizontal
bands and converts them on plain `std::thread`s, so it relies on nothing beyond
the standard threading support both toolchains already provide.

The `zlib` and `x264` ffmpeg features are not optional (`rfb_server` needs
neither — it only links zlib and OpenSSL):

- `zlib` provides the **PNG encoder**, which `rfb_capture` needs.
- `x264` provides the **H.264 encoder**, which `rfb_recorder` needs. It pulls
  in ffmpeg's `gpl` feature, matching a typical Linux ffmpeg
  (`--enable-gpl --enable-libx264`). Use `openh264` instead to avoid GPL.

Without them ffmpeg still builds, but the tools fail at runtime with
`png encoder not found in this ffmpeg build` or the H.264 equivalent.

## Reference

[rfbproto](https://github.com/rfbproto/rfbproto)
