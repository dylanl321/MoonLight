/// @module ShowClockService
/// @also SyncedClock

/// The clock arithmetic: the NTP offset, the filter that trusts the shortest round trip, stepping and slewing, and the frame a scene's start time says is due, across the 32-bit wrap.

#include "doctest.h"
#include "core/util/SyncedClock.h"

using namespace mm;

TEST_CASE("One exchange gives the offset and the round trip, exactly when the two legs are equal") {
    // The reference runs 1000 us ahead; each leg takes 200 us; the reference holds the request 50 us.
    const synced::Sample s = synced::measure({0, 1200, 1250, 450});
    CHECK(s.offsetUs == 1000);
    CHECK(s.roundTripUs == 400);
}

TEST_CASE("Unequal legs err by at most half the round trip") {
    // Out takes 100 us, back 500 us; true offset 1000.
    const synced::Sample s = synced::measure({0, 1100, 1100, 600});
    CHECK(s.roundTripUs == 600);
    CHECK(s.offsetUs >= 1000 - 300);
    CHECK(s.offsetUs <= 1000 + 300);
}

TEST_CASE("The filter trusts the sample with the shortest round trip of the last eight") {
    synced::Filter f;
    synced::Sample best;
    CHECK_FALSE(f.best(best));
    f.add({5000, 9000});
    f.add({1000, 300});    // the quiet one
    f.add({4000, 7000});
    REQUIRE(f.best(best));
    CHECK(best.offsetUs == 1000);
    for (int i = 0; i < 8; i++) f.add({2000, 800});   // pushes the quiet one out
    REQUIRE(f.best(best));
    CHECK(best.offsetUs == 2000);
    CHECK(f.count() == synced::Filter::kSize);
}

TEST_CASE("The first lock and a large error step the clock; a small error slews at most 1 ms a sample") {
    synced::Discipline d;
    CHECK(d.update(5000000));   // first lock steps
    CHECK(d.offsetUs() == 5000000);
    CHECK_FALSE(d.update(5000400));   // small: all of it, under the slew limit
    CHECK(d.offsetUs() == 5000400);
    CHECK_FALSE(d.update(5050000));   // 49.6 ms off: slewed 1 ms
    CHECK(d.offsetUs() == 5001400);
    CHECK(d.update(9000000));   // past 128 ms: steps
    CHECK(d.offsetUs() == 9000000);
    d.reset();
    CHECK_FALSE(d.locked());
}

TEST_CASE("A scene's position: nothing before the start, the due frame during it, and ended after") {
    using synced::positionAt;
    CHECK_FALSE(positionAt(990, 1000, 25, 40).started);
    const auto p = positionAt(1000 + 260, 1000, 25, 40);
    CHECK(p.started);
    CHECK(p.frame == 10);
    CHECK(positionAt(1000 + 1000, 1000, 25, 40).ended);   // 40 frames of 25 ms are over at 1000 ms
}

TEST_CASE("A scene's position holds across the 32-bit millisecond wrap") {
    const uint32_t start = 0xFFFFFF00u;
    const auto p = synced::positionAt(start + 512, start, 50, 100);   // now has wrapped past 0
    CHECK(p.started);
    CHECK(p.frame == 10);
}

TEST_CASE("Only a new name or start bumps the scene version, so a repeat costs a reader nothing") {
    synced::setScene("", 0);
    const uint32_t v0 = synced::scene().version;
    synced::setScene("Snowfall", 1234);
    CHECK(synced::scene().version == v0 + 1);
    synced::setScene("Snowfall", 1234);   // the repeat every sender makes
    CHECK(synced::scene().version == v0 + 1);
    synced::setScene("Snowfall", 9999);   // the same scene started again
    CHECK(synced::scene().version == v0 + 2);
    synced::setScene("", 0);
}

TEST_CASE("Publishing an offset moves synced time by that much") {
    synced::publish(0);
    const uint32_t own = synced::millis();
    synced::publish(int64_t{5000} * 1000);   // 5 s ahead, on top of whatever millis() and micros() differ by
    const int32_t moved = static_cast<int32_t>(synced::millis() - own);
    CHECK(moved >= 4990);
    CHECK(moved <= 5010);
    synced::offsetMs().store(0);
}
