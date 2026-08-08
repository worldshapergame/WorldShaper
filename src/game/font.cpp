#include "game/font.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "world/voxel_type.hpp"

namespace ws {
namespace {

// UTF-8 in, codepoints out. Malformed input yields U+FFFD rather than stopping, because a font is
// asked to draw whatever somebody typed and "your text is invalid" is not a useful answer to
// somebody who just wants a sign.
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
            current.top = 7;
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
            i32 value = 7;
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

TextMetrics Font::measure(const std::string& utf8, i32 tracking) const {
    TextMetrics out;
    i32 pen = 0;
    for (u32 code : decode_utf8(utf8)) {
        const Glyph* glyph = find(code);
        if (glyph == nullptr) {
            // A character with no drawing still takes room, because a missing glyph that took no
            // space would silently close a gap and change the spacing of everything after it.
            pen += 5 + tracking;
            continue;
        }
        pen += glyph->advance + tracking;
        out.above = std::max(out.above, static_cast<i32>(glyph->top));
        out.below = std::max(out.below, static_cast<i32>(glyph->height) - glyph->top);
    }
    out.width = (pen > 0) ? pen - tracking : 0;
    return out;
}

Clip text_to_clip(const Font& font, const TextRequest& request, VoxelTypeTable& types) {
    const TextMetrics metrics = font.measure(request.utf8, request.tracking);
    Clip clip;
    if (metrics.width <= 0 || request.scale <= 0 || request.depth <= 0) return clip;

    clip.size[0] = metrics.width * request.scale;
    clip.size[1] = (metrics.above + metrics.below) * request.scale;
    clip.size[2] = request.depth;
    if (clip.size[0] <= 0 || clip.size[1] <= 0) return clip;
    clip.voxels.assign(static_cast<usize>(clip.cell_count()), kAir);
    clip.inside.assign(static_cast<usize>(clip.cell_count()), 1);

    // A colour a glyph asked for, interned once and reused. A word in one emoji costs one record
    // per colour in it rather than one per pixel.
    std::unordered_map<u32, VoxelTypeId> inked;
    const auto material_for = [&](u32 rgb) {
        const auto found = inked.find(rgb);
        if (found != inked.end()) return found->second;
        VisualRecord visual{};
        visual.red = static_cast<u8>((rgb >> 16) & 0xFFu);
        visual.green = static_cast<u8>((rgb >> 8) & 0xFFu);
        visual.blue = static_cast<u8>(rgb & 0xFFu);
        visual.opacity = 255;
        visual.roughness = 200;
        BehaviourRecord behaviour{};
        const VoxelTypeId id = types.intern(visual, behaviour);
        inked.emplace(rgb, id);
        return id;
    };

    i32 pen = 0;
    for (u32 code : decode_utf8(request.utf8)) {
        const Glyph* glyph = font.find(code);
        if (glyph == nullptr) {
            pen += 5 + request.tracking;
            continue;
        }
        for (u32 gy = 0; gy < glyph->height; ++gy) {
            for (u32 gx = 0; gx < glyph->width; ++gx) {
                const u8 index = glyph->at(gx, gy);
                if (index == 0) continue;

                // The glyph's own colour when it has one; the caller's material when it does not.
                // A letter is not usually a picture and should not cost a record to say so.
                const VoxelTypeId type =
                    glyph->coloured()
                        ? material_for(glyph->palette[std::min<usize>(index,
                                                                      glyph->palette.size() - 1)])
                        : request.material;

                // Measured from the baseline, which is the whole reason `top` exists: a `p` hangs
                // below it and a `T` does not, and both have to sit on the same line.
                const i32 row_from_top = (metrics.above - glyph->top) + static_cast<i32>(gy);
                for (i32 sy = 0; sy < request.scale; ++sy) {
                    for (i32 sx = 0; sx < request.scale; ++sx) {
                        const i32 x = (pen + static_cast<i32>(gx)) * request.scale + sx;
                        const i32 y = clip.size[1] - 1 - (row_from_top * request.scale + sy);
                        if (x < 0 || y < 0 || x >= clip.size[0] || y >= clip.size[1]) continue;
                        for (i32 z = 0; z < request.depth; ++z) {
                            clip.voxels[clip.index(x, y, z)] = type;
                        }
                    }
                }
            }
        }
        pen += glyph->advance + request.tracking;
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

}  // namespace ws
