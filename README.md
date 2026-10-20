# LibRFB

Library for Remote Frame Buffer protocol.

## Supported Encodings

1. Raw
2. ZRLE
3. H264

## Compile-time layer composition

The Linux tools assemble features with **linear policy-mixin inheritance**. Each
layer is a class template that inherits from the next layer and adds or
intercepts part of its interface:

```cpp
template<class Parent>
class add_logging : public Parent { /* ... */ };

template<class Parent>
class add_protocol : public Parent { /* ... */ };

using app = add_logging<add_protocol<base_app>>;
```

This is **not CRTP**. In CRTP, a derived class passes itself to a base template,
for example `class app : base<app>`. Here, each policy receives its parent layer
instead; the final type is a compile-time decorator stack. Calls generally flow
down through `parent::operation()`, while a layer can expose or override an
operation for layers above it.

Ordering is part of the contract. A layer must sit above the operations it
calls and below the layers whose operations it needs to intercept. Comments in
policy headers such as `control.hpp` and `trace.hpp` list those requirements.
There are no virtual calls, but construction runs from the innermost base
outward and destruction runs in reverse.

Do not write the entire application as one deeply nested alias. Group related
policies into named stages so dependencies and compiler errors remain readable:

```cpp
using app_transport = /* Wayland surface, polling, address and port */;
using app_rfb = add_rfb<app_transport>;
using app_presentation = add_presentation_copy<app_rfb>;
using app_input = add_input<app_presentation>;
using draw_app = add_event_loop<app_input>;
```

For stateful resources with coupled lifetimes, prefer one cohesive policy layer
(such as the multi-mode presentation policy) over scattering one resource
across many tiny inheritance layers.

## rfb_window_demo presentation modes

The Wayland viewer supports two GPU DMA-BUF presentation methods:

```text
--presentation-mode legacy|release-gated
```

`legacy` creates one BO, attaches its `wl_buffer` once, and repeatedly copies
into and commits that same storage. It uses the correct XRGB8888 byte stride,
but may rewrite a buffer while the compositor still owns it and therefore does
not follow the normal Wayland buffer ownership contract; use it only for
compatibility experiments.

`release-gated` rotates three BOs. Each frame is copied into a free BO, attached,
and committed; a BO is reused only after `wl_buffer.release`. Frames coalesce to
the latest decoded image while all three slots are busy, and old resize
generations remain alive until release. This is the protocol-correct mode, but
on the current AMD/Sway setup its physical fullscreen scanout can be white even
though composited capture is correct.

Examples:

```bash
rfb_window_demo 127.0.0.1 5900 zrle,raw --presentation-mode legacy
rfb_window_demo 127.0.0.1 5900 zrle,raw --presentation-mode release-gated
```

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
rfb_server --console-launcher [server options]   # Windows autostart helper
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

### Starting it automatically

`install_rfb_server_autostart.ps1` installs the server as an autostart entry so
it survives a reboot. It offers two mechanisms, because the session rule above
leaves no single obvious one:

```powershell
# At boot, before anyone logs in (the default).
powershell -ExecutionPolicy Bypass -File install_rfb_server_autostart.ps1 `
    -Port 5901 -Listen 127.0.0.1 -Name water-coffee-rfb -StartNow

# Only while this user is signed in.
powershell -ExecutionPolicy Bypass -File install_rfb_server_autostart.ps1 -AtLogon

# Remove either of them, and stop a running server.
powershell -ExecutionPolicy Bypass -File install_rfb_server_autostart.ps1 -Uninstall
```

`rfb_server.exe` is auto-detected by walking up from the script directory
through the usual build output paths; pass `-ServerPath` if it lives elsewhere.
The server options are passed through as parameters rather than as a command
line: `-Port`, `-Listen`, `-Password`, `-Name`, `-Fps`, `-AllMonitors`,
`-NoCursor`, `-ViewOnly`. `-StartNow` also starts the server immediately
instead of waiting for the next boot or logon.

| | `-AtBoot` (default) | `-AtLogon` |
|---|---|---|
| Mechanism | Scheduled task, `/SC ONSTART`, `/RU SYSTEM`, `/RL HIGHEST` | `%LOCALAPPDATA%\librfb\rfb_server_start.cmd` run hidden by `rfb_server.vbs` in the Startup folder |
| Serves | From boot, including the logon screen, across logon and logoff | Only while that user is signed in |
| Needs | An elevated shell to create a SYSTEM task | Nothing special |
| Log | `%ProgramData%\librfb\rfb_server.log` | `%LOCALAPPDATA%\librfb\rfb_server.log` |
| Task name | `-TaskName`, default `rfb_server` | n/a |

**How `-AtBoot` reaches the desktop.** Its action is not the server but
`rfb_server --console-launcher <server options>`, a supervisor that stays in
session 0 and pushes the real server into whichever session is attached to the
console. It duplicates its own LocalSystem token, retargets it with
`SetTokenInformation(TokenSessionId)` — which needs `SE_TCB_NAME`, a privilege
LocalSystem has and ordinary administrators do not, hence `/RU SYSTEM` — and
starts the worker on `WinSta0\Default`. A console session exists from boot
onwards showing the logon screen, so the desktop is served before anyone signs
in. The supervisor then polls, restarting the worker if it exits or if the
console session changes, which is what carries the server across logon, logoff
and Fast User Switching. `--console-launcher` is Windows-only and refuses to
run elsewhere.

Windows delivers input to the desktop of the *calling thread*, and a device
context belongs to the desktop it was created on, so both capture and injection
have to follow the desktop that is currently receiving input — which changes at
the logon screen, on lock, and whenever a UAC prompt raises the secure desktop.
That desktop is polled by name rather than detected from a failure, because a
`BitBlt` from a stale desktop keeps *succeeding* and quietly returns the wrong
screen. Injection additionally needs `DESKTOP_JOURNALPLAYBACK` on that desktop;
without it `SendInput` fails with `ERROR_ACCESS_DENIED` and the session looks
like it is ignoring the client. That right is refused to anything but
LocalSystem on the secure desktop, so a server that cannot get it falls back to
capture-only rights and serves the screen read-only.

A scheduled task created with `schtasks /IT` would be the direct way to reach
the interactive session, and it is what the two mechanisms above exist to avoid:
it fails with result 1 on some machines. Note also that a task created by plain
`schtasks /create /sc ONCE` has a one-shot trigger in the past and an
`InteractiveToken` principal, so it never comes back after a reboot — it only
ever starts from `schtasks /run`. The default `schtasks` settings additionally
refuse to start the task on battery and stop it when an idle period ends.

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

**Encoder pacing.** An encoder with output delay (`h264_amf` is one) answers a
submitted frame with an *older* picture, which the encoder reports by stamping
each packet with the pts of the frame it describes. The frame is nevertheless
inside the encoder, so it must be recorded as submitted straight away, and the
picture still owed to the client is collected later by draining the encoder
without feeding it anything new.

Getting this wrong is expensive and hard to see. Treating a late answer as
"this frame was not sent" leaves the screen looking changed, so the very next
poll resubmits the same picture; the queue is refilled as fast as the session
loop spins, and because it drains one picture per submission, the client ends
up watching a screen several capture intervals old. Measured on water-coffee at
`--fps 20` under a pointer-move workload, the session sent **89 updates/s
totalling 93 bytes/s** — almost every one a skip frame, while the real pixels
sat in the queue. Pointer movement lagged badly, yet transport latency looked
excellent precisely because the packets carrying nothing arrived promptly. ZRLE
was unaffected: it has no encoder pipeline, so its pixels are final on arrival.
With the frame recorded as submitted, the same workload sends **16.5 updates/s
carrying 246 KB/s at 26 ms mean latency**.

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
It takes half as much again as the budget before anything moves: a single
update landing marginally late is normal on any real link — one big window
repaint will do it — and reacting to that alone means reacting constantly.
The QP never goes below `--quality` and never above 45. `--no-adapt-quality`
turns the whole thing off.

**Keyframes are excluded from the measurement**, and not merely because an IDR
is several times the size of the updates around it. On an encoder that cannot
be retuned in place a quality change *is* a keyframe, so counting one closes a
loop in which every correction provokes the next: measured against a real
1080p desktop over an SSH tunnel, `h264_amf` walked the QP up and down twelve
times without ever settling, IDR-ing on each step, which the viewer sees as
lag. With keyframes excluded the same session makes two changes in 45 seconds.

Easing off needs a run of four comfortable updates, because a single quick one
is as likely to mean the screen barely changed as it is to mean the link has
room. The step size is then chosen by what it costs rather than by what was
measured — a write that returned immediately says only that the socket was not
blocked, not how much headroom there is. libx264 is retuned in place (ffmpeg's
wrapper re-reads its `crf` option per frame and calls `x264_encoder_reconfig`),
so there it creeps back one step at a time and lets the next slow update stop
the creep. Everything else is reopened, at the cost of an IDR, so one jump
straight back to the asked-for quality is cheaper than the same distance
walked, and being wrong costs a single correction upwards. Changes are rationed
either way: no more than one per 250 ms, or per second for a reopening
encoder.

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

The QP walked 18 -> 27 -> 35 -> 42 -> 45 and stayed there; at a fixed qp 18 the
same stream is effectively frozen. On a 1024x768 Linux test pattern at
500 kbit/s the effect is milder but the same shape: 234 frames against 93 over
20 seconds. Uncapped, the QP stays at 18 apart from the occasional single
excursion and return — the controller only ever degrades a link that is
demonstrably too slow.

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

### Control port

`--control-port <port>` opens a line-oriented text protocol on
`127.0.0.1:<port>` (loopback only, never a real interface). Loopback is an
exposure reduction, not an identity check: another process running as any user
on the same machine may be able to connect. The protocol has no authentication,
so enable it only on a trusted single-user host and reach a remote instance only
through an SSH tunnel you already control. One command
per line, one `ok ...`/`error ...` reply per line. Verbs:

| Command | Effect |
|---|---|
| `ping` | liveness check |
| `size` | current framebuffer width/height |
| `capture <path>` | write the current decoded frame to a PNG at `<path>` |
| `pointer <x> <y> <buttons>` | inject a pointer event |
| `click <x> <y> <button>` | pointer down+up at a position |
| `key <keysym> <state>` | inject a key event |
| `encoding <name>` | renegotiate encoding |
| `scale <factor>` | change display scale |
| `record start <dir> <interval_ms>` / `record stop` | start/stop the input+frame trace recorder (`trace.hpp`), writing `trace.jsonl` plus numbered PNGs under `<dir>` |
| `mark [text]` / `frame` | insert a labelled checkpoint / an extra frame capture into an in-progress recording |
| `refresh` | force a full, non-incremental repaint — a cheap "is the request loop still alive" probe |
| `status` | `host`, `port`, negotiated `encoding`, `bytes_received`, `ms_since_last_update`, `view_only` — the last field is the direct stalled-vs-static check: on an unchanging remote screen it stays small because nothing new arrived to update it, while a stuck request loop makes it grow without bound |
| `view-only on\|off` | toggle whether input is forwarded to the remote host at runtime |
| `quit` | ask the viewer to exit cleanly |

### `rfb_ctl.py`

`rfb_ctl.py` is a stdlib-only (optional Pillow for pixel-exact comparison)
Python client for the control port, meant to run unmodified on the Windows
boxes these viewers are pointed at. One-shot subcommands
(`status`/`refresh`/`capture`/`view-only`/`quit`/`cmd <raw line>`), plus
`replay <trace_dir>`: replays the `key`/`pointer` events from a
`record`-produced `trace.jsonl` against a *live* viewer at their recorded
timing, and after each `start`/`stop`/`mark`/`frame` line captures the live
frame and compares it against the one recorded at that point — reporting a
`MISMATCH` rather than silently continuing if a click landed somewhere else
than it did when recorded. See `rfb_ctl.py --help` and its module docstring
for the full protocol and the security-model note. `test_rfb_ctl.py` covers
the client and replay logic entirely offline, against a small fake
control-port stub the test starts itself (no `rfb_window_demo`/`rfb_server`
required); run with `python3 test_rfb_ctl.py`.

Recording itself is not new: it is the existing `record`/`mark`/`frame`
verbs above, implemented by `trace.hpp`, reached through `rfb_ctl.py cmd`.
What is new here is replay, and the `refresh`/`status`/`view-only`/`quit`
verbs that make the control port sufficient to script a viewer end to end.
Video/frame encoding beyond the existing per-checkpoint PNGs is deliberately
out of scope for this first version.

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
| `rfb_window_win32` | no | yes |
| `test_hip` | yes | no |

`rfb_window_demo` and `test_hip` are Linux only: they render through Wayland,
DRM/amdgpu and HIP external memory. Windows builds instead provide
`rfb_window_win32`, a native Win32 window with a D3D11 swap chain using the same
RFB Raw/ZRLE/H264 protocol stack. **The release-supported Windows path is
read-only viewing:** run it with `--view-only`. Input, clipboard, scale, trace
recording and control-port code is present for development, but those broader
interaction paths have not been validated for release and are not claimed as
supported. `capture` and `record start` accept quoted paths, including normal
Windows paths containing spaces.

### Building on Windows

Requires MSVC (Visual Studio 2022 or Build Tools) and the dependencies below.
The CMake files use plain `find_package`, so any provider works; vcpkg is the
easiest:

```bat
rem Dependencies are declared by the repository-root vcpkg.json manifest.
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Run the native viewer with:

```bat
build\librfb\Release\rfb_window_win32.exe 127.0.0.1 5900 h264,zrle,raw ^
    --view-only
```

The control port is disabled by default and binds only to `127.0.0.1` when
enabled. For multi-step automation, keep one viewer alive and drive its control
port instead of reconnecting `rfb_capture`/`rfb_input` for every operation.
Control coordinates are full RFB framebuffer coordinates; negative and
out-of-frame pointer commands are rejected. `clipboard-set <base64>` and
`clipboard-read` provide binary-safe clipboard transport. Use `--password-file`
for authenticated RFB servers and never expose the unauthenticated control port
on a non-loopback interface. This persistent path needs no TigerVNC, Xvfb, X11,
or libX11 layer.

`rfb_capture` requests one retained full framebuffer and defaults to a 60-second
deadline suitable for large desktops; override it with `--timeout <ms>`. PNGs
are written to a same-directory temporary file and atomically published, so a
failed capture cannot masquerade as a fresh result.

D3D11, DXGI, User32 and GDI32 come from the Windows SDK; vcpkg supplies
zlib, OpenSSL and FFmpeg. Visual Studio 2022 with current C++20 library support
is required.

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
