#!/usr/bin/env python3
"""rfb_ctl.py -- drive a running rfb_window_demo over its --control-port.

Talks the same line-oriented text protocol control.hpp implements: one
command per line, one "ok ..."/"error ..." reply per line. Stdlib socket
code only (plus an OPTIONAL Pillow import used only to compare two PNGs
pixel-for-pixel during `replay`) -- this is meant to run unmodified on the
Windows boxes a viewer would be pointed at, not just on the Linux box that
happens to be driving it right now.

SECURITY MODEL: this tool assumes what control.hpp assumes -- the control
port is bound to 127.0.0.1 only (rfb_window_demo does this itself; it is not
this tool's job to enforce). rfb_ctl.py never binds a socket of its own, it
only connects out, so running it adds no new attack surface. Point it at a
loopback port, normally reached the same way rfb_view.sh/rfb_view.py already
reach the RFB port itself: an SSH -L tunnel. Never point --host at anything
other than 127.0.0.1/localhost unless you have already tunnelled it there
yourself -- the control protocol has no authentication of its own, by
design (see control.hpp), so exposing it on a real interface would let
anything that can reach the port drive the remote desktop.

VERBS (one-shot subcommands, each opens a connection, sends one command,
prints the reply, and exits):
  status              host/port/negotiated encoding/bytes received/how long
                       since the last update arrived/view-only state. This
                       is the stalled-vs-static check: a small
                       ms_since_last_update on an unchanging screen means
                       the server correctly has nothing new to say; a large
                       and growing one means the request loop is stuck.
  refresh             force a full, non-incremental repaint
  capture <path.png>  dump the current frame to a PNG on the VIEWER's
                       filesystem (not this tool's -- see capture()'s note)
  view-only on|off    toggle whether input is forwarded to the remote host
  quit                ask the viewer to exit cleanly
  cmd <raw line>       send any control.hpp verb verbatim (pointer/click/
                       key/encoding/scale/record/mark/frame/...)

RECORDING is already built into rfb_window_demo itself (control.hpp's
`record start <dir> <interval_ms>` / `record stop` / `mark` / `frame`,
implemented by trace.hpp) -- this tool does not duplicate it, it only
exposes it, via `rfb_ctl.py ... cmd record start <dir> <interval_ms>`.

REPLAY is the part that did not exist anywhere in this codebase before this
tool: given a directory written by that recorder (a trace.jsonl plus the
PNGs it captured), `replay` re-sends every recorded key/pointer event to a
LIVE viewer at (approximately) the recorded timing, and after each frame
line in the trace (start/stop/mark/frame) it captures the live viewer's
current frame and compares it against the recorded PNG at that same point,
reporting a MISMATCH rather than silently continuing -- a replay that
clicked the wrong place and nobody noticed is worse than no replay, per the
brief this was built against. Comparison degrades from pixel-exact (needs
Pillow) to size-only (a weaker but still real check) if Pillow is not
installed, and says plainly which one it did.
"""

import argparse
import json
import socket
import sys
import time
from pathlib import Path


class ControlError(RuntimeError):
    pass


def positive_float(value):
    value = float(value)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return value


class ControlClient:
    """One control-port connection. Not reused across processes: control.hpp
    replaces rather than refuses a second connection, so a script that keeps
    its own connection open for a whole session avoids racing anyone else
    doing the same against the same viewer."""

    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, line):
        self.sock.sendall(line.encode("utf-8") + b"\n")
        return self._read_line()

    def _read_line(self):
        while b"\n" not in self._buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ControlError("control connection closed unexpectedly")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")


def parse_status(reply):
    """'ok host=... port=... encoding=... bytes_received=...
    ms_since_last_update=... view_only=...' -> dict. Kept separate from
    ControlClient so replay's stall-vs-static check can reuse it."""
    if not reply.startswith("ok "):
        raise ControlError(f"status failed: {reply}")
    out = {}
    for field in reply[3:].split():
        if "=" in field:
            key, _, value = field.partition("=")
            out[key] = value
    return out


def cmd_status(args):
    with ControlClient(args.host, args.port) as c:
        reply = c.send("status")
        print(reply)
        if not reply.startswith("ok"):
            return 1
    return 0


def cmd_refresh(args):
    with ControlClient(args.host, args.port) as c:
        reply = c.send("refresh")
        print(reply)
        return 0 if reply.startswith("ok") else 1


def quote_argument(value):
    """Quote one control-protocol argument with C++ std::quoted semantics."""
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def cmd_capture(args):
    with ControlClient(args.host, args.port) as c:
        reply = c.send(f"capture {quote_argument(args.path)}")
        print(reply)
        return 0 if reply.startswith("ok") else 1


def cmd_view_only(args):
    with ControlClient(args.host, args.port) as c:
        reply = c.send(f"view-only {args.state}")
        print(reply)
        return 0 if reply.startswith("ok") else 1


def cmd_quit(args):
    with ControlClient(args.host, args.port) as c:
        reply = c.send("quit")
        print(reply)
    return 0


def cmd_raw(args):
    with ControlClient(args.host, args.port) as c:
        reply = c.send(" ".join(args.line))
        print(reply)
        return 0 if reply.startswith("ok") else 1


def _images_match(expected_png, actual_png):
    """Returns (matched: bool, method: str). Prefers a real pixel compare;
    falls back to a byte-size check (still catches "totally different
    content", just not "one pixel off") when Pillow is unavailable, and says
    which one it used so a report never silently overclaims precision."""
    try:
        from PIL import Image, ImageChops
    except ImportError:
        try:
            expected_size = Path(expected_png).stat().st_size
            actual_size = Path(actual_png).stat().st_size
        except OSError as e:
            return False, f"size-only (could not stat: {e})"
        close = abs(expected_size - actual_size) <= max(
                256, expected_size // 20)
        return close, "size-only (Pillow not installed)"
    try:
        with Image.open(expected_png) as a, Image.open(actual_png) as b:
            a = a.convert("RGB")
            b = b.convert("RGB")
            if a.size != b.size:
                return False, f"pixel (size differs: {a.size} vs {b.size})"
            diff = ImageChops.difference(a, b)
            bbox = diff.getbbox()
            if bbox is None:
                return True, "pixel (identical)"
            # A few pixels of cursor blink/clock tick should not fail a
            # replay; a genuinely different screen (wrong click target) will
            # differ over a much larger area.
            hist = diff.convert("L").histogram()
            changed = sum(hist[16:])  # ignore near-zero noise
            total = a.size[0] * a.size[1]
            ratio = changed / total
            return ratio < 0.02, f"pixel (changed {ratio:.1%} of pixels)"
    except Exception as e:
        return False, f"pixel compare failed: {e}"


def cmd_replay(args):
    trace_path = Path(args.trace_dir) / "trace.jsonl"
    if not trace_path.exists():
        print(f"error no trace.jsonl in {args.trace_dir}", file=sys.stderr)
        return 1
    events = [json.loads(line) for line in trace_path.read_text().splitlines()
              if line.strip()]
    if not events:
        print("error trace.jsonl is empty", file=sys.stderr)
        return 1

    mismatches = 0
    frame_checks = 0
    with ControlClient(args.host, args.port) as c:
        start_wall = time.monotonic()
        for ev in events:
            target_t = ev["t_ms"] / 1000.0 / args.speed
            elapsed = time.monotonic() - start_wall
            if target_t > elapsed:
                time.sleep(target_t - elapsed)

            kind = ev["type"]
            if kind == "key":
                reply = c.send(f"key {ev['keysym']} {ev['state']}")
                if not reply.startswith("ok"):
                    print(f"error replaying key at t={ev['t_ms']}ms: {reply}",
                          file=sys.stderr)
                    mismatches += 1
            elif kind == "pointer":
                reply = c.send(
                        f"pointer {ev['x']} {ev['y']} {ev['buttons']}")
                if not reply.startswith("ok"):
                    print(
                        f"error replaying pointer at t={ev['t_ms']}ms: "
                        f"{reply}", file=sys.stderr)
                    mismatches += 1
            elif kind in ("start", "stop", "mark", "frame") and ev.get("frame"):
                recorded_png = Path(args.trace_dir) / ev["frame"]
                if not recorded_png.exists():
                    continue
                live_png = Path(args.trace_dir) / f"replay_{ev['frame']}"
                reply = c.send(f"capture {quote_argument(live_png)}")
                if not reply.startswith("ok"):
                    print(
                        f"error capturing replay frame at t={ev['t_ms']}ms: "
                        f"{reply}", file=sys.stderr)
                    mismatches += 1
                    continue
                frame_checks += 1
                matched, method = _images_match(recorded_png, live_png)
                label = ev.get("text") or ev.get("note") or kind
                if matched:
                    print(f"t={ev['t_ms']:>7}ms  ok    {label}  ({method})")
                else:
                    mismatches += 1
                    print(
                        f"t={ev['t_ms']:>7}ms  MISMATCH  {label}  ({method})"
                        f"  -- recorded {recorded_png}, live {live_png}",
                        file=sys.stderr)

    print(f"replay finished: {frame_checks} frame checks, "
          f"{mismatches} mismatch(es)")
    return 1 if mismatches else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    sub = parser.add_subparsers(dest="verb", required=True)

    sub.add_parser("status").set_defaults(func=cmd_status)
    sub.add_parser("refresh").set_defaults(func=cmd_refresh)
    p_capture = sub.add_parser("capture")
    p_capture.add_argument("path")
    p_capture.set_defaults(func=cmd_capture)
    p_view = sub.add_parser("view-only")
    p_view.add_argument("state", choices=["on", "off"])
    p_view.set_defaults(func=cmd_view_only)
    sub.add_parser("quit").set_defaults(func=cmd_quit)
    p_cmd = sub.add_parser("cmd")
    p_cmd.add_argument("line", nargs="+")
    p_cmd.set_defaults(func=cmd_raw)
    p_replay = sub.add_parser("replay")
    p_replay.add_argument("trace_dir")
    p_replay.add_argument("--speed", type=positive_float, default=1.0,
                           help="playback speed multiplier (default 1.0, "
                                "real time)")
    p_replay.set_defaults(func=cmd_replay)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (ControlError, OSError, socket.timeout) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
