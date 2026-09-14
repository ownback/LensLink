#!/usr/bin/env python3
"""Fake LensLink phone: speaks enough of the wire protocol (docs/PROTOCOL.md)
to test the desktop daemon without a real iPhone. Streams an ffmpeg
test pattern as H.264 Annex B plus a 440 Hz sine as type-10 audio.

Usage: python3 tools/fake-phone.py [--port 9979] [--width 640] [--height 360]
       [--fps 30] [--no-standby] [--no-audio]
"""

import argparse
import math
import socket
import struct
import subprocess
import threading
import time

MAGIC = 0x4F425343
VERSION = 1

HELLO = 1
VIDEO_CONFIG = 2
VIDEO = 3
TIMESYNC_REQ = 5
TIMESYNC_RESP = 6
CONTROL = 7
SCREEN_AUDIO = 10

FLAG_KEYFRAME = 0x0001

HEADER = struct.Struct(">IBBHQI")


def pkt(ptype, flags, pts, payload=b""):
    return HEADER.pack(MAGIC, VERSION, ptype, flags, pts, len(payload)) + payload


class FakePhone:
    def __init__(self, args):
        self.args = args
        self.send_lock = threading.Lock()
        self.start_event = threading.Event()
        self.live = False

    def send(self, pt, flags, pts, payload=b""):
        with self.send_lock:
            self.conn.sendall(pkt(pt, flags, pts, payload))

    def send_json(self, pt, text):
        self.send(pt, 0, 0, text.encode())

    def reader(self):
        buf = b""
        while self.live:
            try:
                data = self.conn.recv(65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while len(buf) >= HEADER.size:
                magic, ver, ptype, flags, pts, size = HEADER.unpack_from(buf)
                if magic != MAGIC or len(buf) < HEADER.size + size:
                    break
                payload = buf[HEADER.size : HEADER.size + size]
                buf = buf[HEADER.size + size :]
                if ptype == CONTROL:
                    text = payload.decode(errors="replace")
                    print(f"[fake-phone] control: {text}")
                    if '"start_stream"' in text:
                        self.start_event.set()
                elif ptype == TIMESYNC_REQ:
                    t2 = time.monotonic_ns()
                    self.send(TIMESYNC_RESP, 0, t2, struct.pack(">Q", pts))

    def audio_loop(self):
        rate = 48000
        chunk_frames = 480  # 10 ms
        phase = 0.0
        pts = time.monotonic_ns()
        while self.live:
            samples = bytearray()
            for _ in range(chunk_frames):
                v = int(12000 * math.sin(2 * math.pi * 440 * phase))
                samples += struct.pack("<hh", v, v)
                phase += 1 / rate
            self.send(SCREEN_AUDIO, 0, pts, bytes(samples))
            pts += chunk_frames * 1_000_000_000 // rate
            time.sleep(chunk_frames / rate)

    def find_sc(self, buf, i):
        """Next Annex B start code: returns (sc_start, payload_start)."""
        n = len(buf)
        while i < n - 2:
            if buf[i : i + 3] == b"\x00\x00\x01":
                sc_start = i - 1 if i > 0 and buf[i - 1] == 0 else i
                return sc_start, i + 3
            i += 1
        return None

    def video_loop(self):
        w, h, fps = self.args.width, self.args.height, self.args.fps
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-re", "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={fps}",
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
            "-pix_fmt", "yuv420p",
            "-x264-params", "scenecut=0:keyint=60:sliced-threads=0",
            "-f", "h264", "pipe:1",
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        frame_ns = 1_000_000_000 // fps

        pts = time.monotonic_ns()
        buf = b""
        au = []
        au_types = set()

        def flush():
            nonlocal au, au_types, pts
            if not au:
                return
            keyframe = 5 in au_types or 7 in au_types
            flags = FLAG_KEYFRAME if keyframe else 0
            try:
                self.send(VIDEO, flags, pts, b"".join(au))
            except OSError:
                proc.kill()
                self.live = False
                return
            au = []
            au_types = set()
            pts += frame_ns

        while self.live:
            data = proc.stdout.read(32768)
            if not data:
                break
            buf += data
            sc = self.find_sc(buf, 0)
            while sc:
                sc_start, payload_start = sc
                nxt = self.find_sc(buf, payload_start)
                if nxt is None:
                    break
                nal = buf[sc_start : nxt[0]]
                ntype = nal[payload_start - sc_start] & 0x1F
                if ntype in range(1, 6) and au_types & set(range(1, 6)):
                    flush()
                au.append(nal)
                au_types.add(ntype)
                sc = nxt
            buf = buf[sc[0] :] if sc else buf
            time.sleep(0.001)

        flush()
        proc.kill()

    def handle(self, conn):
        self.conn = conn
        self.live = True
        print("[fake-phone] client connected")

        hello = (
            '{"name":"Fake iPhone","app":"LensLink","protocol":1,'
            f'"kind":"camera","standby":{str(not self.args.no_standby).lower()}}}'
        )
        self.send_json(HELLO, hello)

        threading.Thread(target=self.reader, daemon=True).start()

        if self.args.no_standby:
            self.start_event.set()
        self.start_event.wait()
        if not self.live:
            return

        self.send_json(
            VIDEO_CONFIG,
            f'{{"codec":"h264","width":{self.args.width},'
            f'"height":{self.args.height},"fps":{self.args.fps},"kind":"camera"}}',
        )

        if not self.args.no_audio:
            threading.Thread(target=self.audio_loop, daemon=True).start()
        self.video_loop()
        self.live = False

    def serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self.args.port))
        srv.listen(1)
        print(f"[fake-phone] listening on 0.0.0.0:{self.args.port}")
        while True:
            conn, addr = srv.accept()
            try:
                self.handle(conn)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            self.live = False
            self.start_event.clear()
            print("[fake-phone] client disconnected")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=9979)
    p.add_argument("--width", type=int, default=640)
    p.add_argument("--height", type=int, default=360)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--no-standby", action="store_true")
    p.add_argument("--no-audio", action="store_true")
    FakePhone(p.parse_args()).serve()
