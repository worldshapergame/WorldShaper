#include "ui/draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ws::ui {
namespace {

// UTF-8 in, one codepoint out, the cursor left on the next. Malformed input yields U+FFFD rather
// than stopping, for the same reason src/game/font.cpp does: the interface is asked to draw
// whatever somebody typed, and "your file name is invalid" is not a useful answer to somebody
// whose file it is.
u32 next_codepoint(std::string_view text, usize& at) {
    const u8 lead = static_cast<u8>(text[at]);
    usize extra = 0;
    u32 value = 0;
    if (lead < 0x80u) {
        ++at;
        return lead;
    } else if ((lead & 0xE0u) == 0xC0u) {
        extra = 1;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
        extra = 2;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
        extra = 3;
        value = lead & 0x07u;
    } else {
        ++at;
        return 0xFFFDu;
    }
    ++at;
    for (usize i = 0; i < extra; ++i) {
        if (at >= text.size() || (static_cast<u8>(text[at]) & 0xC0u) != 0x80u) return 0xFFFDu;
        value = (value << 6) | (static_cast<u8>(text[at]) & 0x3Fu);
        ++at;
    }
    return value;
}

}  // namespace

void DrawList::begin(u32 width, u32 height) {
    width_ = width;
    height_ = height;
    across_ = (width + kTilePixels - 1) / kTilePixels;
    down_ = (height + kTilePixels - 1) / kTilePixels;
    commands_.clear();
    characters_.clear();
    tiles_.clear();
    tile_indices_.clear();
    clips_.clear();
    clips_.push_back(Rect{0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height)});
    bounds_ = Rect{};
}

void DrawList::push_clip(const Rect& rect) { clips_.push_back(rect.clip_to(clips_.back())); }

void DrawList::pop_clip() {
    // The root clip is the screen and is not the caller's to remove. A pop without a push is a
    // programming error that would otherwise show up as marks escaping their window several
    // frames later, which is the worst possible place to notice it.
    if (clips_.size() > 1) clips_.pop_back();
}

void DrawList::add(Mark kind, const Rect& rect, u32 a, u32 b, u32 style, f32 f0, f32 f1, f32 f2,
                   f32 f3) {
    const Rect& clip = clips_.back();
    const Rect visible = rect.clip_to(clip);
    if (visible.empty()) return;   // entirely outside its window: not drawn, and not binned

    Command command;
    command.rect[0] = rect.x0;
    command.rect[1] = rect.y0;
    command.rect[2] = rect.x1;
    command.rect[3] = rect.y1;
    command.clip[0] = clip.x0;
    command.clip[1] = clip.y0;
    command.clip[2] = clip.x1;
    command.clip[3] = clip.y1;
    command.kind = static_cast<u32>(kind);
    command.a = a;
    command.b = b;
    command.style = style;
    command.f0 = f0;
    command.f1 = f1;
    command.f2 = f2;
    command.f3 = f3;
    commands_.push_back(command);

    if (commands_.size() == 1) {
        bounds_ = visible;
    } else {
        bounds_.x0 = std::min(bounds_.x0, visible.x0);
        bounds_.y0 = std::min(bounds_.y0, visible.y0);
        bounds_.x1 = std::max(bounds_.x1, visible.x1);
        bounds_.y1 = std::max(bounds_.y1, visible.y1);
    }
}

void DrawList::glass(const Rect& rect, f32 strength) {
    add(Mark::Glass, rect, 0, 0, 0, strength, 0.0f, 0.0f, 0.0f);
}

void DrawList::ink(const Rect& rect, f32 coverage) {
    add(Mark::Ink, rect, 0, 0, 0, coverage, 0.0f, 0.0f, 0.0f);
}

void DrawList::edge(const Rect& rect, f32 coverage, f32 thickness) {
    add(Mark::Edge, rect, 0, 0, 0, coverage, 0.0f, 0.0f, thickness);
}

void DrawList::hue(const Rect& rect, u32 rgb, f32 coverage) {
    add(Mark::Hue, rect, rgb, 0, 0, coverage, 0.0f, 0.0f, 0.0f);
}

void DrawList::icon(const Rect& cell, Icon which, f32 coverage, f32 phase) {
    add(Mark::Icon, cell, static_cast<u32>(which), 0, 0, coverage, phase, 0.0f, 0.0f);
}

void DrawList::logo(const Rect& cell, u32 rgb, u32 seed, u32 previous, f32 age, f32 morph,
                    const LogoFrame& now, const LogoFrame& before) {
    // The rectangle this mark is BINNED and dispatched over is `kLogoReach` times the cell, centred
    // on it, because the mark is not in a box: what an arrangement throws wide of the drawing and
    // what a dissolve scatters outward have to land somewhere, and anything outside the command's
    // rectangle is never evaluated. The drawing still occupies the cell exactly — the card recovers
    // it with the same constant, which is why that constant is checked in both places.
    const f32 grow = (kLogoReach - 1.0f) * 0.5f;
    const Rect room{cell.x0 - cell.width() * grow, cell.y0 - cell.height() * grow,
                    cell.x1 + cell.width() * grow, cell.y1 + cell.height() * grow};

    // The two frames go in the character buffer, the arriving combination first and the one it is
    // replacing straight after it, so the card finds the second at a fixed stride from the first and
    // the command carries one offset rather than two. The CLOCK is not among them: it reaches the
    // shader in the push block, because it is the same clock for every mark on the screen — which is
    // what frees `f1` to be this offset.
    const u32 base = static_cast<u32>(characters_.size());
    const auto write = [&](const LogoFrame& frame) {
        u32 bits = 0;
        std::memcpy(&bits, &frame.lane_weight, sizeof(bits));
        characters_.push_back(bits);
        std::memcpy(&bits, &frame.arrange_weight, sizeof(bits));
        characters_.push_back(bits);
        characters_.push_back(frame.along_x ? 1u : 0u);
        for (u32 at : frame.at) characters_.push_back(at);
    };
    write(now);
    write(before);

    // `b` and `style` carry the two seeds. They are the two words a Logo has nothing else to say
    // with — text's count and text's style bits — which is why this mark needs no more room in a
    // command than any other one.
    add(Mark::Logo, room, rgb, seed, previous, 1.0f, static_cast<f32>(base), age, morph);
}

f32 DrawList::measure(std::string_view utf8, f32 pixels, u32 style) {
    f32 pen = 0.0f;
    usize at = 0;
    while (at < utf8.size()) {
        const u32 code = next_codepoint(utf8, at);
        pen += glyph_step(glyph_of(code), style);
    }
    // The tracking after the last letter is not part of the word: a string that carried it would
    // sit a pixel left of centre every time it was centred.
    return std::max(pen - 1.0f, 0.0f) * pixels;
}

usize DrawList::fits(std::string_view utf8, f32 room, f32 pixels, u32 style) {
    f32 pen = 0.0f;
    usize at = 0;
    usize taken = 0;
    while (at < utf8.size()) {
        const usize was = at;
        const u32 code = next_codepoint(utf8, at);
        const f32 step = glyph_step(glyph_of(code), style) * pixels;
        // What this character costs if it turns out to be the LAST one, which is the only
        // question being asked: the tracking after the final letter is not part of the word, and
        // `measure` does not count it either. Counting it here made a string that measured exactly
        // its room lose its last letter — an off-by-one that reads as "the name is too long".
        if (pen + step - pixels > room) return was;
        pen += step;
        taken = at;
    }
    return taken;
}

f32 DrawList::text(f32 x, f32 top, std::string_view utf8, f32 pixels, u32 style, Align align,
                   f32 coverage) {
    const f32 width = measure(utf8, pixels, style);
    f32 pen = x;
    if (align == Align::Centre) pen = x - width * 0.5f;
    if (align == Align::Right) pen = x - width;

    // Snapped to whole device pixels. The face is a pixel font: a run set at a fractional offset
    // has its columns land on different sides of a boundary along the string, so one `l` is one
    // pixel wide and the next is two. Nothing else in this interface is snapped, because nothing
    // else is made of pixels that have to stay whole.
    pen = std::floor(pen + 0.5f);
    const f32 snapped_top = std::floor(top + 0.5f);

    const u32 first = static_cast<u32>(characters_.size());
    usize at = 0;
    while (at < utf8.size()) characters_.push_back(next_codepoint(utf8, at));
    const u32 count = static_cast<u32>(characters_.size()) - first;
    if (count == 0) return 0.0f;

    // The cell reaches one row below the descender, which is where an underline hangs.
    const Rect box{pen, snapped_top, pen + width + pixels,
                   snapped_top + static_cast<f32>(kGlyphRows + 1) * pixels};
    add(Mark::Text, box, first, count, style, coverage, pen, snapped_top, pixels);
    return width;
}

void DrawList::bin() {
    tiles_.assign(static_cast<usize>(across_) * down_ * 2, 0u);
    tile_indices_.clear();
    if (commands_.empty()) return;

    // Count first, then fill. Growing a vector per tile allocates once per tile per frame and
    // spends more time in the allocator than the ink spends on the card.
    std::vector<u32> counts(static_cast<usize>(across_) * down_, 0u);
    const auto span = [&](const Command& command, u32& tx0, u32& ty0, u32& tx1, u32& ty1) {
        const f32 x0 = std::max(command.rect[0], command.clip[0]);
        const f32 y0 = std::max(command.rect[1], command.clip[1]);
        const f32 x1 = std::min(command.rect[2], command.clip[2]);
        const f32 y1 = std::min(command.rect[3], command.clip[3]);
        tx0 = static_cast<u32>(std::max(0.0f, std::floor(x0 / kTilePixels)));
        ty0 = static_cast<u32>(std::max(0.0f, std::floor(y0 / kTilePixels)));
        tx1 = static_cast<u32>(std::max(0.0f, std::ceil(x1 / kTilePixels)));
        ty1 = static_cast<u32>(std::max(0.0f, std::ceil(y1 / kTilePixels)));
        tx1 = std::min(tx1, across_);
        ty1 = std::min(ty1, down_);
    };

    for (const Command& command : commands_) {
        u32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        span(command, tx0, ty0, tx1, ty1);
        for (u32 ty = ty0; ty < ty1; ++ty) {
            for (u32 tx = tx0; tx < tx1; ++tx) ++counts[static_cast<usize>(ty) * across_ + tx];
        }
    }

    u32 running = 0;
    for (usize tile = 0; tile < counts.size(); ++tile) {
        tiles_[tile * 2] = running;
        tiles_[tile * 2 + 1] = counts[tile];
        running += counts[tile];
    }
    tile_indices_.assign(running, 0u);

    // Reuse `counts` as a cursor per tile, so the second pass writes in the same order the first
    // counted — which is the order the marks were added, which is the compositing order.
    std::fill(counts.begin(), counts.end(), 0u);
    for (u32 index = 0; index < commands_.size(); ++index) {
        u32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        span(commands_[index], tx0, ty0, tx1, ty1);
        for (u32 ty = ty0; ty < ty1; ++ty) {
            for (u32 tx = tx0; tx < tx1; ++tx) {
                const usize tile = static_cast<usize>(ty) * across_ + tx;
                tile_indices_[tiles_[tile * 2] + counts[tile]] = index;
                ++counts[tile];
            }
        }
    }

    // Out to whole tiles, because that is the granularity the dispatch can actually cover.
    bounds_.x0 = std::floor(bounds_.x0 / kTilePixels) * kTilePixels;
    bounds_.y0 = std::floor(bounds_.y0 / kTilePixels) * kTilePixels;
    bounds_.x1 = std::min(std::ceil(bounds_.x1 / kTilePixels) * kTilePixels,
                          static_cast<f32>(width_));
    bounds_.y1 = std::min(std::ceil(bounds_.y1 / kTilePixels) * kTilePixels,
                          static_cast<f32>(height_));
}

}  // namespace ws::ui
