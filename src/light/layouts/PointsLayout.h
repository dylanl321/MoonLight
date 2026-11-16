#pragma once

#include "light/layouts/LayoutBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

namespace points {

/// Where point files live, and the extension the picker offers.
inline constexpr const char* kDir = "/layouts";
inline constexpr const char* kExt = ".csv";

/// What a new point file starts out as: the format, explained in its own comments, and three lights.
inline constexpr const char* kTemplate =
    "# One light per line, in wiring order: x,y,z\n"
    "# Whole numbers from 0 to 32767, in whatever unit the file chooses.\n"
    "# A fourth field of 0 keeps that light dark: wired, but never lit.\n"
    "# Lines starting with # and blank lines are skipped.\n"
    "0,0,0\n"
    "1,0,0\n"
    "2,0,0\n";

/// What the `file` control tells the UI: the directory, the extension and the new-file template.
inline constexpr const char* kPick[3] = {kDir, kExt, kTemplate};

/// One parsed light: its position, and whether it is a dark slot.
struct Point {
    lengthType x, y, z;
    bool dark;
};

/// Reads up to `len` bytes into `buf`; 0 at the end, negative on error.
using ReadFn = int (*)(void* ctx, char* buf, size_t len);
/// Receives each light in file order.
using PointFn = void (*)(void* ctx, const Point& p);

/// What a parse found: lights accepted, and lines refused as malformed.
struct ParseResult {
    uint32_t lights = 0;
    uint32_t skipped = 0;
    bool readError = false;
};

/// Whether `c` separates two fields: a comma, a space, a tab or a semicolon.
inline bool isSeparator(char c) { return c == ',' || c == ' ' || c == '\t' || c == ';' || c == '\r'; }

/// What reading the next field of a line found.
enum class Field : uint8_t { Value, End, Bad };

/// Read the next whole number from `s`, past any separators, and move `s` beyond it.
inline Field nextField(const char*& s, long& value) {
    while (*s && isSeparator(*s)) s++;
    if (!*s) return Field::End;
    char* end = nullptr;
    value = std::strtol(s, &end, 10);
    if (end == s || (*end && !isSeparator(*end))) return Field::Bad;   // not a number, or a number run into letters
    s = end;
    return Field::Value;
}

/// Turn three coordinates and a lit flag into `p`; false when a coordinate is outside 0 to 32767, the range `lengthType` holds, or the flag is neither 0 nor 1.
inline bool toPoint(const long (&v)[4], Point& p) {
    const auto inRange = [](long c) { return c >= 0 && c <= 32767; };
    if (!inRange(v[0]) || !inRange(v[1]) || !inRange(v[2]) || (v[3] != 0 && v[3] != 1)) return false;
    p = {static_cast<lengthType>(v[0]), static_cast<lengthType>(v[1]), static_cast<lengthType>(v[2]), v[3] == 0};
    return true;
}

/// Parse one line, already stripped of its end of line, into `p`; false for a line that is not a light.
/// A light is three or four whole numbers separated by commas, spaces, tabs or semicolons. Coordinates run 0 to 32767, the range `lengthType` holds, and a fourth field of 0 marks the light dark.
inline bool parseLine(const char* line, Point& p) {
    long v[4] = {0, 0, 0, 1};
    long extra = 0;   // a fifth field lands here, only to be counted
    uint8_t n = 0;
    Field f;
    while ((f = nextField(line, n < 4 ? v[n] : extra)) == Field::Value) n++;
    if (f == Field::Bad || n < 3 || n > 4) return false;   // a word among them, too few, or too many
    return toPoint(v, p);
}

/// One line of a file, gathered a character at a time and judged when it ends.
struct LineReader {
    char line[80];
    size_t len = 0;
    bool overflow = false;   ///< longer than the buffer, so refused whole rather than split

    /// Add one character of the current line.
    void push(char c) {
        if (len + 1 < sizeof(line)) line[len++] = c;
        else overflow = true;
    }
    /// Whether anything is gathered, for the last line of a file with no end of line.
    bool pending() const { return len > 0 || overflow; }

    /// Judge the gathered line into `r`, handing a light to `onPoint` while under `limit`, then start the next line.
    void finish(ParseResult& r, PointFn onPoint, void* ctx, uint32_t limit) {
        line[len] = '\0';
        const char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        const bool blank = !overflow && (!*s || *s == '#' || *s == '\r');
        Point p{};
        if (!blank && !overflow && parseLine(s, p)) take(r, p, onPoint, ctx, limit);
        else if (!blank) r.skipped++;
        len = 0;
        overflow = false;
    }

private:
    static void take(ParseResult& r, const Point& p, PointFn onPoint, void* ctx, uint32_t limit) {
        if (r.lights >= limit) return;
        if (onPoint) onPoint(ctx, p);
        r.lights++;
    }
};

/// Stream a point file through `read`, calling `onPoint` per light, up to `limit` lights.
/// It holds one line at a time, so memory stays fixed whatever the file's size. A line longer than the buffer is refused as malformed rather than split.
inline ParseResult parse(ReadFn read, void* readCtx, PointFn onPoint, void* pointCtx,
                         uint32_t limit = 0xFFFFFFFFu) {
    ParseResult r;
    LineReader lr;
    char chunk[128];
    int n;
    while ((n = read(readCtx, chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < n; i++) {
            if (chunk[i] == '\n') lr.finish(r, onPoint, pointCtx, limit);
            else lr.push(chunk[i]);
        }
    }
    r.readError = n < 0;
    if (lr.pending()) lr.finish(r, onPoint, pointCtx, limit);
    return r;
}

}  // namespace points

/// Lights at positions read from a file: any shape, measured or exported from another tool.
///
/// The other layouts compute where the lights are from a few numbers. This one is for lights whose positions are already known: a house outline traced in a design tool, a tree measured by hand, a prop exported from a sequencer. Each line of the file is one light in wiring order, so the file is also the wiring.
///
/// @guide
///
/// ## The file
///
/// A plain text file in the `/layouts` folder, one light per line in wiring order: `x,y,z`, whole numbers from 0 to 32767. The unit is the file's choice, and it sets the grid the effects draw on, so a coarser unit means a smaller buffer.
///
/// ## Gaps and comments
///
/// A fourth field of `0` keeps that light dark, for a wired pixel that should never light. Lines starting with `#` and blank lines are skipped, and a malformed line is skipped and counted in the status. Two lights may share a position and then show the same color, which suits a bulb wired as two pixels.
///
/// ## Making a file
///
/// Any tool that can write numbers can make one. `moondeck/run/xlights_points.py` writes one per controller from an xLights layout, in the controller's own channel order.
///
/// @technical
///
/// The file is read twice per rebuild, once to count and once to place, through a fixed 208-byte buffer, so a large file costs no memory beyond its stream. The count is taken in `lightCount`, since the Layer asks for it before any child's prepare has run, and `placeLights` stops there, so the count and the lights agree even when the file changes between the two reads.
class PointsLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// The file may place lights on all three axes.
    Dim dimensions() const override { return Dim::D3; }

    /// Publish the file the lights are read from.
    void defineControls() override {
        controls_.addFilePath("file", file_, sizeof(file_), points::kPick);
    }

    /// Count the lights in the file, which the Layer sizes its buffer from, and say what the file held.
    nrOfLightsType lightCount() const override {
        bool dark = false;
        const points::ParseResult r = stream(
            [](void* ctx, const points::Point& p) { if (p.dark) *static_cast<bool*>(ctx) = true; },
            &dark, 0xFFFFFFFFu);
        count_ = r.lights;
        dark_ = dark;
        const_cast<PointsLayout*>(this)->report(r);
        return clampedCount(count_);
    }

    /// Whether the file marks any light dark, which turns the identity fast path off.
    bool hasBlackPixels() const override { return dark_; }

    /// Emit every light in file order, stopping at the count `lightCount` took.
    void placeLights(const CoordSink& sink) const override {
        Emit e{&sink, 0};
        stream(
            [](void* ctx, const points::Point& p) {
                auto* em = static_cast<Emit*>(ctx);
                if (p.dark) em->sink->blackPixel(em->idx++, p.x, p.y, p.z);
                else em->sink->pixel(em->idx++, p.x, p.y, p.z);
            },
            &e, count_);
    }

    /// Point the layout at a file; the next rebuild reads it.
    void setFile(const char* path) { std::snprintf(file_, sizeof(file_), "%s", path ? path : ""); }

private:
    struct Emit { const CoordSink* sink; nrOfLightsType idx; };

    /// Stream the file through the parser, handing each light to `onPoint`, up to `limit` lights.
    points::ParseResult stream(points::PointFn onPoint, void* ctx, uint32_t limit) const {
        points::ParseResult r;
        open_ = false;
        if (!file_[0]) return r;
        platform::FsReadStream s;
        if (!platform::fsReadStreamOpen(s, file_)) return r;
        open_ = true;
        r = points::parse(
            [](void* h, char* buf, size_t len) {
                return platform::fsReadStreamRead(*static_cast<platform::FsReadStream*>(h), buf, len);
            },
            &s, onPoint, ctx, limit);
        platform::fsReadStreamClose(s);
        return r;
    }

    /// Say what the last count found: no file, a missing one, skipped lines, or nothing to say.
    void report(const points::ParseResult& r) {
        if (!file_[0]) return setStatus("pick a file", Severity::Warning);
        if (!open_) return setStatus("file not found", Severity::Error);
        if (r.readError) return setStatus("file read failed", Severity::Error);
        if (r.lights == 0 && r.skipped == 0) return setStatus("no lights in file", Severity::Warning);
        if (r.skipped == 0) return clearStatus();
        std::snprintf(status_, sizeof(status_), "%u lights · %u line%s skipped",
                      static_cast<unsigned>(r.lights), static_cast<unsigned>(r.skipped),
                      r.skipped == 1 ? "" : "s");
        setStatus(status_, Severity::Warning);
    }

    /// The file the lights are read from.
    char file_[96] = "";
    /// The lights the last count found, which bounds the walk.
    mutable uint32_t count_ = 0;
    /// Whether the last count saw a dark light.
    mutable bool dark_ = false;
    /// Whether the last stream found the file.
    mutable bool open_ = false;
    /// Backing store for the status line, which `setStatus` borrows.
    char status_[48] = "";
};

}  // namespace mm
