/// @module PointsLayout
/// @also LayoutBase, Layouts

/// A layout read from a file of positions, one light per line in wiring order.
///
/// The tests pin the contract a layout owes its container: the count answers before any position is asked for, the count and the positions agree, a dark light stays a gap, and a bad line or a missing file leaves a visible status instead of a crash. The parser is pinned on its own, without a filesystem, since the file format is the part other tools write against.

#include "doctest.h"
#include "light/layouts/PointsLayout.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace mm;

namespace {

/// An empty filesystem root per test, so a file one test writes never reaches another.
struct IsolatedFs {
    char root[256];
    explicit IsolatedFs(const char* tag) {
        std::snprintf(root, sizeof(root), "/tmp/mm_points_%s_%u", tag,
                      static_cast<unsigned>(platform::millis()));
        std::filesystem::remove_all(root);
        platform::fsSetRoot(root);
        REQUIRE(platform::fsMount());
        platform::fsMkdir(points::kDir);
    }
    ~IsolatedFs() {
        platform::fsSetRoot("");        // back to the real root for every other test
        std::filesystem::remove_all(root);
    }
};

/// Write `text` as /layouts/<name>, and answer the path.
std::string writePoints(const char* name, const std::string& text) {
    const std::string path = std::string(points::kDir) + "/" + name;
    REQUIRE(platform::fsWriteAtomic(path.c_str(), text.data(), text.size()));
    return path;
}

struct Placed {
    nrOfLightsType idx;
    Coord3D at;
    bool dark;
};

/// Collect what a layout emits, lit and dark apart, the way the Layer's mapping build does.
std::vector<Placed> place(const LayoutBase& layout) {
    std::vector<Placed> out;
    layout.placeLights(CoordSink{
        [](void* ctx, nrOfLightsType i, lengthType x, lengthType y, lengthType z) {
            static_cast<std::vector<Placed>*>(ctx)->push_back({i, {x, y, z}, false});
        },
        [](void* ctx, nrOfLightsType i, lengthType x, lengthType y, lengthType z) {
            static_cast<std::vector<Placed>*>(ctx)->push_back({i, {x, y, z}, true});
        },
        &out});
    return out;
}

/// A read function over an in-memory string, handing it out `step` bytes at a time.
struct StringReader {
    std::string text;
    size_t pos = 0;
    size_t step = 7;   // smaller than a line, so a line always spans two reads
    static int read(void* ctx, char* buf, size_t len) {
        auto* r = static_cast<StringReader*>(ctx);
        const size_t n = std::min({len, r->step, r->text.size() - r->pos});
        std::memcpy(buf, r->text.data() + r->pos, n);
        r->pos += n;
        return static_cast<int>(n);
    }
};

std::vector<points::Point> parseAll(const std::string& text, points::ParseResult* result = nullptr) {
    StringReader reader{text};
    std::vector<points::Point> out;
    const points::ParseResult r = points::parse(
        &StringReader::read, &reader,
        [](void* ctx, const points::Point& p) { static_cast<std::vector<points::Point>*>(ctx)->push_back(p); },
        &out);
    if (result) *result = r;
    return out;
}

}  // namespace

TEST_CASE("A point line is three whole numbers, separated by commas, spaces, tabs or semicolons") {
    points::Point p{};
    REQUIRE(points::parseLine("1,2,3", p));
    CHECK(p.x == 1);
    CHECK(p.y == 2);
    CHECK(p.z == 3);
    CHECK_FALSE(p.dark);
    REQUIRE(points::parseLine("10 20\t30", p));
    CHECK(p.z == 30);
    REQUIRE(points::parseLine("4; 5; 6\r", p));   // a Windows line end is not a field
    CHECK(p.x == 4);
}

TEST_CASE("A fourth field of 0 marks the light dark, and 1 marks it lit") {
    points::Point p{};
    REQUIRE(points::parseLine("7,8,9,0", p));
    CHECK(p.dark);
    REQUIRE(points::parseLine("7,8,9,1", p));
    CHECK_FALSE(p.dark);
}

TEST_CASE("A line that is not a light is refused: too few fields, too many, words, or out of range") {
    points::Point p{};
    CHECK_FALSE(points::parseLine("1,2", p));
    CHECK_FALSE(points::parseLine("1,2,3,1,5", p));
    CHECK_FALSE(points::parseLine("x,y,z", p));
    CHECK_FALSE(points::parseLine("1.5,2,3", p));    // whole numbers only: the unit is the file's choice
    CHECK_FALSE(points::parseLine("-1,2,3", p));     // a coordinate is never negative
    CHECK_FALSE(points::parseLine("1,2,32768", p));  // past what lengthType holds
    CHECK_FALSE(points::parseLine("1,2,3,2", p));    // the fourth field is 0 or 1
}

TEST_CASE("The parser skips comments and blank lines, counts malformed ones, and reads across chunk edges") {
    points::ParseResult r;
    const auto pts = parseAll("# a header\n\n0,0,0\n  # indented comment\n1,0,0\nnot a light\n2,0,0", &r);
    REQUIRE(pts.size() == 3);
    CHECK(pts[2].x == 2);   // the last line has no newline and still counts
    CHECK(r.lights == 3);
    CHECK(r.skipped == 1);
    CHECK_FALSE(r.readError);
}

TEST_CASE("A line longer than the parser's buffer is skipped whole, not split into two lights") {
    points::ParseResult r;
    const std::string longLine = "1,2,3" + std::string(200, ' ') + "\n";
    const auto pts = parseAll(longLine + "4,5,6\n", &r);
    REQUIRE(pts.size() == 1);
    CHECK(pts[0].x == 4);
    CHECK(r.skipped == 1);
}

TEST_CASE("The parser stops handing out lights at the limit, so a count bounds the walk") {
    StringReader reader{"0,0,0\n1,0,0\n2,0,0\n"};
    int seen = 0;
    const points::ParseResult r = points::parse(
        &StringReader::read, &reader, [](void* ctx, const points::Point&) { (*static_cast<int*>(ctx))++; },
        &seen, 2);
    CHECK(seen == 2);
    CHECK(r.lights == 2);
}

TEST_CASE("PointsLayout places the file's lights in file order, with contiguous indices") {
    IsolatedFs fs("order");
    PointsLayout layout;
    layout.defineControls();
    layout.setFile(writePoints("arch.csv", "0,0,0\n5,3,0\n10,6,2\n").c_str());

    CHECK(layout.lightCount() == 3);
    const auto placed = place(layout);
    REQUIRE(placed.size() == 3);
    for (nrOfLightsType i = 0; i < 3; i++) CHECK(placed[i].idx == i);
    CHECK(placed[1].at.x == 5);
    CHECK(placed[1].at.y == 3);
    CHECK(placed[2].at.z == 2);
    CHECK(layout.status() == nullptr);
}

TEST_CASE("A dark light in the file is placed as a gap, and the layout reports gaps") {
    IsolatedFs fs("dark");
    PointsLayout layout;
    layout.setFile(writePoints("gap.csv", "0,0,0\n1,0,0,0\n2,0,0\n").c_str());

    CHECK(layout.lightCount() == 3);
    CHECK(layout.hasBlackPixels());
    const auto placed = place(layout);
    REQUIRE(placed.size() == 3);
    CHECK_FALSE(placed[0].dark);
    CHECK(placed[1].dark);
    CHECK(placed[1].idx == 1);   // the gap keeps its wiring slot
}

TEST_CASE("Two lights may share a position, as a bulb wired as two pixels does") {
    IsolatedFs fs("shared");
    PointsLayout layout;
    layout.setFile(writePoints("c9.csv", "4,4,0\n4,4,0\n8,4,0\n8,4,0\n").c_str());

    CHECK(layout.lightCount() == 4);
    const auto placed = place(layout);
    REQUIRE(placed.size() == 4);
    CHECK(placed[0].at.x == placed[1].at.x);
    CHECK(placed[1].idx == 1);
}

TEST_CASE("A file describing a row places the same lights as the equivalent grid") {
    IsolatedFs fs("grid");
    std::string text;
    for (int x = 0; x < 8; x++) text += std::to_string(x) + ",0,0\n";
    PointsLayout layout;
    layout.setFile(writePoints("row.csv", text).c_str());

    GridLayout grid;
    grid.width = 8;
    grid.height = 1;
    grid.depth = 1;

    REQUIRE(layout.lightCount() == grid.lightCount());
    const auto a = place(layout);
    const auto b = place(grid);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); i++) {
        CHECK(a[i].at.x == b[i].at.x);
        CHECK(a[i].at.y == b[i].at.y);
        CHECK(a[i].at.z == b[i].at.z);
    }
}

TEST_CASE("The walk stops at the count, so lights added to the file after counting are not placed") {
    IsolatedFs fs("grow");
    PointsLayout layout;
    const std::string path = writePoints("grow.csv", "0,0,0\n1,0,0\n");
    layout.setFile(path.c_str());
    REQUIRE(layout.lightCount() == 2);

    writePoints("grow.csv", "0,0,0\n1,0,0\n2,0,0\n");   // the file grows between count and walk
    CHECK(place(layout).size() == 2);
}

TEST_CASE("No file, a missing file, an empty file and skipped lines each leave a status, and no lights where none parse") {
    IsolatedFs fs("status");
    PointsLayout layout;

    CHECK(layout.lightCount() == 0);
    CHECK(layout.severity() == MoonModule::Severity::Warning);   // no file picked yet

    layout.setFile("/layouts/missing.csv");
    CHECK(layout.lightCount() == 0);
    CHECK(layout.severity() == MoonModule::Severity::Error);
    CHECK(place(layout).empty());

    layout.setFile(writePoints("empty.csv", "# nothing yet\n").c_str());
    CHECK(layout.lightCount() == 0);
    CHECK(layout.severity() == MoonModule::Severity::Warning);

    layout.setFile(writePoints("messy.csv", "0,0,0\nbad\n1,0,0\n").c_str());
    CHECK(layout.lightCount() == 2);
    REQUIRE(layout.status() != nullptr);
    CHECK(std::strstr(layout.status(), "1 line skipped") != nullptr);
}
