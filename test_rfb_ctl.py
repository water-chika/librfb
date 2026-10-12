#!/usr/bin/env python3
"""Offline tests for rfb_ctl.py: no rfb_window_demo, no rfb_server, no
network beyond loopback to a fake control-port stub this test starts and
stops itself. Run with: python3 test_rfb_ctl.py
"""

import json
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import rfb_ctl  # noqa: E402


def _write_minimal_png(path, width, height, rgb):
    """A tiny hand-rolled PNG encoder -- just enough for the test to produce
    real, valid PNGs without depending on Pillow being installed to WRITE
    fixtures (rfb_ctl.py itself only ever needs Pillow to READ them)."""
    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        out += struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
        return out

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    raw = bytearray()
    for _ in range(height):
        raw.append(0)
        raw.extend(rgb * width)
    idat = zlib.compress(bytes(raw), 9)
    data = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    Path(path).write_bytes(data)


class FakeControlServer:
    """Implements just enough of control.hpp's line protocol for these
    tests: status/refresh/capture/view-only/quit/key/pointer, with captures
    always writing the SAME fixed-colour PNG (a stand-in for "the desktop
    hasn't changed"), so tests can flip the colour to simulate a real
    on-screen change and confirm replay notices it.
    """

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.view_only = False
        self.received = []
        self.capture_rgb = (10, 20, 30)
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
        try:
            socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
        except OSError:
            pass
        self._thread.join(timeout=2)
        self.sock.close()

    def _serve(self):
        self.sock.settimeout(0.5)
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(2)
                buf = b""
                while True:
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        break
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        reply = self._handle(line.decode())
                        conn.sendall(reply.encode() + b"\n")
                        if reply == "__CLOSE__":
                            return

    def _handle(self, line):
        import shlex
        parts = shlex.split(line, posix=False)
        parts = [part[1:-1].replace('\\\\', '\\').replace('\\"', '"')
                 if len(part) >= 2 and part[0] == part[-1] == '"' else part
                 for part in parts]
        if not parts:
            return "ok"
        cmd = parts[0]
        self.received.append(line)
        if cmd == "status":
            return (
                "ok host=127.0.0.1 port=5900 encoding=16 bytes_received=1234 "
                f"ms_since_last_update=5.0 view_only="
                f"{'true' if self.view_only else 'false'}")
        if cmd == "refresh":
            return "ok"
        if cmd == "capture":
            path = parts[1]
            _write_minimal_png(path, 4, 4, self.capture_rgb)
            return f"ok {path}"
        if cmd == "view-only":
            self.view_only = parts[1] == "on"
            return f"ok {parts[1]}"
        if cmd in ("key", "pointer"):
            return "ok"
        if cmd == "quit":
            return "ok"
        return f"error unknown command: {cmd}"


class ControlClientTest(unittest.TestCase):
    def setUp(self):
        self.server = FakeControlServer()
        self.addCleanup(self.server.stop)

    def test_status_roundtrip(self):
        with rfb_ctl.ControlClient("127.0.0.1", self.server.port) as c:
            reply = c.send("status")
        parsed = rfb_ctl.parse_status(reply)
        self.assertEqual(parsed["encoding"], "16")
        self.assertEqual(parsed["view_only"], "false")

    def test_view_only_toggle(self):
        with rfb_ctl.ControlClient("127.0.0.1", self.server.port) as c:
            reply = c.send("view-only on")
            self.assertEqual(reply, "ok on")
            reply = c.send("status")
        self.assertEqual(rfb_ctl.parse_status(reply)["view_only"], "true")

    def test_capture_subcommand(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "frame with spaces.png"
            rc = rfb_ctl.main([
                "127.0.0.1", str(self.server.port), "capture", str(out)])
            self.assertEqual(rc, 0)
            self.assertTrue(out.exists())

    def test_unknown_command_is_reported_as_error(self):
        with rfb_ctl.ControlClient("127.0.0.1", self.server.port) as c:
            reply = c.send("not-a-real-command")
        self.assertTrue(reply.startswith("error"))


class ReplayTest(unittest.TestCase):
    def setUp(self):
        self.server = FakeControlServer()
        self.addCleanup(self.server.stop)
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)
        self.trace_dir = Path(self.tmpdir.name)

    def _write_trace(self, lines):
        (self.trace_dir / "trace.jsonl").write_text(
            "\n".join(json.dumps(line) for line in lines) + "\n")

    def test_replay_sends_recorded_input_in_order(self):
        self._write_trace([
            {"t_ms": 0, "type": "key", "keysym": 65, "state": 1},
            {"t_ms": 5, "type": "pointer", "x": 10, "y": 20, "buttons": 1},
        ])
        rc = rfb_ctl.main([
            "127.0.0.1", str(self.server.port), "replay",
            str(self.trace_dir), "--speed", "1000"])
        self.assertEqual(rc, 0)
        self.assertIn("key 65 1", self.server.received)
        self.assertIn("pointer 10 20 1", self.server.received)

    def test_replay_matches_unchanged_frame(self):
        _write_minimal_png(
            self.trace_dir / "frame_000000.png", 4, 4, (10, 20, 30))
        self._write_trace([
            {"t_ms": 0, "type": "start", "frame": "frame_000000.png"},
        ])
        rc = rfb_ctl.main([
            "127.0.0.1", str(self.server.port), "replay",
            str(self.trace_dir), "--speed", "1000"])
        self.assertEqual(rc, 0)

    def test_replay_flags_a_changed_frame_as_mismatch(self):
        # Recorded frame was blue; the live server (simulating a screen that
        # now shows something else) answers every capture with red instead.
        _write_minimal_png(
            self.trace_dir / "frame_000000.png", 4, 4, (0, 0, 255))
        self.server.capture_rgb = (255, 0, 0)
        self._write_trace([
            {"t_ms": 0, "type": "mark", "text": "checkpoint",
             "frame": "frame_000000.png"},
        ])
        rc = rfb_ctl.main([
            "127.0.0.1", str(self.server.port), "replay",
            str(self.trace_dir), "--speed", "1000"])
        self.assertEqual(rc, 1)

    def test_replay_reports_missing_trace_cleanly(self):
        empty_dir = self.trace_dir / "empty"
        empty_dir.mkdir()
        rc = rfb_ctl.main([
            "127.0.0.1", str(self.server.port), "replay", str(empty_dir)])
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
