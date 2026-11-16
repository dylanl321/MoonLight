#pragma once

#include "platform/platform.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace mm::synced {

/// A device's time in step with a reference, and the scene the reference says is playing.
///
/// @technical
///
/// ## The offset
///
/// One NTP exchange (RFC 5905) gives four times: t1 the request leaves here, t2 it reaches the reference, t3 the reply leaves, t4 it arrives back. The offset is ((t2 - t1) + (t3 - t4)) / 2 and the round trip (t4 - t1) - (t3 - t2). The offset is exact when the two legs take equal time, and wrong by at most half the round trip when they do not, so of the last eight samples the one with the shortest round trip is the one trusted: NTP's own clock filter.
///
/// ## Stepping and slewing
///
/// A clock that jumps makes every animation jump. So the first lock, or an error past 128 ms (ntpd's step threshold), steps the offset at once; anything smaller is slewed, moved at most 1 ms per sample, so effects never see time run backward by more than that.
///
/// ## The shared state
///
/// The service that runs the exchange writes; the light pipeline reads, perhaps on another core. Both values a reader needs are single 32-bit atomics: the offset in milliseconds modulo 2^32, and the scene's start in synced milliseconds. Synced time is the reference's time in milliseconds, modulo 2^32, so it wraps as `millis()` does and every comparison takes differences.

/// The four times of one exchange, in microseconds: t1 and t4 on this device's clock, t2 and t3 on the reference's.
struct Exchange { int64_t t1, t2, t3, t4; };

/// What one exchange measured.
struct Sample {
    int64_t offsetUs = 0;     ///< add to this device's time to get the reference's
    int64_t roundTripUs = 0;  ///< the network's share of the exchange
};

/// One exchange's offset and round trip; a negative round trip (a reply the reference stamped faster than it could) is clamped to 0.
inline Sample measure(const Exchange& e) {
    Sample s;
    s.offsetUs = ((e.t2 - e.t1) + (e.t3 - e.t4)) / 2;
    s.roundTripUs = (e.t4 - e.t1) - (e.t3 - e.t2);
    if (s.roundTripUs < 0) s.roundTripUs = 0;
    return s;
}

/// NTP's clock filter: the last `kSize` samples, and the one with the shortest round trip among them.
class Filter {
public:
    static constexpr uint8_t kSize = 8;

    /// Keep `s`, dropping the oldest once full.
    void add(const Sample& s) {
        samples_[next_] = s;
        next_ = static_cast<uint8_t>((next_ + 1) % kSize);
        if (count_ < kSize) count_++;
    }
    /// The trusted sample, or false while there is none.
    bool best(Sample& out) const {
        if (count_ == 0) return false;
        out = samples_[0];
        for (uint8_t i = 1; i < count_; i++)
            if (samples_[i].roundTripUs < out.roundTripUs) out = samples_[i];
        return true;
    }
    /// How many samples it holds.
    uint8_t count() const { return count_; }
    /// Forget them all, for a new reference.
    void clear() { count_ = next_ = 0; }

private:
    Sample samples_[kSize] = {};
    uint8_t count_ = 0;
    uint8_t next_ = 0;
};

/// Step or slew from the offset in force toward a new estimate.
class Discipline {
public:
    static constexpr int64_t kStepUs = 128000;   ///< ntpd's step threshold
    static constexpr int64_t kSlewUs = 1000;     ///< the most one sample moves a slewed clock

    /// Move toward `targetUs`, answering whether this stepped.
    bool update(int64_t targetUs) {
        const int64_t err = targetUs - offsetUs_;
        if (!locked_ || err > kStepUs || err < -kStepUs) {
            offsetUs_ = targetUs;
            locked_ = true;
            return true;
        }
        offsetUs_ += err > kSlewUs ? kSlewUs : (err < -kSlewUs ? -kSlewUs : err);
        return false;
    }
    /// The offset in force, in microseconds.
    int64_t offsetUs() const { return offsetUs_; }
    /// Whether any sample has arrived since the last reset.
    bool locked() const { return locked_; }
    /// Unlock, for a new reference.
    void reset() { locked_ = false; offsetUs_ = 0; }

private:
    int64_t offsetUs_ = 0;
    bool locked_ = false;
};

/// This device's microseconds since boot, past `micros()`'s 71-minute wrap: called at least once per wrap, which every tick does.
inline int64_t localMicros() {
    static uint32_t last = 0;
    static int64_t high = 0;
    const uint32_t now = platform::micros();
    if (now < last) high += int64_t{1} << 32;
    last = now;
    return high + now;
}

/// The offset added to `millis()`, modulo 2^32: 0 until a reference is heard, so a device alone keeps its own time.
inline std::atomic<uint32_t>& offsetMs() {
    static std::atomic<uint32_t> v{0};
    return v;
}

/// Publish an offset in microseconds as the millisecond offset `millis()` takes. Synced ms are the reference's microseconds / 1000, so the offset is that minus this device's, both taken from the same `micros()` reading.
inline void publish(int64_t offsetUs) {
    const int64_t local = localMicros();
    const int64_t ref = local + offsetUs;
    const uint32_t refMs = static_cast<uint32_t>(static_cast<uint64_t>(ref / 1000));
    offsetMs().store(refMs - platform::millis(), std::memory_order_relaxed);
}

/// Synced time in milliseconds, modulo 2^32: the reference's when one is heard, this device's own otherwise.
inline uint32_t millis() { return platform::millis() + offsetMs().load(std::memory_order_relaxed); }

/// A reference time in microseconds as synced milliseconds, modulo 2^32.
inline uint32_t toSyncedMs(int64_t refUs) { return static_cast<uint32_t>(static_cast<uint64_t>(refUs / 1000)); }

/// The scene the reference says is playing: its name and its start in synced ms. A version counter tells a reader a new scene from a repeat of the same one.
struct Scene {
    char name[48] = "";
    uint32_t startMs = 0;
    uint32_t version = 0;
};

/// The one scene slot. Written by the clock service's tick and read by effects' ticks; on a board where those run on different cores a reader may see a name and start from two writes for one tick, which the next tick repairs.
inline Scene& scene() {
    static Scene s;
    return s;
}

/// Record a scene; only a change of name or start bumps the version, so a repeat costs a reader nothing.
inline void setScene(const char* name, uint32_t startMs) {
    Scene& s = scene();
    if (std::strcmp(s.name, name) == 0 && s.startMs == startMs) return;
    std::strncpy(s.name, name, sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = '\0';
    s.startMs = startMs;
    s.version++;
}

/// Where a scene stands at `nowMs`, for a player stepping `stepMs` per frame through `frameCount` frames.
struct Position {
    bool started = false;   ///< false before the start time
    bool ended = false;     ///< true past the last frame
    uint32_t frame = 0;     ///< the frame due now, while started and not ended
};

/// The frame a player should show at `nowMs` for a scene started at `startMs`; differences, so a wrap of either is harmless.
inline Position positionAt(uint32_t nowMs, uint32_t startMs, uint32_t stepMs, uint32_t frameCount) {
    Position p;
    const int32_t since = static_cast<int32_t>(nowMs - startMs);
    if (since < 0 || stepMs == 0) return p;
    p.started = true;
    const uint32_t f = static_cast<uint32_t>(since) / stepMs;
    if (f >= frameCount) { p.ended = true; return p; }
    p.frame = f;
    return p;
}

}  // namespace mm::synced
