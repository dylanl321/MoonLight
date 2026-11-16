#pragma once

#include "core/services/OscPacket.h"   // pad4, stringLen, beInt32: the one OSC layout

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::clockpkt {

/// The show clock's three OSC messages: a clock request, its reply, and the scene a group plays.
///
/// @technical
///
/// ## The messages
///
/// Each is a plain OSC 1.0 message, so any OSC library can send or read one, and times are OSC timetags (type `t`): 64-bit NTP time, seconds since 1900 in the high word and a binary fraction in the low.
///
/// | Address | Tags | Arguments |
/// |---------|------|-----------|
/// | `/mm/clock/ping` | `,t` | t1, the asker's send time on its own clock |
/// | `/mm/clock/pong` | `,ttt` | t1 echoed, t2 the reference's receive time, t3 its send time |
/// | `/mm/show/scene` | `,st` | the scene's name, and when it started on the reference's clock |
///
/// The ping and pong are the NTP exchange (RFC 5905): the asker stamps t4 on arrival, and the four times give both the offset and the round trip. The scene is state, not a command: the sender repeats it, a lost packet is repaired by the next, and a board joining late reads where the scene is from its start time. An empty name means nothing plays.
///
/// ## Parsing is the security surface
///
/// Every read is length-checked against the datagram, as OscPacket's are, and nothing is allocated. A scene name is copied into the caller's fixed buffer and cut to fit.

/// The default port: beside OSC's 9000 and 9001, so a board can listen to both.
inline constexpr uint16_t kDefaultPort = 9002;

inline constexpr char kPing[] = "/mm/clock/ping";
inline constexpr char kPong[] = "/mm/clock/pong";
inline constexpr char kScene[] = "/mm/show/scene";

/// Microseconds as an NTP timetag, the epoch the caller's; a negative time is not representable and reads as 0.
inline uint64_t toTimetag(int64_t us) {
    if (us < 0) return 0;
    const uint64_t u = static_cast<uint64_t>(us);
    const uint64_t sec = u / 1000000u;
    const uint64_t frac = ((u % 1000000u) << 32) / 1000000u;
    return (sec << 32) | frac;
}

/// An NTP timetag as microseconds, rounded to the nearest.
inline int64_t fromTimetag(uint64_t t) {
    const uint64_t sec = t >> 32;
    const uint64_t frac = t & 0xFFFFFFFFu;
    return static_cast<int64_t>(sec * 1000000u + ((frac * 1000000u + 0x80000000u) >> 32));
}

/// A big-endian 64-bit load.
inline uint64_t be64(const uint8_t* p) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(osc::beInt32(p))) << 32) |
           static_cast<uint32_t>(osc::beInt32(p + 4));
}

/// A big-endian 64-bit store.
inline void put64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

/// What a datagram held, when it was one of the three.
enum class Kind : uint8_t { None, Ping, Pong, Scene };

/// One parsed message: the times in microseconds on the sender's epoch, and the scene's name.
struct Parsed {
    Kind kind = Kind::None;
    int64_t t1 = 0, t2 = 0, t3 = 0;   ///< ping: t1; pong: t1, t2, t3; scene: t1 is the start
    char name[48] = "";               ///< the scene's name, cut to fit
};

/// Read the address and tag string; answers where the arguments start and how many bytes remain, or false.
inline bool header(const uint8_t* pkt, size_t len, const char*& address, const char*& tags, size_t& at) {
    if (!pkt || len < 8 || pkt[0] != '/') return false;
    const size_t addrLen = osc::stringLen(pkt, len);
    if (addrLen == 0 || addrLen >= len) return false;
    const size_t tagsLen = osc::stringLen(pkt + addrLen, len - addrLen);
    if (tagsLen == 0 || tagsLen > len - addrLen || pkt[addrLen] != ',') return false;
    address = reinterpret_cast<const char*>(pkt);
    tags = reinterpret_cast<const char*>(pkt + addrLen);
    at = addrLen + tagsLen;
    return true;
}

/// Read `count` timetags from `at` into `out`, as microseconds.
inline bool timetags(const uint8_t* pkt, size_t len, size_t at, int64_t* out, int count) {
    if (len < at || len - at < static_cast<size_t>(count) * 8) return false;
    for (int i = 0; i < count; i++) out[i] = fromTimetag(be64(pkt + at + 8 * i));
    return true;
}

/// Read a scene: its name, then its start.
inline bool scene(const uint8_t* pkt, size_t len, size_t at, Parsed& out) {
    if (at >= len) return false;
    const size_t n = osc::stringLen(pkt + at, len - at);
    if (n == 0 || n > len - at) return false;
    const size_t chars = std::strlen(reinterpret_cast<const char*>(pkt + at));
    const size_t keep = chars < sizeof(out.name) - 1 ? chars : sizeof(out.name) - 1;
    std::memcpy(out.name, pkt + at, keep);
    out.name[keep] = '\0';
    return timetags(pkt, len, at + n, &out.t1, 1);
}

/// Parse one datagram; false for anything that is not one of the three, having read nothing past `len`.
inline bool parse(const uint8_t* pkt, size_t len, Parsed& out) {
    const char* address = nullptr;
    const char* tags = nullptr;
    size_t at = 0;
    if (!header(pkt, len, address, tags, at)) return false;
    if (std::strcmp(address, kPing) == 0 && std::strcmp(tags, ",t") == 0) {
        out.kind = Kind::Ping;
        return timetags(pkt, len, at, &out.t1, 1);
    }
    if (std::strcmp(address, kPong) == 0 && std::strcmp(tags, ",ttt") == 0) {
        int64_t t[3];
        if (!timetags(pkt, len, at, t, 3)) return false;
        out.kind = Kind::Pong;
        out.t1 = t[0]; out.t2 = t[1]; out.t3 = t[2];
        return true;
    }
    if (std::strcmp(address, kScene) == 0 && std::strcmp(tags, ",st") == 0) {
        out.kind = Kind::Scene;
        return scene(pkt, len, at, out);
    }
    return false;
}

/// Write an address and tag string, answering where the arguments go, or 0 when `cap` is too small for `args` more bytes.
inline size_t begin(uint8_t* out, size_t cap, const char* address, const char* tags, size_t args) {
    const size_t a = osc::pad4(std::strlen(address) + 1);
    const size_t t = osc::pad4(std::strlen(tags) + 1);
    if (!out || a + t + args > cap) return 0;
    std::memset(out, 0, a + t + args);
    std::memcpy(out, address, std::strlen(address));
    std::memcpy(out + a, tags, std::strlen(tags));
    return a + t;
}

/// A ping stamped `t1`; its length, or 0 when it will not fit.
inline size_t encodePing(uint8_t* out, size_t cap, int64_t t1) {
    const size_t at = begin(out, cap, kPing, ",t", 8);
    if (!at) return 0;
    put64(out + at, toTimetag(t1));
    return at + 8;
}

/// A pong answering `t1`, received at `t2` and sent at `t3`.
inline size_t encodePong(uint8_t* out, size_t cap, int64_t t1, int64_t t2, int64_t t3) {
    const size_t at = begin(out, cap, kPong, ",ttt", 24);
    if (!at) return 0;
    put64(out + at, toTimetag(t1));
    put64(out + at + 8, toTimetag(t2));
    put64(out + at + 16, toTimetag(t3));
    return at + 24;
}

/// A scene named `name`, started at `start`.
inline size_t encodeScene(uint8_t* out, size_t cap, const char* name, int64_t start) {
    const size_t n = osc::pad4(std::strlen(name) + 1);
    const size_t at = begin(out, cap, kScene, ",st", n + 8);
    if (!at) return 0;
    std::memcpy(out + at, name, std::strlen(name));
    put64(out + at + n, toTimetag(start));
    return at + n + 8;
}

}  // namespace mm::clockpkt
