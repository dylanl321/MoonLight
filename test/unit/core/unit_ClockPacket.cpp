/// @module ShowClockService
/// @also ClockPacket

/// The show clock's three OSC messages: pinned by golden bytes, so a sender written in any language can be checked against them, and by the length checks a datagram off the LAN needs.

#include "doctest.h"
#include "core/services/ClockPacket.h"

#include <cstring>
#include <string>
#include <vector>

using namespace mm;

TEST_CASE("A timetag round-trips microseconds, seconds in the high word and a binary fraction in the low") {
    CHECK(clockpkt::toTimetag(1500000) == ((uint64_t{1} << 32) | 0x80000000u));   // 1.5 s
    for (int64_t us : {int64_t{0}, int64_t{1}, int64_t{999999}, int64_t{3976000000123456}})
        CHECK(clockpkt::fromTimetag(clockpkt::toTimetag(us)) == us);
    CHECK(clockpkt::toTimetag(-5) == 0);
}

TEST_CASE("A ping is the OSC message /mm/clock/ping with one timetag, byte for byte") {
    uint8_t pkt[64];
    const size_t len = clockpkt::encodePing(pkt, sizeof(pkt), 1500000);
    const std::vector<uint8_t> golden = {
        '/', 'm', 'm', '/', 'c', 'l', 'o', 'c', 'k', '/', 'p', 'i', 'n', 'g', 0, 0,
        ',', 't', 0, 0,
        0, 0, 0, 1, 0x80, 0, 0, 0};
    REQUIRE(len == golden.size());
    CHECK(std::memcmp(pkt, golden.data(), len) == 0);
}

TEST_CASE("A pong carries the three times back, and a scene its name and start") {
    uint8_t pkt[96];
    clockpkt::Parsed m;
    size_t len = clockpkt::encodePong(pkt, sizeof(pkt), 10, 2000020, 2000030);
    REQUIRE(clockpkt::parse(pkt, len, m));
    CHECK(m.kind == clockpkt::Kind::Pong);
    CHECK(m.t1 == 10);
    CHECK(m.t2 == 2000020);
    CHECK(m.t3 == 2000030);

    len = clockpkt::encodeScene(pkt, sizeof(pkt), "Ice Palace", 7000000);
    m = {};
    REQUIRE(clockpkt::parse(pkt, len, m));
    CHECK(m.kind == clockpkt::Kind::Scene);
    CHECK(std::strcmp(m.name, "Ice Palace") == 0);
    CHECK(m.t1 == 7000000);
}

TEST_CASE("An empty scene name is a scene: nothing plays") {
    uint8_t pkt[64];
    clockpkt::Parsed m;
    const size_t len = clockpkt::encodeScene(pkt, sizeof(pkt), "", 0);
    REQUIRE(clockpkt::parse(pkt, len, m));
    CHECK(m.kind == clockpkt::Kind::Scene);
    CHECK(m.name[0] == '\0');
}

TEST_CASE("A long scene name is cut to fit, not run past the buffer") {
    uint8_t pkt[160];
    const std::string longName(100, 'x');
    clockpkt::Parsed m;
    const size_t len = clockpkt::encodeScene(pkt, sizeof(pkt), longName.c_str(), 1);
    REQUIRE(clockpkt::parse(pkt, len, m));
    CHECK(std::strlen(m.name) == sizeof(m.name) - 1);
}

TEST_CASE("Anything else is refused: other addresses, wrong tags, and every truncation") {
    uint8_t pkt[96];
    clockpkt::Parsed m;
    const size_t len = clockpkt::encodePong(pkt, sizeof(pkt), 1, 2, 3);
    for (size_t cut = 0; cut < len; cut++) CHECK_FALSE(clockpkt::parse(pkt, cut, m));

    uint8_t other[32] = {'/', 'm', 'm', '/', 'x', 0, 0, 0, ',', 't', 0, 0};
    CHECK_FALSE(clockpkt::parse(other, 20, m));

    clockpkt::encodePing(pkt, sizeof(pkt), 5);
    pkt[17] = 'i';   // ",i": the right address with the wrong argument
    CHECK_FALSE(clockpkt::parse(pkt, 28, m));
    CHECK_FALSE(clockpkt::parse(nullptr, 28, m));
}

TEST_CASE("An encoder answers 0 when the buffer is too small") {
    uint8_t pkt[16];
    CHECK(clockpkt::encodePing(pkt, sizeof(pkt), 1) == 0);
    CHECK(clockpkt::encodePong(pkt, sizeof(pkt), 1, 2, 3) == 0);
    CHECK(clockpkt::encodeScene(pkt, sizeof(pkt), "a", 1) == 0);
}
