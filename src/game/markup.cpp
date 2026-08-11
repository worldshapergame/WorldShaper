#include "game/markup.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "world/voxel_type.hpp"

namespace ws {
namespace {

// --- the reader -------------------------------------------------------------------------------

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string& text) {
    const usize first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const usize last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

bool is_rule(const std::string& trimmed) {
    if (trimmed.size() < 3) return false;
    const char mark = trimmed[0];
    if (mark != '-' && mark != '*' && mark != '_') return false;
    usize count = 0;
    for (char c : trimmed) {
        if (c == mark) ++count;
        else if (c != ' ') return false;
    }
    return count >= 3;
}

void emit(std::vector<Run>& out, std::string& buffer, u8 style) {
    if (buffer.empty()) return;
    // Runs that agree about their style are one run. Otherwise every `*` in a sentence would
    // leave a seam in the layout, and a seam is a place a decoration can restart.
    if (!out.empty() && out.back().style == style) out.back().text += buffer;
    else out.push_back(Run{buffer, style});
    buffer.clear();
}

// Whether the delimiter that would open an emphasis has a partner later on the line. Without this
// a lone `*` in "2 * 3" would turn the rest of the sentence italic, which is the failure everybody
// who has ever typed a star into a chat box has seen.
bool closer_ahead(const std::string& text, usize from, char mark, usize length, bool word_rule) {
    for (usize i = from; i + length <= text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
            continue;
        }
        bool run = true;
        for (usize k = 0; k < length; ++k) {
            if (text[i + k] != mark) run = false;
        }
        if (!run) continue;
        if (word_rule && i + length < text.size() && word_char(text[i + length])) continue;
        return true;
    }
    return false;
}

void read_inline(const std::string& text, u8 base, std::vector<Run>& out) {
    std::string buffer;
    u8 style = base;
    usize i = 0;

    while (i < text.size()) {
        const char c = text[i];

        // A backslash makes the next character text, whatever it is. This is the escape hatch that
        // lets somebody write about markdown in markdown, and it costs two lines.
        if (c == '\\' && i + 1 < text.size()) {
            buffer.push_back(text[i + 1]);
            i += 2;
            continue;
        }

        // Code is verbatim: nothing inside a pair of backticks is a delimiter, which is the whole
        // reason a code span exists.
        if (c == '`') {
            const usize close = text.find('`', i + 1);
            if (close != std::string::npos) {
                emit(out, buffer, style);
                const std::string code = text.substr(i + 1, close - i - 1);
                if (!code.empty()) {
                    // A monospaced run cannot also lean: the lean is what breaks the grid.
                    const u8 mono = static_cast<u8>((style & ~static_cast<u8>(kItalic)) | kMono);
                    out.push_back(Run{code, mono});
                }
                i = close + 1;
                continue;
            }
        }

        // `[label](target)`, and `![alt](src)`, which is the same thing: there is nowhere to fetch
        // a picture from inside a world, so an image is its words.
        if (c == '[' || (c == '!' && i + 1 < text.size() && text[i + 1] == '[')) {
            const usize open = (c == '!') ? i + 1 : i;
            const usize close = text.find(']', open + 1);
            if (close != std::string::npos && close + 1 < text.size() && text[close + 1] == '(') {
                const usize end = text.find(')', close + 2);
                if (end != std::string::npos) {
                    emit(out, buffer, style);
                    const u8 linked = static_cast<u8>(style | kUnderline);
                    read_inline(text.substr(open + 1, close - open - 1), linked, out);
                    i = end + 1;
                    continue;
                }
            }
        }

        if (c == '~' && i + 1 < text.size() && text[i + 1] == '~') {
            if ((style & kStrike) != 0) {
                emit(out, buffer, style);
                style = static_cast<u8>(style & ~static_cast<u8>(kStrike));
                i += 2;
                continue;
            }
            if (closer_ahead(text, i + 2, '~', 2, false)) {
                emit(out, buffer, style);
                style = static_cast<u8>(style | kStrike);
                i += 2;
                continue;
            }
        }

        if (c == '*' || c == '_') {
            usize run = 0;
            while (i + run < text.size() && text[i + run] == c) ++run;
            // Two or more is weight, one is a lean. `***both***` therefore opens the weight, then
            // the lean, with no case of its own.
            const usize length = (run >= 2) ? 2 : 1;
            const u8 bit = (length == 2) ? static_cast<u8>(kBold) : static_cast<u8>(kItalic);

            // `_` inside a word is not emphasis, so `snake_case_names` survive being written down.
            const bool word_rule = (c == '_');
            const bool can_open = !word_rule || (i == 0 || !word_char(text[i - 1]));
            const bool can_close =
                !word_rule || (i + length >= text.size() || !word_char(text[i + length]));

            if ((style & bit) != 0 && can_close) {
                emit(out, buffer, style);
                style = static_cast<u8>(style & ~bit);
                i += length;
                continue;
            }
            if ((style & bit) == 0 && can_open &&
                closer_ahead(text, i + length, c, length, word_rule)) {
                emit(out, buffer, style);
                style = static_cast<u8>(style | bit);
                i += length;
                continue;
            }
        }

        buffer.push_back(c);
        ++i;
    }
    emit(out, buffer, style);
}

// --- the layout -------------------------------------------------------------------------------

constexpr i32 kIndentStep = 4;    // pixels one level of list nesting indents by
constexpr i32 kQuoteStep = 3;     // the bar down the side of a quotation, and the gap after it
constexpr i32 kMarkerGap = 2;     // between a bullet or a number and the words it labels
constexpr i32 kCodeIndent = 2;    // how far a fenced block stands in from the margin
constexpr i32 kRuleRows = 3;      // what a `---` occupies, line included

constexpr u32 kBulletMark = 0x2022;
constexpr u32 kBoxMark = 0x2610;
constexpr u32 kTickMark = 0x2611;

struct GlyphPlace {
    const Glyph* glyph;
    i32 x;
    i32 base;
    u8 style;
    i32 scale;
};

struct RectPlace {
    i32 x;
    i32 y;
    i32 width;
    i32 height;
    bool to_the_edge;   // a horizontal rule spans whatever the page turns out to be
};

// How tall a heading is drawn. A heading that is already twice the size does not also need to be
// heavier, and one that is not bigger has nothing else left to say it with — so the weight below
// is added exactly when the size runs out.
i32 heading_scale(u8 level) {
    return std::max(1, 4 - static_cast<i32>(level));
}

}  // namespace

Markup parse_markup(const std::string& markdown) {
    Markup out;
    if (markdown.empty()) return out;

    // The newline that ends the last line ends it; it does not start another. Otherwise every
    // document written the ordinary way would carry a blank line nobody typed, and a page would be
    // a line taller than what is on it.
    std::string text = markdown;
    if (text.back() == '\n') text.pop_back();
    if (!text.empty() && text.back() == '\r') text.pop_back();

    bool fenced = false;
    usize start = 0;
    while (start <= text.size()) {
        const usize newline = text.find('\n', start);
        const usize stop = (newline == std::string::npos) ? text.size() : newline;
        std::string raw = text.substr(start, stop - start);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        start = (newline == std::string::npos) ? text.size() + 1 : newline + 1;

        const std::string trimmed = trim(raw);

        // A fence opens and closes; its own line is never content.
        if (trimmed.rfind("```", 0) == 0 || trimmed.rfind("~~~", 0) == 0) {
            fenced = !fenced;
            continue;
        }
        if (fenced) {
            MarkupLine line;
            line.kind = MarkupBlock::code;
            if (!raw.empty()) line.runs.push_back(Run{raw, kMono});
            out.lines.push_back(line);
            continue;
        }

        MarkupLine line;

        // Quotations are a depth, not a kind: `> - a bullet` is a bullet that happens to be quoted.
        // The indentation is counted twice on purpose — before the markers and after them — so
        // that a nested item inside a quotation is nested by the spaces after its `>` and not by
        // the ones the quotation itself needed.
        usize at = 0;
        usize spaces = 0;
        while (at < raw.size() && (raw[at] == ' ' || raw[at] == '\t')) {
            spaces += (raw[at] == '\t') ? 4u : 1u;
            ++at;
        }
        while (at < raw.size() && raw[at] == '>' && line.quote < 6) {
            ++line.quote;
            ++at;
            if (at < raw.size() && raw[at] == ' ') ++at;
        }
        if (line.quote > 0) {
            spaces = 0;
            while (at < raw.size() && (raw[at] == ' ' || raw[at] == '\t')) {
                spaces += (raw[at] == '\t') ? 4u : 1u;
                ++at;
            }
        }
        const usize nesting = spaces;
        std::string content = trim(raw.substr(at));

        if (content.empty()) {
            line.kind = MarkupBlock::blank;
            out.lines.push_back(line);
            continue;
        }
        if (is_rule(content)) {
            line.kind = MarkupBlock::rule;
            out.lines.push_back(line);
            continue;
        }

        line.indent = static_cast<u8>(std::min<usize>(nesting / 2, 6));

        if (content[0] == '#') {
            usize hashes = 0;
            while (hashes < content.size() && content[hashes] == '#') ++hashes;
            if (hashes <= 6 && (hashes == content.size() || content[hashes] == ' ')) {
                line.kind = MarkupBlock::heading;
                line.level = static_cast<u8>(hashes);
                line.indent = 0;
                read_inline(trim(content.substr(hashes)), kPlain, line.runs);
                out.lines.push_back(line);
                continue;
            }
        }

        const bool bulleted = (content[0] == '-' || content[0] == '*' || content[0] == '+') &&
                              (content.size() == 1 || content[1] == ' ');
        if (bulleted) {
            line.kind = MarkupBlock::bullet;
            content = trim(content.substr(1));
            // `- [ ]` and `- [x]`: a task list, which is the one list whose marker carries state.
            if (content.size() >= 3 && content[0] == '[' && content[2] == ']' &&
                (content.size() == 3 || content[3] == ' ')) {
                const char inside = content[1];
                if (inside == ' ') line.box = 0;
                if (inside == 'x' || inside == 'X') line.box = 1;
                if (line.box >= 0) content = trim(content.substr(3));
            }
            read_inline(content, kPlain, line.runs);
            out.lines.push_back(line);
            continue;
        }

        usize digits = 0;
        while (digits < content.size() && std::isdigit(static_cast<unsigned char>(content[digits]))) {
            ++digits;
        }
        if (digits > 0 && digits + 1 <= content.size() &&
            (content[digits] == '.' || content[digits] == ')') &&
            (digits + 1 == content.size() || content[digits + 1] == ' ')) {
            line.kind = MarkupBlock::ordered;
            line.ordinal = static_cast<u32>(std::strtoul(content.substr(0, digits).c_str(), nullptr, 10));
            read_inline(trim(content.substr(digits + 1)), kPlain, line.runs);
            out.lines.push_back(line);
            continue;
        }

        read_inline(content, kPlain, line.runs);
        out.lines.push_back(line);
    }
    return out;
}

InkPage set_markup(const Font& font, const MarkupRequest& request) {
    const Markup document = parse_markup(request.markdown);
    const i32 tracking = std::max(0, request.tracking);
    const i32 leading = std::max(0, request.leading);

    std::vector<GlyphPlace> glyphs;
    std::vector<RectPlace> rects;
    i32 extent = 0;          // the widest ink any row reached
    i32 bottom = 0;          // and the lowest row any of it touched
    i32 cursor = 0;          // where the next line's capitals start
    i32 first_base = -1;     // the first baseline, which is what the page reports as its own

    for (const MarkupLine& line : document.lines) {
        if (line.kind == MarkupBlock::blank) {
            cursor += kXHeight + leading;
            continue;
        }
        if (line.kind == MarkupBlock::rule) {
            rects.push_back(RectPlace{0, cursor + 1, 0, 1, true});
            // A rule is as wide as the page, and a page with nothing else on it still has to be
            // something: twelve pixels is three letters, which is the shortest line worth drawing.
            extent = std::max(extent, 12);
            bottom = std::max(bottom, cursor + 2);
            cursor += kRuleRows + leading;
            continue;
        }

        const i32 scale =
            (line.kind == MarkupBlock::heading) ? heading_scale(line.level) : 1;

        // The underline's row is only reserved on a line that has one. Always reserving it costs
        // every line a blank row, and at a heading's scale that is three — a gap under a title
        // wide enough to read as a paragraph break that nobody typed.
        bool underlined = false;
        for (const Run& run : line.runs) {
            if ((run.style & kUnderline) != 0) underlined = true;
        }
        const i32 below = kDescent + (underlined ? 1 : 0);
        const i32 row_step = (kCapHeight + below) * scale + leading;

        // What every glyph on this line carries before its own run says anything.
        u8 line_style = kPlain;
        if (line.kind == MarkupBlock::heading && scale == 1) {
            line_style = static_cast<u8>(line_style | kBold);
        }
        if (line.kind == MarkupBlock::code) line_style = static_cast<u8>(line_style | kMono);

        const i32 line_top = cursor;
        i32 left = line.quote * kQuoteStep;
        left += static_cast<i32>(line.indent) * kIndentStep;
        if (line.kind == MarkupBlock::code) left += kCodeIndent;

        i32 base = cursor + kCapHeight * scale;
        i32 pen = left;
        if (first_base < 0) first_base = base;

        // The decorations that belong to a stretch of text rather than to a glyph. They are opened
        // where the style changes and closed where it changes back, so an underline ends under the
        // last letter of the link and not in the gap after it.
        u8 marks = 0;
        i32 marks_from = pen;
        i32 ink_end = pen;

        const auto close_marks = [&]() {
            if (marks != 0 && ink_end > marks_from) {
                if ((marks & kUnderline) != 0) {
                    rects.push_back(
                        RectPlace{marks_from, base + scale, ink_end - marks_from, scale, false});
                }
                if ((marks & kStrike) != 0) {
                    rects.push_back(
                        RectPlace{marks_from, base - 2 * scale, ink_end - marks_from, scale, false});
                }
            }
            marks = 0;
        };

        const auto place = [&](const Glyph* glyph, i32 x, u8 style) {
            const u8 wanted = static_cast<u8>(style & (kUnderline | kStrike));
            if (wanted != marks) {
                close_marks();
                marks = wanted;
                marks_from = x;
            }
            if (glyph != nullptr) {
                glyphs.push_back(GlyphPlace{glyph, x, base, style, scale});
            }
            ink_end = std::max(ink_end, x + ink_width(glyph, style) * scale);
            extent = std::max(extent, ink_end);
        };

        // The marker, which is drawn in the same typeface as everything else because it is a glyph
        // and not a decoration: a bullet has a width, sits on the baseline, and can be typed.
        if (line.kind == MarkupBlock::bullet) {
            const u32 mark = (line.box < 0) ? kBulletMark : (line.box == 1 ? kTickMark : kBoxMark);
            const Glyph* glyph = font.find(mark);
            place(glyph, pen, kPlain);
            pen += font.step(glyph, kPlain, tracking) + kMarkerGap;
        } else if (line.kind == MarkupBlock::ordered) {
            const std::string number = std::to_string(line.ordinal) + ".";
            for (u32 code : decode_utf8(number)) {
                const Glyph* glyph = font.find(code);
                place(glyph, pen, kPlain);
                pen += font.step(glyph, kPlain, tracking);
            }
            pen += kMarkerGap;
        }

        // Where a wrapped continuation starts: under the words, not under the bullet.
        const i32 text_left = pen;
        bottom = std::max(bottom, base + below * scale);

        const auto break_row = [&]() {
            close_marks();
            cursor += row_step;
            base = cursor + kCapHeight * scale;
            pen = text_left;
            marks_from = pen;
            ink_end = pen;
            bottom = std::max(bottom, base + below * scale);
        };

        // A word at a time, because a line that breaks mid-word is harder to read than a short one.
        struct Pending {
            const Glyph* glyph;
            i32 dx;
            u8 style;
        };
        std::vector<Pending> word;
        i32 word_width = 0;

        const auto flush_word = [&]() {
            if (word.empty()) return;
            if (request.wrap > 0 && pen + word_width > request.wrap && pen > text_left) {
                break_row();
            }
            for (const Pending& pending : word) {
                place(pending.glyph, pen + pending.dx, pending.style);
            }
            pen += word_width;
            word.clear();
            word_width = 0;
        };

        for (const Run& run : line.runs) {
            const u8 style = static_cast<u8>(run.style | line_style);
            for (u32 code : decode_utf8(run.text)) {
                const Glyph* glyph = font.find(code);
                const i32 advance = font.step(glyph, style, tracking) * scale;
                if (code == ' ') {
                    // The space ends the word and is set on the line it ended, which is what makes
                    // a wrapped line start with a letter rather than with a gap.
                    flush_word();
                    place(nullptr, pen, style);
                    pen += advance;
                    continue;
                }
                word.push_back(Pending{glyph, word_width, style});
                word_width += advance;
            }
        }
        flush_word();
        close_marks();

        // `break_row` already moved the cursor for every row this line wrapped onto; this is the
        // step off its last one.
        cursor += row_step;

        // The bar down the side of a quotation, drawn last because it has to reach every row the
        // line ended up taking, wrapping included.
        for (i32 depth = 0; depth < static_cast<i32>(line.quote); ++depth) {
            rects.push_back(RectPlace{depth * kQuoteStep, line_top, 1, cursor - line_top, false});
            extent = std::max(extent, depth * kQuoteStep + 1);
        }
    }

    InkPage page;
    page.width = extent;
    page.height = bottom;
    page.baseline = (first_base < 0) ? 0 : first_base;
    if (page.width <= 0 || page.height <= 0) {
        page.width = 0;
        page.height = 0;
        return page;
    }
    page.pixels.assign(static_cast<usize>(page.width) * page.height, 0);
    page.colours.assign(2, 0u);

    for (const RectPlace& rect : rects) {
        const i32 width = rect.to_the_edge ? page.width : rect.width;
        paint_rule(page, rect.x, width, rect.y, rect.height);
    }
    for (const GlyphPlace& placed : glyphs) {
        paint_glyph(page, *placed.glyph, placed.x, placed.base, placed.style, placed.scale);
    }
    return page;
}

Clip markup_to_clip(const Font& font, const MarkupRequest& request, VoxelTypeTable& types) {
    return page_to_clip(set_markup(font, request), request, types);
}

}  // namespace ws
