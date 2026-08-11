// Markdown, and the two things that can go wrong with it: reading it as the wrong structure, and
// setting the right structure in the wrong place.
//
// The reader is tested against what somebody would actually type, including the things that are
// NOT markup — a lone star in "2 * 3", an underscore inside a name — because a reader that turns
// half a sign italic because of an arithmetic sign is worse than one that reads nothing at all.

#include <doctest/doctest.h>

#include <string>

#include "game/markup.hpp"
#include "world/voxel_type.hpp"

using namespace ws;

namespace {

std::string font_directory() {
    return std::string(WS_ASSET_SOURCE_DIR) + "/font";
}

Font shipped() {
    Font font;
    font.read_directory(font_directory());
    return font;
}

// The whole text of a line, whatever it is styled as.
std::string words(const MarkupLine& line) {
    std::string out;
    for (const Run& run : line.runs) out += run.text;
    return out;
}

// Whether some run of a line carries exactly this text in exactly this style.
bool has_run(const MarkupLine& line, const std::string& text, u8 style) {
    for (const Run& run : line.runs) {
        if (run.text == text && run.style == style) return true;
    }
    return false;
}

bool row_has_ink(const InkPage& page, i32 row) {
    for (i32 x = 0; x < page.width; ++x) {
        if (page.at(x, row) != 0) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("the reader tells structure from text") {
    const Markup document = parse_markup(
        "# Title\n"
        "\n"
        "A paragraph.\n"
        "- one\n"
        "  - nested\n"
        "- [ ] to do\n"
        "- [x] done\n"
        "3. third\n"
        "> quoted\n"
        "```\n"
        "code()\n"
        "```\n"
        "---\n");

    REQUIRE(document.lines.size() == 11);

    CHECK(document.lines[0].kind == MarkupBlock::heading);
    CHECK(document.lines[0].level == 1);
    CHECK(words(document.lines[0]) == "Title");

    CHECK(document.lines[1].kind == MarkupBlock::blank);
    CHECK(document.lines[2].kind == MarkupBlock::paragraph);

    CHECK(document.lines[3].kind == MarkupBlock::bullet);
    CHECK(document.lines[3].indent == 0);
    CHECK(words(document.lines[3]) == "one");

    // Nesting is written as indentation and read as a depth, so a list can be typed the way a list
    // looks rather than with a marker for how deep it is.
    CHECK(document.lines[4].kind == MarkupBlock::bullet);
    CHECK(document.lines[4].indent == 1);

    CHECK(document.lines[5].box == 0);
    CHECK(words(document.lines[5]) == "to do");
    CHECK(document.lines[6].box == 1);
    CHECK(words(document.lines[6]) == "done");

    // A numbered item keeps the number it was written with; renumbering somebody's list silently
    // is a decision the reader has no business making.
    CHECK(document.lines[7].kind == MarkupBlock::ordered);
    CHECK(document.lines[7].ordinal == 3);

    // A quotation is a depth, not a kind.
    CHECK(document.lines[8].quote == 1);
    CHECK(document.lines[8].kind == MarkupBlock::paragraph);
    CHECK(words(document.lines[8]) == "quoted");

    // The fence lines are not content; what is between them is, exactly as written.
    CHECK(document.lines[9].kind == MarkupBlock::code);
    CHECK(words(document.lines[9]) == "code()");

    CHECK(document.lines[10].kind == MarkupBlock::rule);
}

TEST_CASE("the reader tells emphasis from punctuation") {
    {
        const Markup document = parse_markup("plain **heavy** and *lean* and `verbatim`");
        REQUIRE(document.lines.size() == 1);
        const MarkupLine& line = document.lines[0];
        CHECK(has_run(line, "heavy", kBold));
        CHECK(has_run(line, "lean", kItalic));
        CHECK(has_run(line, "verbatim", kMono));
        CHECK(words(line) == "plain heavy and lean and verbatim");
    }
    {
        const Markup document = parse_markup("~~gone~~ and [a label](somewhere)");
        const MarkupLine& line = document.lines[0];
        CHECK(has_run(line, "gone", kStrike));
        // A link is its words, underlined. There is nowhere to follow it to from inside a world.
        CHECK(has_run(line, "a label", kUnderline));
        CHECK(words(line) == "gone and a label");
    }
    {
        // Three delimiters is the weight and the lean, with no case of its own.
        const Markup document = parse_markup("***both***");
        CHECK(has_run(document.lines[0], "both", static_cast<u8>(kBold | kItalic)));
    }
    {
        // The things that are NOT markup, each of which was a wrong answer somewhere first.
        const Markup document = parse_markup("2 * 3 = 6, snake_case_name, 50% \\*literal\\*");
        const MarkupLine& line = document.lines[0];
        REQUIRE(line.runs.size() == 1);
        CHECK(line.runs[0].style == kPlain);
        CHECK(line.runs[0].text == "2 * 3 = 6, snake_case_name, 50% *literal*");
    }
    {
        // Nothing inside a code span is a delimiter, which is the whole reason it exists.
        const Markup document = parse_markup("`a * b` and **after**");
        const MarkupLine& line = document.lines[0];
        CHECK(has_run(line, "a * b", kMono));
        CHECK(has_run(line, "after", kBold));
    }
}

TEST_CASE("a heading is bigger, and a body line is not") {
    const Font font = shipped();

    MarkupRequest body;
    body.markdown = "Aa";
    const InkPage plain = set_markup(font, body);

    MarkupRequest heading;
    heading.markdown = "# Aa";
    const InkPage large = set_markup(font, heading);

    MarkupRequest small_heading;
    small_heading.markdown = "### Aa";
    const InkPage bold = set_markup(font, small_heading);

    // Size says it while there is size to spend...
    CHECK(large.height > plain.height);
    CHECK(large.width > plain.width);
    // ...and weight says it when there is not: a third-level heading is body-sized and bold, so it
    // is wider than the same words plain but exactly as tall.
    CHECK(bold.height == plain.height);
    CHECK(bold.width > plain.width);
}

TEST_CASE("a list is indented under its marker, and a quotation carries a bar") {
    const Font font = shipped();

    MarkupRequest request;
    request.markdown = "- one\n  - two";
    const InkPage page = set_markup(font, request);
    CHECK(page.height > kCapHeight + kDescent + 1);   // two lines

    // The nested item starts further right than the one above it. Measured as "the first column
    // with any ink in it on each line", because that is what a reader sees.
    const auto first_ink = [&](i32 from_row, i32 to_row) {
        for (i32 x = 0; x < page.width; ++x) {
            for (i32 y = from_row; y < to_row; ++y) {
                if (page.at(x, y) != 0) return x;
            }
        }
        return page.width;
    };
    const i32 rows = kCapHeight + kDescent + 2;   // a line, and the leading under it
    CHECK(first_ink(0, rows) < first_ink(rows, page.height));

    MarkupRequest quoted;
    quoted.markdown = "> said";
    const InkPage bar = set_markup(font, quoted);
    // The bar is the leftmost column and it runs the whole height of the line it belongs to.
    for (i32 y = 0; y < kCapHeight + kDescent; ++y) {
        INFO("row " << y << " of a quotation's bar");
        CHECK(bar.at(0, y) != 0);
    }
}

TEST_CASE("a wrapped line breaks between words, not inside one") {
    const Font font = shipped();

    MarkupRequest request;
    request.markdown = "aaa bbb ccc";
    request.wrap = 12;
    const InkPage page = set_markup(font, request);

    CHECK(page.width <= 12);
    // Three words that do not fit on one row are three rows, not one long one.
    CHECK(page.height >= 3 * (kCapHeight + kDescent + 1));

    // And the same text without a wrap is one row, as written.
    MarkupRequest as_written = request;
    as_written.wrap = 0;
    const InkPage single = set_markup(font, as_written);
    CHECK(single.width > 12);
    CHECK(single.height < page.height);
}

TEST_CASE("a document becomes matter like a word does") {
    const Font font = shipped();
    VoxelTypeTable types;

    MarkupRequest request;
    request.markdown = "# Sign\n- one\n- two";
    request.material = 1;
    const Clip clip = markup_to_clip(font, request, types);
    CHECK(clip.size[0] > 0);
    CHECK(clip.size[1] > 0);
    CHECK(clip.solid_count() > 0);

    MarkupRequest cut = request;
    cut.sunk = true;
    const Clip sunk = markup_to_clip(font, cut, types);
    REQUIRE(sunk.cell_count() == clip.cell_count());
    CHECK(clip.solid_count() + sunk.solid_count() == static_cast<u64>(clip.cell_count()));

    // Scale multiplies the matter and not the drawing, the same way a single word does.
    MarkupRequest bigger = request;
    bigger.scale = 2;
    const Clip large = markup_to_clip(font, bigger, types);
    CHECK(large.solid_count() == clip.solid_count() * 4);
}

TEST_CASE("an underline ends under the last letter of the link") {
    const Font font = shipped();

    MarkupRequest request;
    request.markdown = "[go](x) on";
    const InkPage page = set_markup(font, request);

    // The underline is the lowest row of the first line, and it stops where the link's words do:
    // a rule that ran to the end of the sentence would be saying that all of it is a link.
    const i32 underline = kCapHeight + 1;
    REQUIRE(page.height > underline);
    CHECK(row_has_ink(page, underline));

    i32 last = -1;
    for (i32 x = 0; x < page.width; ++x) {
        if (page.at(x, underline) != 0) last = x;
    }
    CHECK(last >= 0);
    CHECK(last < page.width - 1);
}
