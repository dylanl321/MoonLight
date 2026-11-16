# /// script
# dependencies = ["pytest"]
# ///
"""The show clock's sender: the same bytes the firmware's ClockPacket reads, and a ping answered over a real socket."""

import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "run"))

import show_clock  # noqa: E402


def test_a_ping_matches_the_firmware_golden_bytes():
    """unit_ClockPacket.cpp pins the same 28 bytes for a ping at 1.5 s."""
    assert show_clock.encode_ping(1_500_000) == (b"/mm/clock/ping\0\0" b",t\0\0" b"\0\0\0\x01\x80\0\0\0")


def test_timetags_round_trip_microseconds():
    for us in (0, 1, 999_999, 3_976_000_000_123_456):
        assert show_clock.from_timetag(show_clock.to_timetag(us)) == us


def test_a_scene_is_its_address_tags_name_and_start():
    pkt = show_clock.encode_scene("Snowfall", 7_000_000)
    assert pkt.startswith(b"/mm/show/scene\0\0,st\0Snowfall\0\0\0\0")
    assert show_clock.from_timetag(pkt[-8:]) == 7_000_000
    assert len(pkt) % 4 == 0


def test_only_a_well_formed_ping_is_read_as_one():
    assert show_clock.parse_ping(show_clock.encode_ping(42)) == 42
    assert show_clock.parse_ping(show_clock.encode_pong(1, 2, 3)) is None
    assert show_clock.parse_ping(show_clock.encode_ping(42)[:-1]) is None


def test_a_ping_over_the_network_comes_back_a_pong_with_the_reference_times():
    clock = show_clock.ShowClock([], port=0, bind="127.0.0.1")
    port = clock.sock.getsockname()[1]
    board = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    board.settimeout(2)
    board.sendto(show_clock.encode_ping(123), ("127.0.0.1", port))
    clock.answer()
    pkt, _ = board.recvfrom(256)
    head = b"/mm/clock/pong\0\0,ttt\0\0\0\0"
    assert pkt.startswith(head)
    t1, t2, t3 = (show_clock.from_timetag(pkt[len(head) + 8 * i:len(head) + 8 * i + 8]) for i in range(3))
    assert t1 == 123
    assert abs(t2 - show_clock.now_us()) < 2_000_000
    assert t3 >= t2


def test_play_starts_a_scene_a_lead_time_ahead_and_sends_it_to_each_target():
    board = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    board.bind(("127.0.0.1", 0))
    board.settimeout(2)
    clock = show_clock.ShowClock(["127.0.0.1"], port=0, lead_ms=300, bind="127.0.0.1")
    clock.port = board.getsockname()[1]   # send to the board's port
    before = show_clock.now_us()
    clock.play("Ice Palace")
    pkt, _ = board.recvfrom(256)
    assert b"Ice Palace" in pkt
    start = show_clock.from_timetag(pkt[-8:])
    assert before + 250_000 <= start <= show_clock.now_us() + 350_000
