#include "game/font.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "world/voxel_type.hpp"

namespace ws {
namespace {

u32 parse_codepoint(const std::string& token) {
    // `U+0041`, or a bare hex number, or a single character in quotes.
    if (token.size() > 2 && (token[0] == 'U' || token[0] == 'u') && token[1] == '+') {
        return static_cast<u32>(std::strtoul(token.c_str() + 2, nullptr, 16));
    }
    if (token.size() >= 3 && token.front() == '\'' && token.back() == '\'') {
        const std::vector<u32> inner = decode_utf8(token.substr(1, token.size() - 2));
        return inner.empty() ? 0u : inner[0];
    }
    return static_cast<u32>(std::strtoul(token.c_str(), nullptr, 16));
}

// One pixel of a drawn row. A dot or a space is empty; anything else is a palette index, written
// as a hex digit so a glyph with fifteen colours still fits one character to a pixel and stays
// something a person can read as a picture.
u8 pixel_of(char c) {
    if (c == '.' || c == ' ' || c == '_') return 0;
    if (c >= '1' && c <= '9') return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
    return 1;   // `#` and anything else is the first colour, which is what a plain letter uses
}

}  // namespace

i32 ink_width(const Glyph* glyph, u8 style) {
    if (glyph == nullptr || glyph->width == 0) return 0;
    i32 width = glyph->width;
    if ((style & kBold) != 0) ++width;
    if ((style & kItalic) != 0) ++width;
    return width;
}

std::vector<u32> decode_utf8(const std::string& text) {
    std::vector<u32> out;
    usize i = 0;
    while (i < text.size()) {
        const u8 c = static_cast<u8>(text[i]);
        u32 value = 0xFFFD;
        u32 extra = 0;
        if (c < 0x80) {
            value = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            value = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            value = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            value = c & 0x07u;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= text.size()) {
            out.push_back(0xFFFD);
            break;
        }
        for (u32 k = 1; k <= extra; ++k) {
            const u8 n = static_cast<u8>(text[i + k]);
            if ((n & 0xC0) != 0x80) {
                value = 0xFFFD;
                break;
            }
            value = (value << 6) | (n & 0x3Fu);
        }
        out.push_back(value);
        i += extra + 1;
    }
    return out;
}

bool Font::read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        problems_.push_back("could not open '" + path + "'");
        return false;
    }

    Glyph current;
    bool building = false;
    std::vector<std::string> rows;
    u32 line_number = 0;

    const auto commit = [&]() {
        if (!building) return;
        current.height = static_cast<u8>(rows.size());
        current.width = 0;
        for (const std::string& row : rows) {
            current.width = std::max(current.width, static_cast<u8>(row.size()));
        }
        current.pixels.assign(static_cast<usize>(current.width) * current.height, 0);
        for (usize y = 0; y < rows.size(); ++y) {
            for (usize x = 0; x < rows[y].size(); ++x) {
                current.pixels[y * current.width + x] = pixel_of(rows[y][x]);
            }
        }
        if (current.palette.size() < 2) current.palette = {0u, 0xFFFFFFu};
        if (current.advance == 0) current.advance = static_cast<u8>(current.width);
        glyphs_[current.codepoint] = current;
        building = false;
        rows.clear();
    };

    std::string line;
    while (std::getline(file, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // A row of a glyph is anything inside a drawing; everything else is a directive. The two
        // are told apart by whether a glyph is open, not by looking at the characters, so a row
        // of dots is a row of dots and never mistaken for a comment.
        const usize first = line.find_first_not_of(" \t");
        const bool blank = (first == std::string::npos);
        const bool comment = !blank && line[first] == '#' && !building;

        if (comment) continue;
        if (blank) {
            commit();
            continue;
        }

        std::istringstream tokens(line);
        std::string head;
        tokens >> head;

        if (head == "glyph") {
            commit();
            std::string code;
            tokens >> code;
            current = Glyph{};
            current.codepoint = parse_codepoint(code);
            current.top = static_cast<i8>(kCapHeight);
            building = true;
            continue;
        }
        if (head == "palette" && building) {
            // `palette 1 F2C94C 2 6E4A1F` — index then colour, as many as fit on the line.
            std::string index_text;
            std::string colour_text;
            while (tokens >> index_text >> colour_text) {
                const usize index = std::strtoul(index_text.c_str(), nullptr, 10);
                const u32 colour = static_cast<u32>(std::strtoul(colour_text.c_str(), nullptr, 16));
                if (index == 0 || index > 15) continue;
                if (current.palette.size() <= index) current.palette.resize(index + 1, 0xFFFFFFu);
                current.palette[index] = colour;
                if (current.palette.empty()) current.palette.resize(1, 0u);
            }
            continue;
        }
        if (head == "top" && building) {
            i32 value = kCapHeight;
            tokens >> value;
            current.top = static_cast<i8>(value);
            continue;
        }
        if (head == "advance" && building) {
            i32 value = 0;
            tokens >> value;
            current.advance = static_cast<u8>(std::max(0, value));
            continue;
        }
        if (building) {
            rows.push_back(line.substr(first));
            continue;
        }
        problems_.push_back(path + ":" + std::to_string(line_number) + ": expected `glyph`, got '" +
                            head + "'");
    }
    commit();
    return true;
}

bool Font::read_directory(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) {
        problems_.push_back("'" + path + "' is not a directory of font files");
        return false;
    }
    // Sorted, so a font reads the same on every machine. Two files defining the same character is
    // a mistake somebody should be told about rather than a race.
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    bool ok = true;
    for (const std::string& file : files) ok = read(file) && ok;
    return ok;
}

const Glyph* Font::find(u32 codepoint) const {
    const auto found = glyphs_.find(codepoint);
    return (found != glyphs_.end()) ? &found->second : nullptr;
}

std::vector<u32> Font::missing(const std::string& utf8) const {
    std::vector<u32> out;
    for (u32 code : decode_utf8(utf8)) {
        if (code == ' ' || code == '\n') continue;
        if (find(code) == nullptr &&
            std::find(out.begin(), out.end(), code) == out.end()) {
            out.push_back(code);
        }
    }
    return out;
}

i32 Font::step(const Glyph* glyph, u8 style, i32 tracking) const {
    // Monospaced, for a code span: every glyph steps the same, which is the entire property that
    // makes a column of code line up. Six, because `m` is five wide and a fixed step that hides
    // half an `m` under the next letter is worse than one that leaves a gap after an `i`.
    if ((style & kMono) != 0) return kMonoAdvance;

    // A character with no drawing still takes room, because a missing glyph that took no space
    // would silently close a gap and change the spacing of everything after it.
    i32 advance = (glyph != nullptr) ? static_cast<i32>(glyph->advance) : kNominalWidth;
    if ((style & kBold) != 0) ++advance;
    if ((style & kItalic) != 0) ++advance;
    return advance + tracking;
}

TextMetrics Font::measure(const std::string& utf8, i32 tracking) const {
    return measure_styled(utf8, kPlain, tracking);
}

TextMetrics Font::measure_styled(const std::string& utf8, u8 style, i32 tracking) const {
    TextMetrics out;
    i32 pen = 0;
    for (u32 code : decode_utf8(utf8)) {
        const Glyph* glyph = find(code);
        // The width is where the INK ends, not where the pen does: a trailing space, and the gap
        // after the last letter, are not part of the drawing. Taking the maximum rather than the
        // final pen also makes an underline end under the last letter rather than past it.
        out.width = std::max(out.width, pen + ink_width(glyph, style));
        if (glyph != nullptr) {
            out.above = std::max(out.above, static_cast<i32>(glyph->top));
            out.below = std::max(out.below, static_cast<i32>(glyph->height) - glyph->top);
        }
        pen += step(glyph, style, tracking);
    }
    // The underline hangs one row below the descender, so it never touches a `p`. It is part of
    // the drawing's height whether or not the string has a descender in it, because a run that
    // changed height depending on its letters would not sit on a line with its neighbours.
    if ((style & kUnderline) != 0 && !utf8.empty()) {
        out.below = std::max(out.below, kDescent + 1);
    }
    return out;
}

u8 InkPage::intern(u32 rgb) {
    for (usize i = 2; i < colours.size(); ++i) {
        if (colours[i] == rgb) return static_cast<u8>(i);
    }
    if (colours.size() < 2) colours.assign(2, 0u);
    if (colours.size() >= 255) return 1;   // out of indices: fall back to the caller's own ink
    colours.push_back(rgb);
    return static_cast<u8>(colours.size() - 1);
}

void paint_glyph(InkPage& page, const Glyph& glyph, i32 pen, i32 base, u8 style, i32 scale) {
    const bool bold = (style & kBold) != 0;
    const bool italic = (style & kItalic) != 0;
    for (u32 gy = 0; gy < glyph.height; ++gy) {
        const i32 height = glyph.top - static_cast<i32>(gy);
        // Only the rows above the x-height lean. One pixel is the whole lean available: two turns
        // a three-wide letter into a diagonal.
        const i32 lean = (italic && height >= kXHeight) ? 1 : 0;
        for (u32 gx = 0; gx < glyph.width; ++gx) {
            const u8 index = glyph.at(gx, gy);
            if (index == 0) continue;

            // The glyph's own colour when it has one; the caller's ink when it does not. A letter
            // is not usually a picture and should not cost a record to say so.
            const u8 ink = glyph.coloured()
                               ? page.intern(glyph.palette[std::min<usize>(
                                     index, glyph.palette.size() - 1)])
                               : u8{1};

            const i32 x0 = pen + (static_cast<i32>(gx) + lean) * scale;
            const i32 y0 = base - height * scale;
            const i32 smear = bold ? scale : 0;
            for (i32 sy = 0; sy < scale; ++sy) {
                for (i32 sx = 0; sx < scale; ++sx) {
                    page.set(x0 + sx, y0 + sy, ink);
                    if (bold) page.set(x0 + sx + smear, y0 + sy, ink);
                }
            }
        }
    }
}

void paint_rule(InkPage& page, i32 x, i32 width, i32 y, i32 thickness) {
    for (i32 row = 0; row < thickness; ++row) {
        for (i32 column = 0; column < width; ++column) page.set(x + column, y + row, 1);
    }
}

InkPage set_text(const Font& font, const std::string& utf8, u8 style, i32 tracking) {
    const TextMetrics metrics = font.measure_styled(utf8, style, tracking);

    InkPage page;
    page.width = metrics.width;
    page.height = metrics.above + metrics.below;
    page.baseline = metrics.above;
    if (page.width <= 0 || page.height <= 0) return page;
    page.pixels.assign(static_cast<usize>(page.width) * page.height, 0);
    page.colours.assign(2, 0u);

    i32 pen = 0;
    for (u32 code : decode_utf8(utf8)) {
        const Glyph* glyph = font.find(code);
        if (glyph != nullptr) paint_glyph(page, *glyph, pen, page.baseline, style, 1);
        pen += font.step(glyph, style, tracking);
    }

    // The decorations, which belong to the run and not to any glyph in it.
    if ((style & kStrike) != 0) {
        paint_rule(page, 0, metrics.width, page.baseline - 2, 1);
    }
    if ((style & kUnderline) != 0) {
        paint_rule(page, 0, metrics.width, page.baseline + 1, 1);
    }
    return page;
}

Clip page_to_clip(const InkPage& page, const MatterRequest& request, VoxelTypeTable& types) {
    Clip clip;
    if (page.width <= 0 || page.height <= 0 || request.scale <= 0 || request.depth <= 0) {
        return clip;
    }

    clip.size[0] = page.width * request.scale;
    clip.size[1] = page.height * request.scale;
    clip.size[2] = request.depth;
    clip.voxels.assign(static_cast<usize>(clip.cell_count()), kAir);
    clip.inside.assign(static_cast<usize>(clip.cell_count()), 1);

    // A colour the page asked for, interned once and reused.
    std::vector<VoxelTypeId> inked(page.colours.size(), 0);
    const auto material_for = [&](u8 index) {
        if (index <= 1) return request.material;
        if (inked[index] != 0) return inked[index];
        const u32 rgb = page.colours[index];
        VisualRecord visual{};
        visual.red = static_cast<u8>((rgb >> 16) & 0xFFu);
        visual.green = static_cast<u8>((rgb >> 8) & 0xFFu);
        visual.blue = static_cast<u8>(rgb & 0xFFu);
        visual.opacity = 255;
        visual.roughness = 200;
        BehaviourRecord behaviour{};
        inked[index] = types.intern(visual, behaviour);
        return inked[index];
    };

    for (i32 row = 0; row < page.height; ++row) {
        for (i32 column = 0; column < page.width; ++column) {
            const u8 index = page.at(column, row);
            if (index == 0) continue;
            const VoxelTypeId type = material_for(index);
            for (i32 sy = 0; sy < request.scale; ++sy) {
                for (i32 sx = 0; sx < request.scale; ++sx) {
                    const i32 x = column * request.scale + sx;
                    const i32 y = clip.size[1] - 1 - (row * request.scale + sy);
                    for (i32 z = 0; z < request.depth; ++z) {
                        clip.voxels[clip.index(x, y, z)] = type;
                    }
                }
            }
        }
    }

    // Cut in rather than standing proud: the same letters, as the absence of matter. A sign
    // carved into a wall and one raised off it are the same drawing and should be the same clip
    // with one flag, not two clips that have to be kept in step.
    if (request.sunk) {
        for (usize i = 0; i < clip.voxels.size(); ++i) {
            clip.voxels[i] = (clip.voxels[i] == kAir) ? request.material : kAir;
        }
    }

    clip.build_coarse();
    return clip;
}

Clip text_to_clip(const Font& font, const TextRequest& request, VoxelTypeTable& types) {
    const InkPage page = set_text(font, request.utf8, request.style, request.tracking);
    return page_to_clip(page, request, types);
}

}  // namespace ws
