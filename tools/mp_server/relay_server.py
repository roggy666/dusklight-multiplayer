#!/usr/bin/env python3
"""
Dusklight multiplayer relay server (Path B: native state-sync).

A tiny dependency-free TCP relay. Each client publishes its player transform a
few dozen times per second; the server keeps the latest state per client and
broadcasts a snapshot of everyone else to each client on a fixed tick. This is
NOT the original TPOnline lockstep server (that one is deterministic and tied to
PowerPC memory); this matches dusklight/src/dusk/net.cpp.

Wire protocol (matches net.cpp). Every message is framed:
    [u16 LE length][payload]      length counts payload bytes only
payload[0] = message type:
    0 Hello    C->S : u8 version, u8 nameLen, name[nameLen]
    1 Welcome  S->C : u8 id
    2 State    C->S : u32 sceneHash, s8 room, f32 x, f32 y, f32 z, s16 angleY, u16 anim
    3 Snapshot S->C : u8 count, count * { u8 id, u8 nameLen, name, <state> }
    4 ProgressDelta C->S : u8 full, u32 baseVersion, s32 rupeeField,
                           u8 spanCount, spanCount*{u16 off,u16 len,bytes}
    5 ProgressState S->C : u32 version, u8 region[SAVE_REGION_SIZE]
    6 Warp          S->C : /load cross-scene warp (Phase 3)
All integers little-endian. <state> = _STATE_FMT.

Usage:
    python relay_server.py [--host 0.0.0.0] [--port 10020] [--tick 30]
"""

import argparse
import selectors
import socket
import struct
import sys
import time

PROTOCOL_VERSION = 6
MAX_PLAYERS = 16
MAX_NAME = 15

MSG_HELLO = 0
MSG_WELCOME = 1
MSG_STATE = 2
MSG_SNAPSHOT = 3
MSG_PROGRESS_DELTA = 4   # C->S: shared inventory/world delta (or full seed)
MSG_PROGRESS_STATE = 5   # S->C: u32 version + full canonical save region
MSG_WARP = 6             # S->C: /load cross-scene warp (Phase 3)

# State payload after the type byte: u32 scene, s8 room, 3f, s16 angle, u16 anim
# (legs), u16 animUpper (arms), u8 costume, u8 flags, 24s eventName (cutscene),
# 8s stageName (real stage name for /save + warps), 3f horse pos, s16 horse angle,
# u16 horse anim, u8 seat (shared-Epona co-op)
_STATE_FMT = "<IbfffhHHBB24s8sfffhHB"
_STATE_LEN = struct.calcsize(_STATE_FMT)

# Shared progress: a whitelist of the live save block is mirrored here. The relay
# is authoritative — it holds the canonical region and merges client deltas
# (rupees additively, everything else per-byte) so concurrent pickups all count.
SAVE_REGION_SIZE = 0x8F0   # mPlayer + mSave[32] + mSave2[64] + mEvent
RUPEE_OFF = 0x04           # dSv_player_status_a_c::mRupee (big-endian u16)


def frame(payload: bytes) -> bytes:
    """Prefix a payload with its u16 little-endian length."""
    return struct.pack("<H", len(payload)) + payload


class Client:
    __slots__ = ("sock", "id", "name", "inbuf", "outbuf", "have_state", "state")

    def __init__(self, sock: socket.socket, pid: int):
        self.sock = sock
        self.id = pid
        self.name = f"P{pid}"
        self.inbuf = bytearray()
        self.outbuf = bytearray()
        self.have_state = False
        # (sceneHash, room, x, y, z, angleY, anim, animUpper, costume, flags,
        #  eventName, stageName, horseX, horseY, horseZ, horseAngleY, horseAnim, seat)
        self.state = (0, -1, 0.0, 0.0, 0.0, 0, 0, 0, 0, 0, b"", b"", 0.0, 0.0, 0.0, 0, 0, 0)

    def queue(self, data: bytes):
        self.outbuf += data


class RelayServer:
    def __init__(self, host: str, port: int, tick_hz: float):
        self.sel = selectors.DefaultSelector()
        self.clients: dict[socket.socket, Client] = {}
        self.next_id = 1
        self.tick_dt = 1.0 / max(1.0, tick_hz)

        # Canonical shared inventory/world progress (authoritative).
        self.progress = bytearray(SAVE_REGION_SIZE)
        self.progress_version = 0
        self.progress_seeded = False

        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((host, port))
        self.listener.listen(MAX_PLAYERS)
        self.listener.setblocking(False)
        self.sel.register(self.listener, selectors.EVENT_READ, None)
        print(f"[relay] listening on {host}:{port}, {tick_hz:.0f} Hz tick")

    # --- connection lifecycle ---
    def accept(self):
        try:
            sock, addr = self.listener.accept()
        except BlockingIOError:
            return
        if len(self.clients) >= MAX_PLAYERS:
            sock.close()
            return
        sock.setblocking(False)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        client = Client(sock, self.next_id)
        self.next_id = self.next_id + 1
        if self.next_id > 250:
            self.next_id = 1
        self.clients[sock] = client
        self.sel.register(sock, selectors.EVENT_READ | selectors.EVENT_WRITE, client)
        print(f"[relay] {addr} connected as id {client.id}")

    def drop(self, client: Client):
        print(f"[relay] id {client.id} ({client.name}) disconnected")
        try:
            self.sel.unregister(client.sock)
        except KeyError:
            pass
        client.sock.close()
        self.clients.pop(client.sock, None)

    # --- incoming data ---
    def on_readable(self, client: Client):
        try:
            data = client.sock.recv(4096)
        except (BlockingIOError, InterruptedError):
            return True
        except OSError:
            return False
        if not data:
            return False
        client.inbuf += data
        self.parse(client)
        return True

    def parse(self, client: Client):
        buf = client.inbuf
        off = 0
        while len(buf) - off >= 2:
            (length,) = struct.unpack_from("<H", buf, off)
            if len(buf) - off - 2 < length:
                break
            payload = bytes(buf[off + 2: off + 2 + length])
            off += 2 + length
            self.handle(client, payload)
        if off:
            del buf[:off]

    def handle(self, client: Client, payload: bytes):
        if not payload:
            return
        msg = payload[0]
        body = payload[1:]
        if msg == MSG_HELLO:
            if len(body) >= 2:
                nlen = body[1]
                name = body[2:2 + nlen].decode("utf-8", "replace")[:MAX_NAME]
                if name:
                    client.name = name
            client.queue(frame(bytes([MSG_WELCOME, client.id])))
            print(f"[relay] id {client.id} hello as '{client.name}'")
            # A joining client adopts the shared world: hand it the canonical
            # progress immediately (if we have one). Its own full snapshot will be
            # ignored below since we're already seeded.
            if self.progress_seeded:
                client.queue(self.build_progress_state())
        elif msg == MSG_STATE:
            if len(body) >= _STATE_LEN:
                client.state = struct.unpack_from(_STATE_FMT, body, 0)
                client.have_state = True
        elif msg == MSG_PROGRESS_DELTA:
            self.handle_progress(client, body)
        # unknown messages ignored

    # --- shared progress ---
    def build_progress_state(self) -> bytes:
        return frame(bytes([MSG_PROGRESS_STATE]) +
                     struct.pack("<I", self.progress_version) + bytes(self.progress))

    def broadcast_progress(self):
        state = self.build_progress_state()
        for c in self.clients.values():
            c.queue(state)

    @staticmethod
    def _parse_spans(body: bytes, off: int, count: int):
        spans = []
        for _ in range(count):
            if len(body) - off < 4:
                break
            (spoff, splen) = struct.unpack_from("<HH", body, off)
            off += 4
            if len(body) - off < splen:
                break
            spans.append((spoff, body[off:off + splen]))
            off += splen
        return spans

    def handle_progress(self, client: Client, body: bytes):
        # body: u8 full, u32 baseVersion, s32 rupeeField, u8 spanCount, spans...
        if len(body) < 10:
            return
        full = body[0]
        (_base_version,) = struct.unpack_from("<I", body, 1)
        (rupee_field,) = struct.unpack_from("<i", body, 5)
        span_count = body[9]
        spans = self._parse_spans(body, 10, span_count)

        if full and not self.progress_seeded:
            # First/main player defines the world.
            self.progress = bytearray(SAVE_REGION_SIZE)
            self._apply_spans(spans)
            self._set_rupee(max(0, min(0xFFFF, rupee_field)))   # absolute seed value
            self.progress_seeded = True
            self.progress_version = 1
            print(f"[relay] progress seeded by id {client.id} (rupees={rupee_field})")
            self.broadcast_progress()
        elif full and self.progress_seeded:
            # Already have a world; this joiner adopts ours (sent on hello). Ignore.
            pass
        elif not full and self.progress_seeded:
            self._apply_spans(spans)
            cur = (self.progress[RUPEE_OFF] << 8) | self.progress[RUPEE_OFF + 1]
            self._set_rupee(max(0, min(0xFFFF, cur + rupee_field)))  # additive merge
            self.progress_version += 1
            self.broadcast_progress()

    def _apply_spans(self, spans):
        for off, data in spans:
            if 0 <= off and off + len(data) <= SAVE_REGION_SIZE:
                self.progress[off:off + len(data)] = data

    def _set_rupee(self, value: int):
        self.progress[RUPEE_OFF] = (value >> 8) & 0xFF
        self.progress[RUPEE_OFF + 1] = value & 0xFF

    # --- outgoing snapshots ---
    def build_snapshot(self, exclude: Client) -> bytes:
        entries = []
        count = 0
        for c in self.clients.values():
            if c is exclude or not c.have_state:
                continue
            name = c.name.encode("utf-8")[:MAX_NAME]
            entries.append(bytes([c.id, len(name)]) + name +
                           struct.pack(_STATE_FMT, *c.state))
            count += 1
            if count >= MAX_PLAYERS:
                break
        return frame(bytes([MSG_SNAPSHOT, count]) + b"".join(entries))

    def broadcast(self):
        for client in self.clients.values():
            client.queue(self.build_snapshot(client))

    def on_writable(self, client: Client):
        if not client.outbuf:
            return True
        try:
            sent = client.sock.send(client.outbuf)
            del client.outbuf[:sent]
        except (BlockingIOError, InterruptedError):
            return True
        except OSError:
            return False
        return True

    # --- main loop ---
    def run(self):
        next_tick = time.monotonic()
        while True:
            timeout = max(0.0, next_tick - time.monotonic())
            for key, mask in self.sel.select(timeout=timeout):
                client = key.data
                if client is None:
                    self.accept()
                    continue
                if mask & selectors.EVENT_READ and not self.on_readable(client):
                    self.drop(client)
                    continue
                if mask & selectors.EVENT_WRITE and not self.on_writable(client):
                    self.drop(client)

            now = time.monotonic()
            if now >= next_tick:
                self.broadcast()
                next_tick = now + self.tick_dt


def main(argv):
    ap = argparse.ArgumentParser(description="Dusklight multiplayer relay server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=10020)
    ap.add_argument("--tick", type=float, default=30.0, help="snapshot broadcast rate (Hz)")
    args = ap.parse_args(argv)
    try:
        RelayServer(args.host, args.port, args.tick).run()
    except KeyboardInterrupt:
        print("\n[relay] shutting down")
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
