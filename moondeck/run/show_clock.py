#!/usr/bin/env python3
"""Lead a show clock: answer MoonLight boards' clock requests and tell them which scene plays, so boards apart render as one show.

Boards running ShowClockService with this computer as their reference keep their clocks on its time (true NTP time,
from the system clock) and follow the scene it names: an FSEQ player in `follow` mode plays the playlist file named
after the scene, at the frame due now. Standard library only.

    python3 show_clock.py --targets 10.0.1.143,10.0.0.68 --scene "Ice Palace"
    python3 show_clock.py --targets 10.0.1.255 --playlist "Snowfall,Candy Cane" --seconds 90

A scene is state, repeated every second, so a board that reboots or joins late reads where the scene is from its
start time. A new scene starts a little in the future (`--lead`), so boards that hear it at different moments switch
together. The messages are plain OSC; ClockPacket.h in the firmware has the layout.
"""

from __future__ import annotations

import argparse
import itertools
import select
import socket
import struct
import time

PORT = 9002
PING = b"/mm/clock/ping"
PONG = b"/mm/clock/pong"
SCENE = b"/mm/show/scene"
NTP_UNIX_DELTA_US = 2208988800 * 1_000_000   # 1900 to 1970: timetags carry NTP time


def now_us() -> int:
    """This computer's time as NTP microseconds."""
    return time.time_ns() // 1000 + NTP_UNIX_DELTA_US


def osc_string(s: bytes) -> bytes:
    """An OSC string: NUL-terminated, padded to four bytes."""
    return s + b"\0" * (4 - len(s) % 4)


def to_timetag(us: int) -> bytes:
    sec, rem = divmod(max(us, 0), 1_000_000)
    return struct.pack(">II", sec & 0xFFFFFFFF, (rem << 32) // 1_000_000)


def from_timetag(b: bytes) -> int:
    sec, frac = struct.unpack(">II", b)
    return sec * 1_000_000 + ((frac * 1_000_000 + 0x80000000) >> 32)


def encode_ping(t1: int) -> bytes:
    return osc_string(PING) + osc_string(b",t") + to_timetag(t1)


def encode_pong(t1: int, t2: int, t3: int) -> bytes:
    return osc_string(PONG) + osc_string(b",ttt") + to_timetag(t1) + to_timetag(t2) + to_timetag(t3)


def encode_scene(name: str, start_us: int) -> bytes:
    return osc_string(SCENE) + osc_string(b",st") + osc_string(name.encode()) + to_timetag(start_us)


def parse_ping(pkt: bytes) -> int | None:
    """A ping's t1, or None for anything else."""
    head = osc_string(PING) + osc_string(b",t")
    if len(pkt) != len(head) + 8 or not pkt.startswith(head):
        return None
    return from_timetag(pkt[len(head):])


class ShowClock:
    """The reference: answers pings on a socket, and repeats the current scene to the targets."""

    def __init__(self, targets: list[str], port: int = PORT, lead_ms: int = 300, bind: str = "0.0.0.0"):
        self.targets = targets
        self.port = port
        self.lead_us = lead_ms * 1000
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind((bind, port))
        self.scene = ""
        self.start_us = 0

    def play(self, name: str) -> None:
        """Start `name` a lead time from now, and tell the boards at once."""
        self.scene = name
        self.start_us = now_us() + self.lead_us
        self.announce()

    def announce(self) -> None:
        pkt = encode_scene(self.scene, self.start_us)
        for host in self.targets:
            self.sock.sendto(pkt, (host, self.port))

    def serve(self, seconds: float) -> None:
        """Answer pings for `seconds`, announcing the scene once a second."""
        end = time.monotonic() + seconds
        next_announce = time.monotonic()
        while (left := end - time.monotonic()) > 0:
            if time.monotonic() >= next_announce:
                self.announce()
                next_announce += 1.0
            ready, _, _ = select.select([self.sock], [], [], min(left, max(next_announce - time.monotonic(), 0)))
            if ready:
                self.answer()

    def answer(self) -> None:
        pkt, addr = self.sock.recvfrom(256)
        t2 = now_us()
        t1 = parse_ping(pkt)
        if t1 is not None:
            self.sock.sendto(encode_pong(t1, t2, now_us()), addr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--targets", required=True, help="comma-separated board addresses, or a broadcast address")
    ap.add_argument("--scene", default="", help="the scene to play; empty plays nothing")
    ap.add_argument("--playlist", default="", help="comma-separated scenes to cycle through instead of --scene")
    ap.add_argument("--seconds", type=float, default=60.0, help="how long each playlist scene plays")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--lead", type=int, default=300, help="milliseconds between announcing a scene and its start")
    a = ap.parse_args(argv)
    clock = ShowClock([t.strip() for t in a.targets.split(",") if t.strip()], a.port, a.lead)
    scenes = [s.strip() for s in a.playlist.split(",") if s.strip()]
    try:
        if not scenes:
            clock.play(a.scene)
            print(f"leading on port {a.port}, scene {a.scene!r}")
            clock.serve(float("inf"))
        for name in itertools.cycle(scenes):
            print(f"scene {name!r}")
            clock.play(name)
            clock.serve(a.seconds)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
