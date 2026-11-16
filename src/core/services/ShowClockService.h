#pragma once

#include "core/module/MoonModule.h"
#include "core/services/ClockPacket.h"
#include "core/util/Ipv4.h"
#include "core/util/SyncedClock.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Keeps this device's clock in step with a reference, and follows the scene the reference plays, so boards apart render as one show.
///
/// Every effect animates from the layer's clock, and this service moves that clock onto the reference's time. Two boards following one reference then draw a time-based effect the same way at the same moment, and an FSEQ player following the scene shows the same frame on each.
///
/// @guide
///
/// ## Following a reference
///
/// Turn on `listen` and name the reference by address: a show computer running `moondeck/run/show_clock.py`, or another MoonLight board. The status shows the offset and the round trip once it answers.
///
/// ## Being a reference
///
/// With `listen` on and no reference named, the device answers others' clock requests on its own time.
///
/// @technical
///
/// Once a second it sends the reference an NTP-style request and keeps the reply with the shortest round trip of the last eight; the clock steps on the first lock and slews by at most 1 ms a second after it, as `SyncedClock.h` explains. The scene arrives as repeated state, so a lost packet or a late board catches up on the next. The messages are plain OSC ([ClockPacket.h](ClockPacket.h)), and the port is unauthenticated, so listening is off until turned on.
class ShowClockService : public MoonModule {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Answer and send clock messages, which an unauthenticated port carries.
    bool listen = false;
    /// The UDP port both ends use.
    uint16_t port = clockpkt::kDefaultPort;

    /// Declare listening, the port and the reference.
    void defineControls() override {
        controls_.addControl("listen", listen);
        controls_.addControl("port", port, 1, 65535);
        /// The reference's address; empty answers others and follows no one.
        controls_.addText("reference", reference_, sizeof(reference_));
    }

    /// Reopen on a port or listen change, and start over on a new reference.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "reference") == 0) restart();
        if (std::strcmp(name, "listen") == 0 || std::strcmp(name, "port") == 0) closeSocket();
        MoonModule::onControlChanged(name);
    }

    /// Release the socket.
    void release() override {
        closeSocket();
        MoonModule::release();
    }

    /// Drain what arrived: answer requests, keep replies, record scenes.
    void tick() MM_NONBLOCKING override {
        MoonModule::tick();
        if (!listen) { if (open_) closeSocket(); return; }
        if (!ensureSocket()) return;
        uint8_t pkt[128];
        uint8_t src[4];
        uint16_t srcPort = 0;
        int n;
        while ((n = sock_.recvFrom(pkt, sizeof(pkt), src, &srcPort)) > 0)
            handle(pkt, static_cast<size_t>(n), src, srcPort, synced::localMicros());
    }

    /// Ask the reference for the time, and say how the clock stands.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        if (!listen || !open_) return;
        if (hasReference_) {
            uint8_t pkt[32];
            const size_t len = clockpkt::encodePing(pkt, sizeof(pkt), synced::localMicros());
            if (len) transmit(referenceIp_, port, pkt, len);
        }
        report();
    }

    /// Handle one datagram that arrived at `nowUs` on this device's clock; the seam a test drives.
    void handle(const uint8_t* pkt, size_t len, const uint8_t src[4], uint16_t srcPort, int64_t nowUs) {
        clockpkt::Parsed m;
        if (!clockpkt::parse(pkt, len, m)) return;
        if (m.kind == clockpkt::Kind::Ping) return answer(m, src, srcPort, nowUs);
        if (!fromReference(src)) return;   // only the reference sets the time and the scene
        lastHeardMs_ = platform::millis();
        if (m.kind == clockpkt::Kind::Pong) return keep(m, nowUs);
        synced::setScene(m.name, synced::toSyncedMs(m.t1));
    }

    /// Point at a reference by address, as the control does.
    void setReference(const char* host) {
        std::snprintf(reference_, sizeof(reference_), "%s", host ? host : "");
        restart();
    }

    /// The offset in force, in microseconds.
    int64_t offsetUs() const { return discipline_.offsetUs(); }
    /// Whether a reply has locked the clock.
    bool locked() const { return discipline_.locked(); }

protected:
    /// Send one datagram, the seam a test captures.
    virtual void transmit(const uint8_t ip[4], uint16_t toPort, const uint8_t* pkt, size_t len) {
        if (open_) sock_.sendToAddr(ip, toPort, pkt, len);
    }

private:
    /// Reply to a request on this device's synced time, so a board that follows can also lead.
    void answer(const clockpkt::Parsed& m, const uint8_t src[4], uint16_t srcPort, int64_t nowUs) {
        const int64_t t2 = nowUs + discipline_.offsetUs();
        uint8_t pkt[64];
        const size_t len = clockpkt::encodePong(pkt, sizeof(pkt), m.t1, t2, synced::localMicros() + discipline_.offsetUs());
        if (len) transmit(src, srcPort, pkt, len);
    }

    /// Fold one reply into the filter and move the clock toward its best sample.
    void keep(const clockpkt::Parsed& m, int64_t nowUs) {
        filter_.add(synced::measure({m.t1, m.t2, m.t3, nowUs}));
        synced::Sample best;
        if (!filter_.best(best)) return;
        discipline_.update(best.offsetUs);
        roundTripUs_ = best.roundTripUs;
        synced::publish(discipline_.offsetUs());
    }

    bool fromReference(const uint8_t src[4]) const {
        return hasReference_ && std::memcmp(src, referenceIp_, 4) == 0;
    }

    /// Forget the old reference's samples, so the next reply steps the clock.
    void restart() {
        hasReference_ = parseDottedQuad(reference_, referenceIp_);
        filter_.clear();
        discipline_.reset();
        lastHeardMs_ = 0;
    }

    bool ensureSocket() {
        if (open_) return true;
        if (!platform::networkReady()) return false;
        if (!sock_.open() || !sock_.bind(port)) {
            sock_.close();
            setStatus("port busy", Severity::Error);
            return false;
        }
        open_ = true;
        if (!restarted_) { restart(); restarted_ = true; }
        return true;
    }

    void closeSocket() {
        if (open_) sock_.close();
        open_ = false;
        setStatus(listen ? "opening" : "off");
    }

    /// The offset, the round trip and the scene, or why there is none.
    void report() {
        if (!hasReference_) {
            if (reference_[0]) return setStatus("reference is not an address", Severity::Error);
            return setStatus("answering, following no one");
        }
        if (!discipline_.locked()) return setStatus("waiting for the reference", Severity::Warning);
        const bool stale = platform::millis() - lastHeardMs_ > kStaleMs;
        std::snprintf(status_, sizeof(status_), "%s, round trip %.1f ms%s%s",
                      stale ? "reference silent, holding" : "in step",
                      static_cast<double>(roundTripUs_) / 1000.0,
                      synced::scene().name[0] ? ", " : "", synced::scene().name);
        setStatus(status_, stale ? Severity::Warning : Severity::Status);
    }

    static constexpr uint32_t kStaleMs = 10000;   ///< silence past this keeps the last offset and says so

    char reference_[40] = "";
    uint8_t referenceIp_[4] = {};
    bool hasReference_ = false;
    bool restarted_ = false;
    synced::Filter filter_;
    synced::Discipline discipline_;
    int64_t roundTripUs_ = 0;
    uint32_t lastHeardMs_ = 0;
    platform::UdpSocket sock_;
    bool open_ = false;
    char status_[80] = "";
};

}  // namespace mm
