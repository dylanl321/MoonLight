/// @module ShowClockService
/// @also ClockPacket, SyncedClock

/// The service's handling of each message, driven through its seam with no socket: it answers any asker, takes the time and the scene only from its reference, and steps its clock on the first reply.

#include "doctest.h"
#include "core/services/ShowClockService.h"

#include <cstring>
#include <vector>

using namespace mm;

namespace {

/// The service with its sends captured.
struct Captured : ShowClockService {
    struct Sent { uint8_t ip[4]; uint16_t port; std::vector<uint8_t> pkt; };
    std::vector<Sent> sent;
    void transmit(const uint8_t ip[4], uint16_t toPort, const uint8_t* pkt, size_t len) override {
        Sent s{{ip[0], ip[1], ip[2], ip[3]}, toPort, std::vector<uint8_t>(pkt, pkt + len)};
        sent.push_back(s);
    }
};

const uint8_t kRef[4] = {10, 0, 1, 18};
const uint8_t kStranger[4] = {10, 0, 1, 99};

}  // namespace

TEST_CASE("A ping from anyone is answered with a pong to its sender, echoing its t1") {
    Captured svc;
    uint8_t pkt[64];
    const size_t len = clockpkt::encodePing(pkt, sizeof(pkt), 777);
    svc.handle(pkt, len, kStranger, 50000, 1000000);
    REQUIRE(svc.sent.size() == 1);
    CHECK(svc.sent[0].port == 50000);
    CHECK(std::memcmp(svc.sent[0].ip, kStranger, 4) == 0);
    clockpkt::Parsed m;
    REQUIRE(clockpkt::parse(svc.sent[0].pkt.data(), svc.sent[0].pkt.size(), m));
    CHECK(m.kind == clockpkt::Kind::Pong);
    CHECK(m.t1 == 777);
    CHECK(m.t2 == 1000000);
}

TEST_CASE("A pong from the reference locks the clock to its offset") {
    Captured svc;
    svc.setReference("10.0.1.18");
    uint8_t pkt[64];
    // Sent at 1 s here, the reference 60 s ahead, 200 us each way.
    const size_t len = clockpkt::encodePong(pkt, sizeof(pkt), 1000000, 61000200, 61000250);
    svc.handle(pkt, len, kRef, clockpkt::kDefaultPort, 1000450);
    CHECK(svc.locked());
    CHECK(svc.offsetUs() == 60000000);
    synced::offsetMs().store(0);
}

TEST_CASE("A pong or a scene from anyone but the reference is ignored") {
    Captured svc;
    svc.setReference("10.0.1.18");
    synced::setScene("", 0);
    uint8_t pkt[64];
    size_t len = clockpkt::encodePong(pkt, sizeof(pkt), 0, 99000000, 99000000);
    svc.handle(pkt, len, kStranger, clockpkt::kDefaultPort, 10);
    CHECK_FALSE(svc.locked());
    len = clockpkt::encodeScene(pkt, sizeof(pkt), "Rogue", 5);
    svc.handle(pkt, len, kStranger, clockpkt::kDefaultPort, 10);
    CHECK(synced::scene().name[0] == '\0');
}

TEST_CASE("A scene from the reference becomes the scene, its start in synced milliseconds") {
    Captured svc;
    svc.setReference("10.0.1.18");
    uint8_t pkt[64];
    const size_t len = clockpkt::encodeScene(pkt, sizeof(pkt), "Snowfall", int64_t{42000} * 1000);
    svc.handle(pkt, len, kRef, clockpkt::kDefaultPort, 10);
    CHECK(std::strcmp(synced::scene().name, "Snowfall") == 0);
    CHECK(synced::scene().startMs == 42000);
    synced::setScene("", 0);
}

TEST_CASE("A new reference forgets the old one's samples, so its first reply steps the clock") {
    Captured svc;
    svc.setReference("10.0.1.18");
    uint8_t pkt[64];
    size_t len = clockpkt::encodePong(pkt, sizeof(pkt), 0, 5000000, 5000000);
    svc.handle(pkt, len, kRef, clockpkt::kDefaultPort, 0);
    REQUIRE(svc.offsetUs() == 5000000);
    svc.setReference("10.0.1.99");
    CHECK_FALSE(svc.locked());
    len = clockpkt::encodePong(pkt, sizeof(pkt), 0, 1000, 1000);
    svc.handle(pkt, len, kStranger, clockpkt::kDefaultPort, 0);
    CHECK(svc.offsetUs() == 1000);
    synced::offsetMs().store(0);
}
