// The logo, and what it does.
//
// # Why the drawing is a bitmask and not a picture
//
// `assets/logo.txt` is the source — one character per pixel, a hundred and sixty square, readable
// and diffable — and `tools/logo-table.ps1` packs it into the table below, one bit a pixel. A test
// reads the table back out of this file and checks every pixel against that source, exactly as the
// typeface is checked (`tests/test_logo.cpp`). Two copies of a drawing is one place for them to
// disagree.
//
// A texture would have needed a sampler, a staging copy and an upload to keep in step with it, for
// eight hundred words. But the real reason is what the mark DOES: it is grown from its roots,
// dissolved into voxels and reassembled, extruded into a slab and turned in space. Every one of
// those asks *is there mark at this point* and nothing else, which is a question a bitmask answers
// and an image only pretends to.
//
// # Why it is different every time
//
// It is the one surface in this interface allowed a hue of its own (documentation/14-ui-style.md,
// the five permitted colours) — so it is the one place where a palette costs nothing, and the only
// picture on the screen. A picture that is identical on every launch is furniture. So: a **seed**,
// chosen when it is pressed and again on its own when nobody has touched anything for a while, and
// everything below derives from it — which animations run, which patterns colour it, which palette
// they read from, how fast, which way, and whether it is flat or has depth.
//
// # Why nothing here is ever one choice
//
// A seed picks **two** animations and **two** patterns, not one of each, and blends each pair on
// its own slow clock. That is the difference between a mark that loops and a mark that keeps
// becoming something: the wind arrives in a mark that is breathing, one pattern washes across
// another, and neither pair ever lands on the same phase twice.
//
// # What the drawing is made OF, and where its pieces go
//
// The mark is not a picture being filtered. It is a set of bits, so it can be taken apart:
//
//   - the drawing is cut into a **grid of 144 blocks**, and a seed picks an **arrangement** those
//     blocks fly into and back out of — a ring, a spiral, a bar chart, the surface of a turning
//     cube, a sphere, a double helix, a **four-dimensional lattice** turning in two planes at once,
//     a sheet with a wave running through it, a city of towers, or a swarm on 144 small orbits;
//   - it is also cut into **sixteen slices**, and those slices are put in whatever order a
//     **sorting algorithm** has reached this second: odd-even, insertion, selection, shell, bitonic,
//     gnome, cocktail, comb, or the same networks sorting the other way, on a key the seed also
//     chooses. The sort runs in registers, the same sixteen numbers for every pixel, which is the
//     only reason a sort is drawable at all with no buffer to scatter into.
//
// **All of it at once**, and each on its own clock: the blocks can be halfway into a tesseract while
// the slices are halfway through a shell sort while the mark is leaning in the wind. Counted, the
// discrete choices come to **3,981,312,000** before a single continuous parameter — but the number
// that matters is that there is no final state: the sort's cycle number is part of its own hash, so
// every cycle shuffles differently from the last, and the clock does not stop.
//
// The point is not the number. The point is that pressing it is worth doing twice.
//
// # And the change itself is drawn
//
// A new seed does not cut. The outgoing combination dissolves into voxels as the arriving one
// gathers out of them, both live at once for `kLogoMorph` of a second, cross-faded premultiplied
// so a pixel that only one of the two has a mark at is not averaged against black. `logo_draw`
// takes both seeds and how far through that morph it is; the host decides when
// (`src/ui/logo.hpp`), because *when the picture changes* is a thing about presses and idleness
// rather than about pixels.
//
// # What this costs, and where it is bounded
//
// Everything expensive is behind a weight, and every weight is uniform over the mark except the
// pattern's — so the second animation is evaluated only while it is being blended in, the outgoing
// seed only during a morph, and the branch is coherent across the whole dispatch rather than
// splitting a warp. The mark is about a fifth of the short side of the window and nothing else in
// this shader is anywhere near it, which is what makes a per-pixel search over the drawing
// (`logo_edge`) affordable at all.
// --- the drawing ------------------------------------------------------------------------------
// --- generated: tools/logo-table.ps1 ---
const int kLogoWidth = 160;
const int kLogoHeight = 160;
const int kLogoWords = 800;
const uint kLogoBits[800] = uint[800](
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x0FFFC000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0FFFC000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x1FFFF800u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x3FFFFC00u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0xFFFFFF80u, 0x00000001u, 0x00000000u, 0x00000000u,
    0x00000000u, 0xFFFFFF80u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xFFFFFF80u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFE0u,
    0x000003FFu, 0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFE0u, 0x000003FFu,
    0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFF0u, 0x00001FFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0xFFFFFFF8u, 0x00001FFFu, 0x00000000u, 0x00000000u,
    0xFFC00000u, 0xFFFFFFFFu, 0x0001FFFFu, 0x00000000u, 0x00000000u, 0xFFC00000u,
    0xFFFFFFFFu, 0x0003FFFFu, 0x00000000u, 0x00000000u, 0xFFC00000u, 0xFFFFFFFFu,
    0x0003FFFFu, 0x00000000u, 0x00000000u, 0xFFFF0000u, 0xFFFFFFFFu, 0x003FFFFFu,
    0x00000000u, 0x00000000u, 0xFFFF0000u, 0xFFFFFFFFu, 0x003FFFFFu, 0x00000000u,
    0x00000000u, 0xFFFF8000u, 0xFFFFFFFFu, 0x00FFFFFFu, 0x00000000u, 0x00000000u,
    0xFFFFC000u, 0xFFFFFFFFu, 0x00FFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFF800u,
    0xFFFFFFFFu, 0x07FFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFF800u, 0xFFFFFFFFu,
    0x1FFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFF800u, 0xFFFFFFFFu, 0x1FFFFFFFu,
    0x00000000u, 0x00000000u, 0xFFFFF800u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x00000000u,
    0x00000000u, 0xFFFFF800u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x00000000u, 0x00000000u,
    0xFFFFFE00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFFE00u,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFFE00u, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFFE00u, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0x00000003u, 0x00000000u, 0xFFFFFE00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000003u,
    0x00000000u, 0xFFFFFE00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000003u, 0x00000000u,
    0xFFFFFE00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000003u, 0x00000000u, 0xFFFFFE00u,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000000Fu, 0x00000000u, 0xFFFFFE00u, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0x0000000Fu, 0x00000000u, 0xFFFFFF00u, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0x0000000Fu, 0x00000000u, 0xFFFFFF00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000003Fu,
    0x00000000u, 0xFFFFFF00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000003Fu, 0x00000000u,
    0xFFFFFFF0u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000003Fu, 0x00000000u, 0xFFFFFFF0u,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000003Fu, 0x00000000u, 0xFFFFFFF0u, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0x0000007Fu, 0x00000000u, 0xFFFFFFF0u, 0x00FFFFFFu, 0xFFFFFFFEu,
    0x0000007Fu, 0x00000000u, 0xFFFFFFFCu, 0x00FFFFFFu, 0xFFFFFFFCu, 0x0000007Fu,
    0x00000000u, 0xFFFFFFFCu, 0x003FFFFFu, 0xFFFFFFFEu, 0x000001FFu, 0x00000000u,
    0xFFFFFFFCu, 0x003E0FFFu, 0xFFFFFC1Eu, 0x000001FFu, 0x00000000u, 0xFFFFFFFFu,
    0x003C0FFFu, 0xFFFFFC0Eu, 0x000001FFu, 0x00000000u, 0xFFFFFFFFu, 0x003C0FFFu,
    0xFFFFFC06u, 0x000001FFu, 0x00000000u, 0xFFFFFFFEu, 0x003C0FFFu, 0xFFFFFC07u,
    0x000001FFu, 0x00000000u, 0xFFFFFFFFu, 0x0038003Fu, 0xFFFFFC07u, 0x000001FFu,
    0x80000000u, 0xFFFFFFFFu, 0xC038003Fu, 0xFFFFFC07u, 0x000007FFu, 0xE0000000u,
    0xFFFFFFFFu, 0xF0F8003Fu, 0xFFFFFC07u, 0x000007FFu, 0xE0000000u, 0xFFFFFFFFu,
    0xF0F0003Fu, 0xFFFE0C03u, 0x000007FFu, 0xF8000000u, 0xFFFFFFFFu, 0xF3E003FFu,
    0xFFFE0601u, 0x000007FFu, 0xF8000000u, 0xFFFFFFFFu, 0xF3E003E7u, 0xFFFE0601u,
    0x000007FFu, 0xF8000000u, 0xFFFFFFFFu, 0xFFE00FE7u, 0xFFFE0601u, 0x000007FFu,
    0xF8000000u, 0xFFFFFFFFu, 0x7F800F80u, 0xFFFE0600u, 0x000000FFu, 0xF8000000u,
    0xFFFFFFFFu, 0x7F800F80u, 0xFFFE1E00u, 0x0000007Fu, 0xF8000000u, 0xFFFFFFFFu,
    0x1E001F80u, 0xFFFF3F80u, 0x0000003Fu, 0xF8000000u, 0x3FFFFFFFu, 0x1E003C00u,
    0xFF07FF80u, 0x0000001Fu, 0xF8000000u, 0x3FFFFFFFu, 0x1E007800u, 0xFF03FFF8u,
    0x0000000Fu, 0xF8000000u, 0x31FFFFFFu, 0x1E007000u, 0x03061FF8u, 0x00000000u,
    0xF8000000u, 0xE1FFFFFFu, 0x1E007000u, 0x01FE1FFEu, 0x00000000u, 0xF8000000u,
    0xC07FFFFFu, 0x1E007000u, 0x000000FCu, 0x00000000u, 0xF8000000u, 0xC07FFFFFu,
    0x7E01F000u, 0x000000FEu, 0x00000000u, 0xF8000000u, 0xC07FFFFFu, 0xFE03F000u,
    0x000000FEu, 0x00000000u, 0xE0000000u, 0x003FFFFFu, 0xFE07F007u, 0x00000087u,
    0x00000000u, 0xE0000000u, 0x001FFFFFu, 0xFE07FFFFu, 0x00000007u, 0x00000000u,
    0xE0000000u, 0x001FFFFFu, 0xFE07FFFFu, 0x00000001u, 0x00000000u, 0xE0000000u,
    0xFFFFFFFFu, 0xFE1FFFFFu, 0x00000001u, 0x00000000u, 0x80000000u, 0xFFFFFFFFu,
    0x7E1FF01Fu, 0x00000000u, 0x00000000u, 0x80000000u, 0xFFFFFFFFu, 0x7F9FF01Fu,
    0x00000000u, 0x00000000u, 0x00000000u, 0xFE1FFFFFu, 0x1FDFF01Fu, 0x00000000u,
    0x00000000u, 0x00000000u, 0xFE1FFFFEu, 0x1FFFE01Fu, 0x00000000u, 0x00000000u,
    0x00000000u, 0x3FFFFFFCu, 0x1FFFC000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x0FFFFFF0u, 0x0FFFC000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0FFFFFF0u,
    0x0FFFC000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x007FFFC0u, 0x0FFF0000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x007FFFC0u, 0x0FFF0000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x007FFF00u, 0x0FFC0000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x007FFF00u, 0x0FFC0000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x001FFE00u, 0x0FFC0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0003F800u,
    0x03FC0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0001F800u, 0x03FF0000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x03FF0000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x03FF0000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x03FF8000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x03FFC000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x03FFC000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x03FFC000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x03FFF000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x03FFF000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x03FFF000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x03FFF000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x03FFFE00u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x03FFFE00u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x03FFFE00u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x0FFFFF80u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x0FFFFF80u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x0FFFFF80u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x0FFFFF80u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x7FFFFFF0u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00000007u,
    0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00000007u, 0x00000000u,
    0x00000000u, 0xFFC00000u, 0xFFFFFFFFu, 0x00001FFFu, 0x00000000u, 0x00000000u,
    0xFFE00000u, 0xFFFFFFFFu, 0x00001FFFu, 0x00000000u, 0xF8000000u, 0xFFFF0000u,
    0xFFFFFFFFu, 0x0003FFFFu, 0x000000F8u, 0xE0000000u, 0x001F0001u, 0xFFFFFFFFu,
    0x0007C007u, 0x0000003Cu, 0xE0000000u, 0x001FFFFFu, 0xFFFFFFFFu, 0xFFFFC007u,
    0x0000003Fu, 0x00000000u, 0x0007FFFEu, 0xFFFFFFFFu, 0xFFFF0007u, 0x00000003u,
    0x00000000u, 0x0007FE00u, 0xFFFFFFFFu, 0x03FF8007u, 0x00000000u, 0x80000000u,
    0xFFFFFE00u, 0xFFFFFFFFu, 0x01FFFFFFu, 0x00000008u, 0x80000000u, 0xFFFF3E00u,
    0xFFFFFFFFu, 0x01E7FFFFu, 0x00000008u, 0xE0000000u, 0xFFFF3E03u, 0xFFFFFFFFu,
    0x01E3FFFFu, 0x0000003Eu, 0x00000000u, 0xFFC70E02u, 0x0FFFFF80u, 0x03C79FF8u,
    0x00000002u, 0x00000000u, 0xFFC78FFEu, 0x07FFFF80u, 0xFF8F1FF8u, 0x00000003u,
    0x00000000u, 0xFFC18FFFu, 0x07FFFF80u, 0xFF8E1FF8u, 0x00000003u, 0x00000000u,
    0x01C001F0u, 0x0FFFFF80u, 0x7C001C00u, 0x00000000u, 0x00000000u, 0x01FC01F0u,
    0x7FFFFFF0u, 0x7C01FC00u, 0x00000000u, 0x00000000u, 0x001C0070u, 0x7FFFFFF0u,
    0x7001C000u, 0x00000000u, 0x00000000u, 0xFE1FF870u, 0xFFFFFFFFu, 0x70FFC3FFu,
    0x00000000u, 0x00000000u, 0x02078870u, 0xFFFFFFFFu, 0x78CF8307u, 0x00000000u,
    0x00000000u, 0x03C78FF0u, 0xFFFFFFFFu, 0x7F8F1E07u, 0x00000000u, 0x00000000u,
    0x800781C0u, 0x1C3FF0E7u, 0x1C0F000Fu, 0x00000000u, 0x00000000u, 0xC0038000u,
    0x1C3FF0E3u, 0x000E000Eu, 0x00000000u, 0x00000000u, 0xE001F800u, 0x1C3FF0E1u,
    0x00FE003Eu, 0x00000000u, 0x00000000u, 0xE001F800u, 0x1C1FC0C1u, 0x00FE003Cu,
    0x00000000u, 0x00000000u, 0xFF81FE00u, 0x1C0FC0C1u, 0x01FE07FCu, 0x00000000u,
    0x00000000u, 0xE3818000u, 0x3C1FC0E1u, 0x000E0F3Cu, 0x00000000u, 0x00000000u,
    0xE3F18000u, 0x7C3FF0F1u, 0x000E7E3Cu, 0x00000000u, 0x00000000u, 0xF9F18000u,
    0x4CFFF891u, 0x000E7CFCu, 0x00000000u, 0x00000000u, 0xF8B38000u, 0x4CFFF891u,
    0x000E64FEu, 0x00000000u, 0x00000000u, 0xFE1F8000u, 0x0FFFFF81u, 0x000FC1FCu,
    0x00000000u, 0x00000000u, 0xCE1C0000u, 0x0F870F81u, 0x0001C38Cu, 0x00000000u,
    0x00000000u, 0xCE1C0000u, 0x0F870F81u, 0x0001C18Eu, 0x00000000u, 0x00000000u,
    0xC2000000u, 0x0E070381u, 0x0000030Eu, 0x00000000u, 0x00000000u, 0xC3800000u,
    0x7E0703F1u, 0x0000070Eu, 0x00000000u, 0x00000000u, 0xC3800000u, 0x7E0703F1u,
    0x0000070Eu, 0x00000000u, 0x00000000u, 0xC1000000u, 0x72070371u, 0x0000000Eu,
    0x00000000u, 0x00000000u, 0xC0000000u, 0xF3E71E3Fu, 0x0000000Fu, 0x00000000u,
    0x00000000u, 0x00000000u, 0xF0E73C3Fu, 0x00000007u, 0x00000000u, 0x00000000u,
    0x00000000u, 0xF0FFF83Fu, 0x00000007u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x70FFF830u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x70FFF830u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00FFF800u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00400000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u
);
// --- end generated ---

const int kLogoWordsPerRow = (kLogoWidth + 31) / 32;

// How many of each there are. The host mirrors these numbers in `src/ui/logo.hpp` and
// `tests/test_logo.cpp` checks the mirror against this file — because a count raised here without a
// branch to go with it is a seed that draws nothing, which is trap 7 in the handover wearing an
// interface's clothes: *"nothing here" and "I could not fit it" must never be the same answer.*
//
// The other four — the arrangements, the sorting algorithms, their keys and their axis — are
// declared beside the code that reads them, further down.
const uint kLogoAnimations = 24u;
const uint kLogoPatterns = 20u;
const uint kLogoPalettes = 12u;

// How much room the mark is drawn into, as a multiple of the square the drawing occupies. The host
// inflates the command's rectangle by exactly this and `shell.comp` takes it back out again, so the
// drawing keeps its size and everything thrown wide of it is still evaluated. `src/ui/logo.hpp`
// carries the same number and a test checks the pair: a host binning one rectangle while this file
// draws another is a mark clipped to a box that appears nowhere in either.
const float kLogoReach = 4.0;

// Is there mark at this pixel of the drawing?
bool logo_bit(ivec2 at) {
    if (at.x < 0 || at.y < 0 || at.x >= kLogoWidth || at.y >= kLogoHeight) return false;
    int word = at.y * kLogoWordsPerRow + (at.x >> 5);
    return ((kLogoBits[word] >> uint(at.x & 31)) & 1u) != 0u;
}

// The mark at a point in its own square, 0 to 1 across. Nearest, deliberately: the drawing is
// pixels and this is a game about pixels that are matter. Smoothing it would make the one picture
// on the screen the only thing in the game with soft edges.
float logo_mask(vec2 uv) {
    return logo_bit(ivec2(floor(uv * vec2(kLogoWidth, kLogoHeight)))) ? 1.0 : 0.0;
}

// How far this point is from the nearest edge of the mark, in mark-pixels, searched to `reach`.
// Used by the two patterns that draw the mark as its own contour.
float logo_edge(vec2 uv, float reach) {
    ivec2 centre = ivec2(floor(uv * vec2(kLogoWidth, kLogoHeight)));
    bool here = logo_bit(centre);
    int limit = int(reach);
    for (int r = 1; r <= limit; ++r) {
        for (int i = -r; i <= r; ++i) {
            if (logo_bit(centre + ivec2(i, -r)) != here) return float(r);
            if (logo_bit(centre + ivec2(i, r)) != here) return float(r);
            if (logo_bit(centre + ivec2(-r, i)) != here) return float(r);
            if (logo_bit(centre + ivec2(r, i)) != here) return float(r);
        }
    }
    return reach;
}

// --- picking things out of a seed ---------------------------------------------------------------

uint logo_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// A number from a seed and a salt, so every choice is independent of every other. Salting rather
// than consuming bits in order means adding a twenty-fifth animation does not change which palette
// every existing seed picks -- which matters, because a seed is a thing a player can arrive at
// twice and should recognise. What it does NOT protect is the choice whose bound changed: raising
// the count of animations moves which animation each seed lands on, and nothing can prevent that
// short of never adding one.
//
// The salts in use, so a new choice does not quietly land on a taken one:
//
//   11-14   the palette: scheme, base hue, saturation, floor
//   21      the first pattern, 22-24 its scale, angle and drift
//   31      the first animation, 32-33 its speed and amount
//   41      the second animation, 62-63 its speed and amount
//   42-44   how the two animations are mixed: rate, swing, phase
//   51      the second pattern, 55-57 its scale, angle and drift
//   52-54   how the two patterns are mixed: rate, lean, angle
//   71-74   the slices: the sorting algorithm, its key, its axis, its exchanges a second
//   75-76   how far out of order the slices go: swing and rate
//   81-84   the arrangement: which one, and its swing, rate and phase
//   85-88   the two turns an arrangement in three or four dimensions is seen through
uint logo_pick(uint seed, uint salt, uint bound) {
    return logo_hash(seed ^ (salt * 0x9E3779B9u)) % max(bound, 1u);
}

float logo_unit(uint seed, uint salt) {
    return float(logo_hash(seed ^ (salt * 0x85EBCA6Bu)) & 0xFFFFFFu) / float(0xFFFFFF);
}

float logo_range(uint seed, uint salt, float low, float high) {
    return low + (high - low) * logo_unit(seed, salt);
}

float logo_cell_random(vec2 cell) {
    uint h = logo_hash(uint(int(cell.x) * 374761393 + int(cell.y) * 668265263 + 1013904223));
    return float(h & 0xFFFFu) / 65535.0;
}

float logo_noise(vec2 p) {
    vec2 cell = floor(p);
    vec2 within = fract(p);
    within = within * within * (3.0 - 2.0 * within);
    float a = logo_cell_random(cell);
    float b = logo_cell_random(cell + vec2(1.0, 0.0));
    float c = logo_cell_random(cell + vec2(0.0, 1.0));
    float d = logo_cell_random(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, within.x), mix(c, d, within.x), within.y);
}

vec3 logo_hsv(float h, float s, float v) {
    vec3 k = fract(vec3(h) + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0;
    return v * mix(vec3(1.0), clamp(abs(k) - 1.0, 0.0, 1.0), s);
}

vec2 logo_turn(vec2 v, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return vec2(v.x * c - v.y * s, v.x * s + v.y * c);
}

// --- the palette --------------------------------------------------------------------------------
//
// Twelve schemes, and each is a rule for turning one number in 0 to 1 into a colour. `bias` is the
// player's own logo colour from the settings, which shifts the family rather than replacing it:
// somebody who chose amber gets amber's relatives, and somebody who never looked gets whatever the
// seed says.
vec3 logo_palette(uint seed, float at, vec3 bias) {
    uint scheme = logo_pick(seed, 11u, kLogoPalettes);
    float base = fract(logo_unit(seed, 12u) + bias.r * 0.5 + bias.g * 0.25 + bias.b * 0.125);
    float saturation = logo_range(seed, 13u, 0.45, 1.0);
    // The DARK end of whatever ramp the scheme draws, and it is a legibility floor rather than a
    // taste: the title room is nearly black, so a scheme free to take its floor to a quarter puts
    // some of the two and a half million combinations on the screen as a shape you have to look for.
    // Every scheme below is free to spend the range above this and none of them may go under it.
    float floor_value = logo_range(seed, 14u, 0.40, 0.62);
    at = clamp(at, 0.0, 1.0);

    float hue = base;
    float value = mix(floor_value, 1.0, at);
    if (scheme == 0u) {
        hue = base;                                       // one hue, all of it spent on brightness
    } else if (scheme == 1u) {
        hue = base + (at < 0.5 ? 0.0 : 0.5);              // complementary, split at the middle
        value = mix(floor_value, 1.0, abs(at - 0.5) * 2.0);
    } else if (scheme == 2u) {
        hue = base + floor(at * 3.0) / 3.0;               // triad, in three bands
    } else if (scheme == 3u) {
        hue = base + (at - 0.5) * 0.16;                   // analogous, a narrow sweep
    } else if (scheme == 4u) {
        hue = mix(base, base + 0.5, smoothstep(0.35, 0.65, at));   // warm to cool, crossfaded
        saturation *= mix(1.0, 0.55, abs(at - 0.5) * 2.0);
    } else if (scheme == 5u) {
        hue = base + (at < 0.6 ? 0.0 : 0.42);             // duotone, with a bright edge
        value = mix(floor_value, 1.0, pow(at, 0.6));
        saturation *= (at > 0.85) ? 0.35 : 1.0;
    } else if (scheme == 6u) {
        hue = base + floor(at * 4.0) * 0.25;              // tetrad, four flat quarters
        value = mix(floor_value, 0.95, fract(at * 4.0) * 0.4 + 0.6);
    } else if (scheme == 7u) {
        // Hot metal: one hue, and the top of the range goes white rather than brighter, which is
        // what heat actually looks like and what makes a highlight read as a highlight.
        hue = base + at * 0.10;
        value = mix(floor_value, 1.0, pow(at, 0.45));
        saturation *= 1.0 - smoothstep(0.7, 1.0, at) * 0.85;
    } else if (scheme == 8u) {
        // Ash: almost no colour at all, except one band that has all of it. The restraint is the
        // point -- a mark that is grey with one live seam reads as material rather than as paint.
        hue = base;
        saturation *= 0.12 + 0.88 * exp(-(at - 0.72) * (at - 0.72) * 160.0);
        value = mix(floor_value, 1.0, at);
    } else if (scheme == 9u) {
        hue = base + at * 0.72;                           // most of the circle, at a steady value
        value = mix(mix(floor_value, 1.0, 0.55), 1.0, 0.45 + 0.55 * at);
    } else if (scheme == 10u) {
        // A stripe pair: two hues alternating along the range with their values apart, so the two
        // read as two even where the pattern under them is smooth.
        float which = step(0.5, fract(at * 4.0));
        hue = base + which * 0.33;
        value = mix(floor_value, 1.0, mix(0.45, 1.0, which) * (0.4 + 0.6 * at));
    } else {
        // Deep shadow: the dark end is the SATURATED end. Every other scheme here fades colour out
        // as it darkens, which is right for light and wrong for a shadow in a coloured room.
        hue = base;
        value = mix(floor_value, 1.0, at);
        saturation *= mix(1.0, 0.35, at);
    }
    return logo_hsv(fract(hue), saturation, value);
}

// --- the patterns -------------------------------------------------------------------------------
//
// Twenty fields over the mark. Each returns 0 to 1 and the palette turns it into colour, so a
// pattern never names a colour and a palette never knows what shape it is on.
//
// `salt` is where this pattern's own three continuous parameters live, so the two a seed picks are
// not the same field at two indices: 22-24 for the first, 55-57 for the second.
float logo_pattern_one(uint which, uint seed, uint salt, vec2 uv, float t) {
    float scale = logo_range(seed, salt, 3.0, 11.0);
    float turn = logo_unit(seed, salt + 1u) * 6.2831853;
    float drift = logo_range(seed, salt + 2u, -0.35, 0.35) * t;
    vec2 p = uv - 0.5;
    vec2 rotated = logo_turn(p, turn);

    if (which == 0u) return 0.5 + 0.5 * sin(drift);                      // flat, and it breathes
    if (which == 1u) return clamp(1.0 - uv.y + drift * 0.3, 0.0, 1.0);   // up the tree
    if (which == 2u) return fract(rotated.x * 1.2 + 0.5 + drift);        // a gradient, any angle
    if (which == 3u) return fract(length(p) * scale * 0.5 - drift);      // rings from the middle
    if (which == 4u) {                                                   // voxels, checked
        vec2 cell = floor(uv * scale * 1.6);
        return clamp(mod(cell.x + cell.y, 2.0) * 0.7 + 0.15 +
                     0.15 * sin(drift * 3.0 + cell.x + cell.y), 0.0, 1.0);
    }
    if (which == 5u) return logo_noise(uv * scale + vec2(drift, drift * 0.7));   // clouds
    if (which == 6u) {                                                   // cells, nearest site
        vec2 grid = uv * scale;
        vec2 base = floor(grid);
        float best = 9.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec2 neighbour = base + vec2(float(x), float(y));
                vec2 site = neighbour + vec2(logo_cell_random(neighbour),
                                             logo_cell_random(neighbour + 41.0));
                site += 0.3 * vec2(sin(drift * 2.0 + site.x), cos(drift * 2.0 + site.y));
                best = min(best, length(grid - site));
            }
        }
        return clamp(best, 0.0, 1.0);
    }
    if (which == 7u) {                                                   // spokes
        float angle = atan(p.y, p.x) + drift;
        return 0.5 + 0.5 * sin(angle * floor(scale));
    }
    if (which == 8u) {                                                   // bands, jittered
        float band = rotated.y * scale + logo_noise(uv * 6.0) * 1.5 + drift;
        return 0.5 + 0.5 * sin(band);
    }
    if (which == 9u) {                                                   // its own contour
        return clamp(logo_edge(uv, 7.0) / 7.0 + 0.12 * sin(drift * 4.0), 0.0, 1.0);
    }
    if (which == 10u) {                                                  // a spiral, winding
        float arms = floor(scale * 0.4) + 1.0;
        return fract(atan(p.y, p.x) / 6.2831853 * arms + length(p) * scale * 0.5 - drift);
    }
    if (which == 11u) {                                                  // plaid, two ways at once
        return 0.5 + 0.5 * sin(rotated.x * scale * 3.0 + drift) *
                             sin(rotated.y * scale * 3.0 - drift);
    }
    if (which == 12u) {                                                  // hexagons, packed
        const vec2 lattice = vec2(1.0, 1.7320508);
        vec2 grid = uv * scale;
        vec2 a = mod(grid, lattice) - lattice * 0.5;
        vec2 b = mod(grid + lattice * 0.5, lattice) - lattice * 0.5;
        vec2 near = (dot(a, a) < dot(b, b)) ? a : b;
        return clamp(length(near) * 1.6 + 0.12 * sin(drift * 3.0), 0.0, 1.0);
    }
    if (which == 13u) {                                                  // square rings, not round
        return fract(max(abs(rotated.x), abs(rotated.y)) * scale - drift);
    }
    if (which == 14u) {                                                  // stripes, warped
        return 0.5 + 0.5 * sin(rotated.x * scale * 4.0 + logo_noise(uv * 3.0) * 4.0 + drift * 2.0);
    }
    if (which == 15u) {                                                  // noise warped by noise
        vec2 warp = vec2(logo_noise(uv * scale * 0.5 + drift),
                         logo_noise(uv * scale * 0.5 + 7.3 - drift));
        return clamp(logo_noise(uv * scale + warp * 1.6) * 0.75 +
                     logo_noise(uv * scale * 2.3) * 0.25, 0.0, 1.0);
    }
    if (which == 16u) {                                                  // columns, each its own
        float column = floor(uv.x * scale * 2.0);
        float own = logo_cell_random(vec2(column, 5.0));
        return clamp(own * 0.85 + 0.15 * sin(drift * 4.0 + column), 0.0, 1.0);
    }
    if (which == 17u) {                                                  // sparks, four of them
        float best = 9.0;
        for (int i = 0; i < 4; ++i) {
            float own = float(i) * 1.7;
            vec2 at = 0.32 * vec2(sin(drift * 2.0 + own), cos(drift * 1.6 + own * 1.3));
            best = min(best, length(p - at));
        }
        return clamp(1.0 - best * scale * 0.35, 0.0, 1.0);
    }
    if (which == 18u) {                                                  // two waves, interfering
        float r1 = length(p - vec2(0.20 * sin(drift), 0.20 * cos(drift * 0.8)));
        float r2 = length(p + vec2(0.22 * cos(drift * 1.1), 0.18 * sin(drift * 0.9)));
        return clamp(0.5 + 0.25 * sin(r1 * scale * 3.0) + 0.25 * sin(r2 * scale * 3.0), 0.0, 1.0);
    }
    // Its own contour again, but in steps rather than as a ramp: the mark drawn as the contour map
    // of itself, which is a different picture from the same measurement.
    return clamp(floor(logo_edge(uv, 6.0) * 0.5) / 3.0 + 0.1 * sin(drift * 3.0), 0.0, 1.0);
}

// How much of the second pattern there is at this point. `lean` is what makes this worth doing at
// all: at nought the whole mark crossfades together, and anywhere else the boundary between the two
// patterns is a front travelling across the drawing, which is the thing that reads as *morphing*
// rather than as a dissolve.
float logo_pattern_mix(uint seed, vec2 uv, float t) {
    float rate = logo_range(seed, 52u, 0.05, 0.35);
    float lean = logo_range(seed, 53u, -1.7, 1.7);
    float turn = logo_unit(seed, 54u) * 6.2831853;
    float along = dot(uv - 0.5, vec2(cos(turn), sin(turn))) * lean;
    return 0.5 + 0.5 * sin(t * rate * 6.2831853 + along * 3.0);
}

// Both patterns, mixed. The second is evaluated only where it is worth anything, and the weight is
// a smooth field rather than a per-pixel choice, so the branch below is coherent over most of the
// mark and divergent only along the front.
float logo_pattern(uint seed, vec2 uv, float t) {
    uint first = logo_pick(seed, 21u, kLogoPatterns);
    uint second = logo_pick(seed, 51u, kLogoPatterns);
    float mixture = logo_pattern_mix(seed, uv, t);
    if (first == second) return logo_pattern_one(first, seed, 22u, uv, t);
    if (mixture <= 0.002) return logo_pattern_one(first, seed, 22u, uv, t);
    if (mixture >= 0.998) return logo_pattern_one(second, seed, 55u, uv, t);
    return mix(logo_pattern_one(first, seed, 22u, uv, t),
               logo_pattern_one(second, seed, 55u, uv, t), mixture);
}

// --- the animations -----------------------------------------------------------------------------
//
// What each one returns: where to sample the mark, how much of it to show, and a depth for the ones
// with a third dimension in them. `age` is seconds since this seed was chosen, so an animation can
// have a beginning; `t` is the clock, so it can also have no end.
struct LogoMotion {
    vec2 uv;
    float show;
    float depth;   // 0 is flat; anything else shades as a solid
    bool outside;  // the sample fell off the mark entirely
};

// One of the twenty-four. `salt` is where this one's speed and amount live, so the two a seed
// blends are not the same motion twice: 32-33 for the first, 62-63 for the second.
LogoMotion logo_motion_one(uint which, uint seed, uint salt, vec2 uv, float age, float t) {
    float speed = logo_range(seed, salt, 0.7, 1.7);
    float amount = logo_range(seed, salt + 1u, 0.6, 1.4);
    float phase = age * speed;

    LogoMotion motion;
    motion.uv = uv;
    motion.show = 1.0;
    motion.depth = 0.0;
    motion.outside = false;

    if (which == 0u) {
        // Grown from the roots. The tree arrives the way a tree does: from the ground up, with the
        // canopy last and a little overshoot as it settles.
        float front = smoothstep(0.0, 2.2, phase) * 1.25;
        float wobble = 1.0 + 0.04 * sin(phase * 4.0) * exp(-phase * 0.8);
        motion.uv = (uv - vec2(0.5, 1.0)) / wobble + vec2(0.5, 1.0);
        motion.show = smoothstep(0.0, 0.16, front - (1.0 - motion.uv.y));
    } else if (which == 1u) {
        // Wind. Nothing at the roots moves; the canopy moves most, and the whole thing leans back
        // against the gust rather than swinging past it.
        float height = clamp(1.0 - uv.y * 1.35, 0.0, 1.0);
        float gust = sin(t * 1.1 * speed) * 0.5 + sin(t * 2.7 * speed + 1.3) * 0.2;
        motion.uv.x -= gust * height * height * 0.09 * amount;
        motion.uv.y -= abs(gust) * height * height * 0.012;
    } else if (which == 2u) {
        // Dissolved into voxels and put back. Each cell has its own scatter and its own arrival
        // time, so it reassembles as a crowd rather than as one block.
        float cells = 22.0;
        vec2 cell = floor(uv * cells);
        float own = logo_cell_random(cell + 17.0);
        float back = smoothstep(0.0, 1.0, clamp(phase * 0.55 - own * 0.9, 0.0, 1.0));
        float angle = own * 6.2831853;
        vec2 away = vec2(cos(angle), sin(angle)) * (1.0 - back) * 0.55 * amount;
        motion.uv = uv + away;
        motion.show = back * back;
    } else if (which == 3u) {
        // A ripple out from the middle, as if the press landed in water.
        vec2 from = uv - 0.5;
        float r = length(from);
        float wave = sin(r * 26.0 - phase * 7.0) * exp(-r * 3.0) * exp(-phase * 0.55);
        motion.uv = uv + normalize(from + vec2(1e-5)) * wave * 0.06 * amount;
    } else if (which == 4u) {
        // A line sweeps across and the mark appears behind it, lit along the front.
        float across = fract(phase * 0.35) * 1.4;
        float edge = uv.x * 0.7 + uv.y * 0.3;
        motion.show = max(smoothstep(0.0, 0.08, across - edge), step(1.2, phase * 0.35));
        motion.depth = 0.35 * exp(-abs(edge - across) * 22.0);
    } else if (which == 5u) {
        // Turned about its own axis. A real projection rather than a squash: the far half
        // compresses towards the axis the way a turning card does.
        float spin = phase * 0.8 * amount;
        float x = uv.x - 0.5;
        float denominator = max(1.0 - x * sin(spin) * 0.55, 0.25);
        motion.uv.x = 0.5 + x * cos(spin) / denominator;
        motion.uv.y = 0.5 + (uv.y - 0.5) / denominator;
        motion.depth = x * sin(spin);
        motion.outside = abs(motion.uv.x - 0.5) > 0.5 || abs(motion.uv.y - 0.5) > 0.5;
    } else if (which == 6u) {
        // A slab. The mark is given thickness and the ray walks through it, so the sides of the
        // shape are visible and the whole thing has a body.
        float lean_x = sin(phase * 0.5) * 0.5 * amount;
        float lean_y = cos(phase * 0.37) * 0.28 * amount;
        float thickness = 0.10;
        float found = 0.0;
        float depth = 0.0;
        const int kSteps = 10;
        for (int i = 0; i < kSteps; ++i) {
            float along = (float(i) / float(kSteps - 1) - 0.5) * thickness;
            vec2 at = uv + vec2(along * lean_x, along * lean_y);
            if (logo_mask(at) > 0.0) {
                found = 1.0;
                depth = along / thickness;
                motion.uv = at;
                break;
            }
        }
        motion.show = found;
        motion.depth = depth * 2.0;
        motion.outside = found < 0.5;
    } else if (which == 7u) {
        // Layers at different depths, moved by a virtual eye. The canopy sits in front of the
        // roots and they part as it turns.
        float eye = sin(phase * 0.6) * 0.05 * amount;
        float layer = 1.0 - uv.y;
        motion.uv.x += eye * (layer - 0.5) * 2.0;
        motion.depth = (layer - 0.5) * 1.4;
    } else if (which == 8u) {
        // Shattered upward and fallen back, each cell on its own clock.
        float cells = 16.0;
        vec2 cell = floor(uv * cells);
        float own = logo_cell_random(cell + 7.0);
        float local = clamp(phase * 0.8 - own * 0.25, 0.0, 3.0);
        float lift = max(0.0, sin(local * 2.2) * exp(-local * 1.1));
        motion.uv.y += lift * (0.25 + own * 0.4) * amount;
        motion.uv.x += lift * (own - 0.5) * 0.35 * amount;
    } else if (which == 9u) {
        // Breathing: a slow scale, and nothing else at all.
        float pulse = 1.0 + 0.05 * sin(t * 1.3 * speed) * amount;
        motion.uv = (uv - 0.5) / pulse + 0.5;
    } else if (which == 10u) {
        // The roots take hold first and the canopy follows, which is the reverse of `grow` and
        // reads entirely differently: the tree arrives already grown.
        float root_front = smoothstep(0.0, 1.4, phase);
        float top_front = smoothstep(0.6, 2.4, phase);
        float below = smoothstep(0.55, 0.75, uv.y);
        motion.show = mix(top_front, root_front, below);
        motion.uv.y += (1.0 - root_front) * below * 0.05;
    } else if (which == 11u) {
        // Turned about the horizontal, like a card.
        float spin = phase * 0.7 * amount;
        float y = uv.y - 0.5;
        float denominator = max(1.0 - y * sin(spin) * 0.55, 0.25);
        motion.uv.y = 0.5 + y * cos(spin) / denominator;
        motion.uv.x = 0.5 + (uv.x - 0.5) / denominator;
        motion.depth = y * sin(spin);
        motion.outside = abs(motion.uv.y - 0.5) > 0.5 || abs(motion.uv.x - 0.5) > 0.5;
    } else if (which == 12u) {
        // Risen from under the floor and settled, overshooting once. `grow` uncovers the drawing in
        // place; this one moves the whole of it, which is a different thing to watch.
        float land = clamp(phase * 0.9, 0.0, 4.0);
        float ease = 1.0 - exp(-land * 2.2) * cos(land * 3.4);
        motion.uv.y -= (1.0 - clamp(ease, 0.0, 1.0)) * 0.9 * amount;
        motion.show = smoothstep(0.0, 0.1, land);
    } else if (which == 13u) {
        // An iris out of the middle, lit along its own front.
        float r = length((uv - 0.5) * vec2(1.0, 1.15));
        float front = smoothstep(0.0, 2.0, phase) * 0.95;
        motion.show = smoothstep(0.04, 0.0, r - front);
        motion.depth = 0.4 * exp(-abs(r - front) * 26.0);
    } else if (which == 14u) {
        // Columns arrive from alternate sides. The drawing assembles as a set of vertical slices,
        // which is the shape a voxel column actually has.
        float columns = 14.0;
        float column = floor(uv.x * columns);
        float own = logo_cell_random(vec2(column, 3.0));
        float arrived = smoothstep(0.0, 1.0, clamp(phase * 0.8 - own * 0.8, 0.0, 1.0));
        float side = (mod(column, 2.0) < 1.0) ? -1.0 : 1.0;
        motion.uv.x += (1.0 - arrived) * side * 0.7 * amount;
        motion.show = arrived;
    } else if (which == 15u) {
        // The same, in rows, which reads as a blind turning rather than as a wall being built.
        float rows = 12.0;
        float row = floor(uv.y * rows);
        float own = logo_cell_random(vec2(9.0, row));
        float arrived = smoothstep(0.0, 1.0, clamp(phase * 0.8 - own * 0.7, 0.0, 1.0));
        float side = (mod(row, 2.0) < 1.0) ? -1.0 : 1.0;
        motion.uv.x += (1.0 - arrived) * side * 0.8 * amount;
        motion.show = arrived;
        motion.depth = (1.0 - arrived) * side * 0.5;
    } else if (which == 16u) {
        // Jelly: the two axes breathe out of phase, pinned at the roots so it squashes rather than
        // floats. One scale on both axes is `breathe`; two is a thing with weight.
        float sx = 1.0 + 0.06 * sin(t * 1.7 * speed) * amount;
        float sy = 1.0 + 0.06 * sin(t * 1.7 * speed + 1.9) * amount;
        motion.uv = (uv - vec2(0.5, 1.0)) / vec2(sx, sy) + vec2(0.5, 1.0);
    } else if (which == 17u) {
        // A small orbit, with the light following it round. Nothing about the shape changes, which
        // is why it can carry a pattern that changes a great deal.
        float angle = t * 0.5 * speed;
        motion.uv += vec2(cos(angle), sin(angle * 1.3)) * 0.03 * amount;
        motion.depth = 0.25 * sin(angle);
    } else if (which == 18u) {
        // Hung from its canopy and swinging: the roots travel furthest, which is the opposite of
        // `wind` and reads as a thing suspended rather than a thing planted.
        float below = clamp(uv.y, 0.0, 1.0);
        motion.uv.x -= sin(t * 1.6 * speed) * 0.09 * amount * below * below;
    } else if (which == 19u) {
        // A tumble that comes to rest square. It starts most of a turn away and eases to nothing,
        // so the last second of it is the mark settling rather than spinning.
        float angle = 2.8 * exp(-phase * 0.7) * amount;
        motion.uv = 0.5 + logo_turn(uv - 0.5, angle);
        motion.depth = 0.6 * sin(angle);
    } else if (which == 20u) {
        // Melting, and recovering: the lower the point, the further down the drawing is stretched,
        // by a per-column amount so the run is uneven the way a run is.
        float run = 0.5 + 0.5 * sin(t * 0.5 * speed);
        float where = smoothstep(0.35, 1.0, uv.y);
        motion.uv.y -= where * run * 0.10 * amount *
                       (0.6 + 0.4 * logo_noise(vec2(uv.x * 9.0, 0.0)));
    } else if (which == 21u) {
        // Wound in: every cell arrives along its own arc, small and turned, and unwinds into place.
        float cells = 20.0;
        vec2 cell = floor(uv * cells);
        float own = logo_cell_random(cell + 11.0);
        float back = smoothstep(0.0, 1.0, clamp(phase * 0.5 - own * 0.5, 0.0, 1.0));
        motion.uv = 0.5 + logo_turn(uv - 0.5, (1.0 - back) * 2.4 * amount) / mix(0.6, 1.0, back);
        motion.show = back;
    } else if (which == 22u) {
        // A chisel travels across it: the band it is in is absent, what is just past it has
        // dropped, and the fresh cut catches the light. This is the game's own subject, so it is
        // the one animation here that is a picture of the verb rather than of the noun.
        float front = fract(phase * 0.22) * 1.4 - 0.2;
        float along = uv.x * 0.6 + (1.0 - uv.y) * 0.4;
        float gap = (along - front) * 14.0;
        gap = exp(-gap * gap);
        motion.show = 1.0 - gap;
        motion.uv.y -= gap * 0.05 * amount;
        motion.depth = gap * 0.6;
    } else {
        // Rain: cells fall in from above, each on its own delay, easing to a stop. The last thing
        // to land is not the last thing to start, which is what keeps it from reading as a wipe.
        float cells = 18.0;
        vec2 cell = floor(uv * cells);
        float own = logo_cell_random(cell + 29.0);
        float land = clamp(phase * 0.6 - own * 1.2, 0.0, 1.0);
        float ease = 1.0 - (1.0 - land) * (1.0 - land);
        motion.uv.y += (1.0 - ease) * (0.9 + own * 0.5) * amount;
        motion.show = step(0.001, land);
    }
    return motion;
}

// Both animations, blended. Two rules make this safe to do to any pair:
//
//   the weight is uniform over the whole mark, so the drawing never tears -- a blend that varied
//   across the picture would sample two different motions either side of a boundary and cut the
//   mark in half;
//
//   an arm that says `outside` contributes NO position. Its uv is off the card and averaging it in
//   would drag the whole picture sideways for as long as the far half of a turning card is facing
//   away. It does still cost coverage, because that half genuinely is not there.
LogoMotion logo_motion(uint seed, vec2 uv, float age, float t) {
    uint first = logo_pick(seed, 31u, kLogoAnimations);
    uint second = logo_pick(seed, 41u, kLogoAnimations);

    // How the two are mixed. `swing` decides how much of the pair a seed actually spends: some sit
    // almost entirely on their first animation with the second only breathing through it, and some
    // travel the whole way and back.
    float rate = logo_range(seed, 42u, 0.03, 0.22);
    float swing = logo_range(seed, 43u, 0.15, 0.5);
    float middle = logo_range(seed, 44u, 0.15, 0.85);
    float weight = clamp(middle + swing * sin(t * rate * 6.2831853), 0.0, 1.0);
    if (first == second) weight = 0.0;

    LogoMotion a = logo_motion_one(first, seed, 32u, uv, age, t);
    if (weight <= 0.002) return a;
    LogoMotion b = logo_motion_one(second, seed, 62u, uv, age, t);
    if (weight >= 0.998) return b;

    float wa = (1.0 - weight) * (a.outside ? 0.0 : 1.0);
    float wb = weight * (b.outside ? 0.0 : 1.0);
    float total = wa + wb;

    LogoMotion motion;
    motion.outside = total <= 0.0001;
    if (motion.outside) {
        motion.uv = uv;
        motion.show = 0.0;
        motion.depth = 0.0;
        return motion;
    }
    motion.uv = (a.uv * wa + b.uv * wb) / total;
    motion.depth = (a.depth * wa + b.depth * wb) / total;
    // Coverage is NOT renormalised: an arm that is facing away is coverage that is genuinely gone.
    motion.show = a.show * wa + b.show * wb;
    return motion;
}

// --- what the host decided ----------------------------------------------------------------------
//
// Three things have to be the same for every pixel of the mark, and all three are worked out on the
// host and read here: the order the sort has reached, which way the mark's slices run, and the two
// weights that say how far out of place the slices and the blocks are.
//
// **The sort is the host's and cannot be anything else.** A pixel cannot ask what another pixel
// decided, so a shader can only sort an array small enough to hold in registers and re-sort from
// scratch for every pixel — which is what the first version of this did, and it bounded the mark to
// sixteen slices and to the algorithms whose comparisons are local. On the host it is one slice per
// voxel row or column of the drawing, ten real algorithms, once a frame instead of once a pixel, and
// covered by tests that run each one to completion. `src/ui/logo.hpp` is the whole argument.
//
// This is the one include in the project that reads a buffer it does not declare. `characters` is
// declared in `shaders/shell.comp` above the include, because it is the same buffer the text runs
// out of — a hundred and sixty words does not deserve an upload of its own.
const uint kLogoLanes = 160u;
const uint kLogoFrameWords = 3u + kLogoLanes;

struct LogoFrame {
    float lane;       // how far the slices are from where they belong, 0 to 1
    float arrange;    // and how far into its arrangement the mark is
    bool along_x;     // whether the slices run across the mark or up it
    uint base;        // where this frame's permutation starts
};

LogoFrame logo_frame(uint base) {
    LogoFrame frame;
    frame.lane = uintBitsToFloat(characters[base]);
    frame.arrange = uintBitsToFloat(characters[base + 1u]);
    frame.along_x = characters[base + 2u] != 0u;
    frame.base = base + 3u;
    return frame;
}

// The drawing's own coordinate for a point, once the slices have been re-ordered.
//
// A DISPLACEMENT rather than a choice: at weight nought this is the identity to the last bit, so a
// mark that is not being sorted is the mark exactly as it was drawn. One slice is one voxel row of
// the drawing, so nothing here is quantised to anything coarser than the picture itself.
vec2 logo_lane_map(vec2 uv, LogoFrame frame) {
    if (frame.lane <= 0.0) return uv;
    float along = frame.along_x ? uv.x : uv.y;
    if (along < 0.0 || along >= 1.0) return uv;   // off the drawing: there is no slice to move
    float scaled = along * float(kLogoLanes);
    uint at = min(uint(scaled), kLogoLanes - 1u);
    float within = scaled - float(at);
    float target = (float(characters[frame.base + at]) + within) / float(kLogoLanes);
    float mixed = mix(along, target, frame.lane);
    return frame.along_x ? vec2(mixed, uv.y) : vec2(uv.x, mixed);
}

// --- the arrangements ---------------------------------------------------------------------------
//
// The mark folded into something else: a ring, a spiral, a bar chart of its own weight, the surface
// of a turning cube, a sphere, a cylinder, a **four-dimensional** rotation of the plane it lives on,
// a leaning card, a sheet with a wave through it, a terrain, or a vortex.
//
// # Why every one of these is written BACKWARDS
//
// The first version of this cut the drawing into 144 blocks and asked, for every pixel, which of the
// 144 covered it. That works and it is what a scatter looks like when a pixel cannot be told what
// lands on it — but 144 blocks *is* a resolution, and a coarse one: the mark is 160 voxels across, so
// a block was thirteen voxels of drawing moved as one lump.
//
// So each arrangement is a **map from where a pixel is to where in the drawing it reads**, which is
// the same fold run in reverse. Then there is no grid at all: every voxel of the drawing goes to its
// own place, the loop over blocks disappears, and the whole thing costs a handful of multiplies
// instead of 144 iterations. The solids are ray casts — a ray for the pixel against a rotating cube,
// sphere or cylinder — so their occlusion is exact for free: the nearest hit is the only hit there is.
//
// What a backward map cannot do is draw two pieces of the drawing overlapping in one place. That is
// the trade, and it is worth it: a fold that reads the drawing at its own resolution is the thing
// that was asked for, and overlapping lumps are what it replaced.
const uint kLogoArrangements = 12u;

struct LogoFold {
    vec2 uv;       // where in the drawing this pixel reads
    float depth;   // -1 to 1, larger is nearer, for the shading
    float show;    // 0 where a solid's ray missed it entirely
};

// How much mark a column of the drawing holds, sampled. The bar chart and the terrain are pictures
// OF this number, so it has to come from the drawing rather than from a hash.
float logo_column_mark(float x) {
    if (x < 0.0 || x >= 1.0) return 0.0;
    float sum = 0.0;
    for (int i = 0; i < 16; ++i) sum += logo_mask(vec2(x, (float(i) + 0.5) / 16.0));
    return sum * 0.0625;
}

mat3 logo_basis(float yaw, float pitch) {
    float cy = cos(yaw);
    float sy = sin(yaw);
    float cp = cos(pitch);
    float sp = sin(pitch);
    mat3 turn_y = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
    mat3 turn_x = mat3(1.0, 0.0, 0.0, 0.0, cp, sp, 0.0, -sp, cp);
    return turn_y * turn_x;
}

// A ray for this pixel. The eye is in front of the mark's own square and looks at the middle of it,
// which is what makes a solid drawn here sit where the drawing sat.
void logo_ray(vec2 p, out vec3 from, out vec3 along) {
    from = vec3(0.0, 0.0, -2.4);
    along = normalize(vec3((p - 0.5) * 1.35, 2.4));
}

LogoFold logo_fold(uint which, uint seed, vec2 p, float t, float turn_a, float turn_b) {
    LogoFold fold;
    fold.uv = p;
    fold.depth = 0.0;
    fold.show = 1.0;
    if (which == 0u) return fold;   // where it already is: the arrangement that is no arrangement

    vec2 q = p - 0.5;
    if (which == 1u || which == 2u) {
        // Polar: the drawing wrapped round the middle. An annulus for the ring; the same thing with
        // the angle sheared by the radius for the spiral, which is one extra term and a completely
        // different figure.
        float r = length(q);
        float angle = atan(q.y, q.x) / 6.2831853 + 0.5;
        float inner = 0.10;
        float outer = 0.62;
        float shear = (which == 2u) ? 2.0 : 0.0;
        float up = (r - inner) / (outer - inner);
        fold.uv = vec2(fract(angle + turn_a * 0.15915494 + up * shear), clamp(1.0 - up, 0.0, 1.0));
        fold.show = (r >= inner && r <= outer) ? 1.0 : 0.0;
        fold.depth = 0.35 * cos((angle + turn_a * 0.15915494) * 6.2831853);
        return fold;
    }
    if (which == 3u) {
        // A bar chart of the drawing's own weight, column by column, standing on the bottom of the
        // square. Every column keeps every voxel it had — it is a scale, not a resampling.
        float height = 0.25 + 0.75 * logo_column_mark(p.x);
        float from_base = (1.0 - p.y) / max(height, 0.001);
        fold.uv = vec2(p.x, 1.0 - from_base);
        fold.show = (from_base <= 1.0 && from_base >= 0.0) ? 1.0 : 0.0;
        return fold;
    }
    if (which == 4u) {
        // A cube, cast against. Each of the six faces carries the whole drawing, so which face you
        // are looking at is which copy you see, and the far ones are behind the near ones because a
        // ray stops at the first slab it enters.
        vec3 from;
        vec3 along;
        logo_ray(p, from, along);
        mat3 basis = logo_basis(turn_a, turn_b);
        vec3 o = basis * from;          // into the cube's own frame: a rotation's inverse is its
        vec3 d = basis * along;         // transpose, and `basis` is built to be used this way round
        vec3 inverse_d = 1.0 / max(abs(d), vec3(1e-5)) * sign(d + vec3(1e-9));
        vec3 half_size = vec3(0.62);
        vec3 t0 = (-half_size - o) * inverse_d;
        vec3 t1 = (half_size - o) * inverse_d;
        vec3 near3 = min(t0, t1);
        vec3 far3 = max(t0, t1);
        float near_t = max(max(near3.x, near3.y), near3.z);
        float far_t = min(min(far3.x, far3.y), far3.z);
        if (far_t < near_t || far_t < 0.0) {
            fold.show = 0.0;
            return fold;
        }
        vec3 hit = o + d * near_t;
        vec2 face_uv;
        if (near_t == near3.x) face_uv = hit.zy * sign(-o.x);
        else if (near_t == near3.y) face_uv = hit.xz * sign(-o.y);
        else face_uv = hit.xy * sign(-o.z);
        fold.uv = face_uv / (half_size.x * 2.0) + 0.5;
        fold.depth = clamp(-hit.z * 1.4, -1.0, 1.0);
        return fold;
    }
    if (which == 5u) {
        // A sphere, and the drawing in its longitude and latitude. The poles pinch, which is what a
        // sphere does to a flat picture and is worth seeing rather than correcting.
        vec3 from;
        vec3 along;
        logo_ray(p, from, along);
        float b = dot(from, along);
        float c = dot(from, from) - 0.62 * 0.62;
        float discriminant = b * b - c;
        if (discriminant < 0.0) {
            fold.show = 0.0;
            return fold;
        }
        vec3 hit = normalize(from + along * (-b - sqrt(discriminant)));
        hit = logo_basis(turn_a, turn_b) * hit;
        fold.uv = vec2(atan(hit.x, hit.z) / 6.2831853 + 0.5, acos(clamp(hit.y, -1.0, 1.0)) / 3.1415927);
        fold.depth = clamp(-hit.z, -1.0, 1.0);
        return fold;
    }
    if (which == 6u) {
        // A cylinder: the drawing rolled into a tube, turning. The one solid here that keeps the
        // mark's own up-and-down, so the tree still stands while it turns.
        vec3 from;
        vec3 along;
        logo_ray(p, from, along);
        float lean = turn_b * 0.25;
        vec2 o = vec2(from.x, from.z);
        vec2 d = vec2(along.x, along.z);
        float a = dot(d, d);
        float b = dot(o, d);
        float c = dot(o, o) - 0.5 * 0.5;
        float discriminant = b * b - a * c;
        if (discriminant < 0.0) {
            fold.show = 0.0;
            return fold;
        }
        float hit_t = (-b - sqrt(discriminant)) / max(a, 1e-5);
        vec3 hit = from + along * hit_t;
        float up = (hit.y + hit.x * lean) / 1.2 + 0.5;
        fold.uv = vec2(fract(atan(hit.x, hit.z) / 6.2831853 + 0.5 + turn_a * 0.15915494),
                       clamp(1.0 - up, 0.0, 1.0));
        fold.show = (up >= 0.0 && up <= 1.0) ? 1.0 : 0.0;
        fold.depth = clamp(-hit.z * 1.6, -1.0, 1.0);
        return fold;
    }
    if (which == 7u || which == 8u) {
        // The plane the drawing lives on, turned — in four dimensions for the first of these and in
        // three for the second, and both are the same three lines because a rotation of a plane
        // followed by a projection is a projective map of that plane, whichever space it turned in.
        // So the fold is the INVERSE of a three by three matrix, which is exact and costs nothing,
        // and every voxel of the drawing lands where the turn put it.
        //
        // The fourth dimension is not decoration here: a plane turned in the x-w and y-z planes at
        // once comes back to itself on a different schedule than anything a three-dimensional turn
        // can do, and the projection through w makes it swell and shrink as it goes.
        vec3 ex;
        vec3 ey;
        vec3 origin;
        if (which == 7u) {
            float c1 = cos(turn_a);
            float s1 = sin(turn_a);
            float c2 = cos(turn_b * 1.3);
            float s2 = sin(turn_b * 1.3);
            // The plane's two axes and its middle, as points in four dimensions, each turned in two
            // planes and then divided by their own w to come back into three.
            vec4 ax = vec4(1.0, 0.0, 0.0, 0.0);
            vec4 ay = vec4(0.0, 1.0, 0.0, 0.0);
            vec4 at4 = vec4(0.0, 0.0, 0.0, 0.45);
            mat4 turn = mat4(c1, 0.0, 0.0, s1, 0.0, c2, -s2, 0.0, 0.0, s2, c2, 0.0, -s1, 0.0, 0.0, c1);
            vec4 tx = turn * ax;
            vec4 ty = turn * ay;
            vec4 to = turn * at4;
            ex = tx.xyz;
            ey = ty.xyz;
            origin = to.xyz + vec3(0.0, 0.0, 1.0 / max(2.3 - to.w, 0.7) - 0.4);
        } else {
            mat3 basis = logo_basis(turn_a, turn_b);
            ex = basis * vec3(1.0, 0.0, 0.0);
            ey = basis * vec3(0.0, 1.0, 0.0);
            origin = vec3(0.0, 0.0, 0.0);
        }
        // Where (u, v) lands on the screen: p = (origin + u*ex + v*ey) projected. In homogeneous
        // terms that is a three by three matrix acting on (u, v, 1), so reading it backwards is one
        // `inverse`.
        float kEye = 2.4;
        mat3 forward = mat3(vec3(ex.xy * kEye, -ex.z), vec3(ey.xy * kEye, -ey.z),
                            vec3(origin.xy * kEye, kEye - origin.z));
        vec3 found = inverse(forward) * vec3((p - 0.5) * 1.35, 1.0);
        if (abs(found.z) < 1e-6) {
            fold.show = 0.0;
            return fold;
        }
        fold.uv = found.xy / found.z + 0.5;
        fold.depth = clamp((origin.z + fold.uv.x * ex.z + fold.uv.y * ey.z) * -1.2, -1.0, 1.0);
        return fold;
    }
    if (which == 9u) {
        // A sheet with two waves crossing it. A warp read backwards is the same warp with its sign
        // turned round, which is why the smooth ones are the cheap ones.
        vec2 shift = vec2(0.06 * sin(p.y * 9.0 + t * 1.7), 0.06 * cos(p.x * 7.0 - t * 1.3));
        fold.uv = p - shift;
        fold.depth = 0.6 * sin(p.x * 7.0 - t * 1.3);
        return fold;
    }
    if (which == 10u) {
        // A terrain: every column of the drawing lifted by how much mark it holds, and the whole
        // thing sheared as though seen from above and to one side.
        float lift = logo_column_mark(p.x) * 0.35;
        float shear = (p.y - 0.5) * 0.25 * sin(turn_a * 0.5);
        fold.uv = vec2(p.x + shear, p.y + lift - 0.12);
        fold.depth = clamp(lift * 3.0 - 0.5, -1.0, 1.0);
        return fold;
    }
    // A vortex: turned about the middle by an angle that dies away with the radius, so the canopy
    // winds one way and the roots barely move. The one fold that keeps the mark readable the whole
    // way through, which is why it is worth having.
    float r = length(q);
    float wind = 3.0 * exp(-r * 3.2) * sin(t * 0.6 + turn_a);
    fold.uv = 0.5 + logo_turn(q, wind);
    fold.depth = 0.4 * sin(wind);
    return fold;
}

// --- the whole mark -----------------------------------------------------------------------------

// What age the OUTGOING combination is evaluated at during a transition. It was settled when it was
// replaced -- a seed lives twenty-odd seconds or until it is pressed -- so running its own clock from
// nought again would play its beginning backwards as it left, which is the one thing a change must
// not do.
const float kLogoSettled = 8.0;

// One combination, at a point, and the order the four stages run in is the whole design:
//
//   1. the ANIMATIONS decide where in the mark's square this pixel is looking (two of them, mixed);
//   2. the ARRANGEMENT folds that square into a ring, a solid, a turning plane or a terrain;
//   3. the SORT decides which slice of the drawing is standing at the place the fold arrived at;
//   4. the PATTERN and the PALETTE colour the point in the drawing that steps 2 and 3 reached -- so
//      a piece of the mark keeps its own colour as it travels, rather than picking up the colour of
//      wherever it has got to.
vec4 logo_one(uint seed, vec2 local, float age, float t, vec3 bias, LogoFrame frame) {
    LogoMotion motion = logo_motion(seed, local, age, t);
    if (motion.outside) return vec4(0.0);

    float show = clamp(motion.show, 0.0, 1.0);
    float depth = motion.depth;
    vec2 at = motion.uv;

    if (frame.arrange > 0.002) {
        uint which = logo_pick(seed, 81u, kLogoArrangements);
        float turn_a = t * logo_range(seed, 85u, 0.15, 0.7) + logo_unit(seed, 86u) * 6.2831853;
        float turn_b = t * logo_range(seed, 87u, 0.11, 0.5) + logo_unit(seed, 88u) * 6.2831853;
        LogoFold fold = logo_fold(which, seed, at, t, turn_a, turn_b);
        // Blended against where the pixel already reads, so at a weight of nought this is the
        // identity and the fold costs the picture nothing at all.
        at = mix(at, fold.uv, frame.arrange);
        depth += fold.depth * frame.arrange;
        show *= mix(1.0, fold.show, frame.arrange);
    }

    vec2 source = logo_lane_map(at, frame);
    float mark = logo_mask(source) * show;
    if (mark <= 0.0) return vec4(0.0);

    vec3 colour = logo_palette(seed, logo_pattern(seed, source, t), bias);

    // Depth, where there is any: the near face keeps its colour and what is behind falls away, so a
    // solid reads as a solid rather than as a picture of one.
    if (abs(depth) > 0.001) {
        colour *= clamp(0.55 + depth * 0.45, 0.25, 1.35);
    }

    // The seconds just after a new seed: a brief lift, so pressing it is answered by the drawing and
    // not only by what the drawing becomes.
    colour *= 1.0 + 0.6 * exp(-age * 3.0);
    return vec4(clamp(colour, 0.0, 1.0), mark);
}

// --- the transitions ----------------------------------------------------------------------------
//
// How one combination becomes the next. There are twenty-four, the arriving seed picks which, and
// every one of them is the same shape: given how far through the change we are, say where the
// OUTGOING picture is read and how much of it is left, and the same for the one ARRIVING. Warping the
// two sample points rather than fading two finished pictures is what lets a change be a movement
// instead of a mix — a slide, a fall, a turn, a squeeze through a line.
//
// A press is answered by whichever of them the new seed names, so pressing the mark twice is two
// different changes as well as two different marks.
const uint kLogoTransitions = 24u;

struct LogoFade {
    vec2 out_uv;
    float out_show;
    vec2 in_uv;
    float in_show;
};

// Where a cell of the SCREEN is, for the transitions that break the picture into voxels. Cells of the
// screen and not of the drawing, so what comes apart is the picture in front of you.
float logo_fade_cell(vec2 local, float cells, out vec2 cell) {
    cell = floor(local * cells);
    return logo_cell_random(cell + 53.0);
}

LogoFade logo_transition(uint which, uint seed, vec2 local, float m) {
    LogoFade fade;
    fade.out_uv = local;
    fade.in_uv = local;
    fade.out_show = 1.0 - m;
    fade.in_show = m;

    float turn = logo_unit(seed, 92u) * 6.2831853;
    vec2 direction = vec2(cos(turn), sin(turn));
    vec2 q = local - 0.5;
    vec2 cell;

    if (which == 0u) {
        // Voxels: the outgoing picture scatters outward and the arriving one gathers out of the same
        // scatter, each cell on its own bearing.
        float own = logo_fade_cell(local, 20.0, cell);
        vec2 away = vec2(cos(own * 6.2831853), sin(own * 6.2831853)) * (0.4 + own * 0.9) * 0.5;
        fade.out_uv = local + away * m * m;
        fade.in_uv = local + away * (1.0 - m) * (1.0 - m);
    } else if (which == 1u) {
        // A plain crossfade, and it is here because sometimes nothing should happen but the change.
    } else if (which == 2u) {
        // A front sweeping across at the seed's own angle: ahead of it the old picture, behind it the
        // new one, and nothing anywhere is half of both.
        float along = dot(q, direction) + 0.5;
        float front = m * 1.4 - 0.2;
        fade.out_show = smoothstep(0.0, 0.04, along - front);
        fade.in_show = 1.0 - fade.out_show;
    } else if (which == 3u) {
        float r = length(q * vec2(1.0, 1.1));
        float front = m * 0.9;
        fade.in_show = smoothstep(0.03, 0.0, r - front);
        fade.out_show = 1.0 - fade.in_show;
    } else if (which == 4u) {
        // Row by row, each on its own delay, so the change runs down the mark rather than over it.
        float row = floor(local.y * 24.0);
        float own = logo_cell_random(vec2(3.0, row));
        float when = clamp(m * 2.0 - own, 0.0, 1.0);
        fade.in_show = when;
        fade.out_show = 1.0 - when;
        fade.out_uv.x += (1.0 - fade.out_show) * 0.4 * ((mod(row, 2.0) < 1.0) ? -1.0 : 1.0);
        fade.in_uv.x -= fade.in_show * 0.0;
    } else if (which == 5u) {
        float column = floor(local.x * 24.0);
        float own = logo_cell_random(vec2(column, 9.0));
        float when = clamp(m * 2.0 - own, 0.0, 1.0);
        fade.in_show = when;
        fade.out_show = 1.0 - when;
        fade.out_uv.y += (1.0 - fade.out_show) * 0.4 * ((mod(column, 2.0) < 1.0) ? -1.0 : 1.0);
    } else if (which == 6u) {
        // Checked: every cell changes over at its own moment, so the two pictures interleave.
        float own = logo_fade_cell(local, 14.0, cell);
        float when = step(own, m);
        fade.in_show = when;
        fade.out_show = 1.0 - when;
    } else if (which == 7u) {
        // A slide: one goes, the other comes from the other side, both at full strength the whole
        // way, because a slide that fades is a slide nobody believes.
        fade.out_uv = local + direction * m * 1.3;
        fade.in_uv = local - direction * (1.0 - m) * 1.3;
        fade.out_show = 1.0;
        fade.in_show = 1.0;
    } else if (which == 8u) {
        fade.out_uv.y = local.y - m * 1.2;         // the old one lifts away
        fade.in_uv.y = local.y + (1.0 - m) * 1.2;  // and the new one rises into its place
        fade.out_show = 1.0;
        fade.in_show = 1.0;
    } else if (which == 9u) {
        // Through: the old picture grows past the edges and the new one comes up out of nothing.
        fade.out_uv = 0.5 + q / max(1.0 - m * 0.9, 0.05);
        fade.in_uv = 0.5 + q / max(0.05 + m * 0.95, 0.05);
    } else if (which == 10u) {
        fade.out_uv = 0.5 + q / max(0.05 + (1.0 - m) * 0.95, 0.05);
        fade.in_uv = 0.5 + q / max(1.0 - (1.0 - m) * 0.9, 0.05);
    } else if (which == 11u) {
        // Both turn about the middle, one out of the frame and one into it.
        fade.out_uv = 0.5 + logo_turn(q, m * 2.4) / max(1.0 - m * 0.7, 0.1);
        fade.in_uv = 0.5 + logo_turn(q, (1.0 - m) * -2.4) / max(1.0 - (1.0 - m) * 0.7, 0.1);
    } else if (which == 12u) {
        // A card turning: the old face for the first half, the new one for the second, and the
        // compression is the projection rather than a squash.
        float spin = m * 3.1415927;
        float squeeze = max(abs(cos(spin)), 0.06);
        fade.out_uv = vec2(0.5 + q.x / squeeze, local.y);
        fade.in_uv = vec2(0.5 - q.x / squeeze, local.y);
        fade.out_show = (m < 0.5) ? 1.0 : 0.0;
        fade.in_show = 1.0 - fade.out_show;
    } else if (which == 13u) {
        // The old one falls apart downward under something like gravity; the new one is thrown up
        // into place.
        float own = logo_fade_cell(local, 18.0, cell);
        fade.out_uv.y = local.y - (m * m * (1.2 + own * 0.8));
        fade.out_uv.x = local.x - (own - 0.5) * m * 0.3;
        fade.in_uv.y = local.y + (1.0 - m) * (1.0 - m) * (0.9 + own * 0.6);
        fade.out_show = 1.0;
        fade.in_show = 1.0;
    } else if (which == 14u) {
        // Rain: both fall, one out of the bottom and one in at the top, each column on its own clock.
        float own = logo_cell_random(vec2(floor(local.x * 20.0), 5.0));
        fade.out_uv.y = local.y - m * (1.1 + own * 0.7);
        fade.in_uv.y = local.y + (1.0 - m) * (1.1 + own * 0.7);
        fade.out_show = 1.0;
        fade.in_show = 1.0;
    } else if (which == 15u) {
        // A ripple carries the change out from the middle: the front is a wave rather than an edge,
        // and both pictures are displaced by it as it passes.
        float r = length(q);
        float front = m * 0.95;
        float wave = exp(-abs(r - front) * 18.0);
        vec2 push = normalize(q + vec2(1e-5)) * wave * 0.1;
        fade.out_uv = local + push;
        fade.in_uv = local - push;
        fade.in_show = smoothstep(0.02, 0.0, r - front);
        fade.out_show = 1.0 - fade.in_show;
    } else if (which == 16u) {
        // The old one melts down and the new one gathers up out of the puddle.
        float run = m * m;
        fade.out_uv.y = local.y - run * (0.3 + 0.7 * logo_noise(vec2(local.x * 11.0, 0.0)));
        fade.in_uv.y = local.y + (1.0 - m) * (1.0 - m) * 0.5;
    } else if (which == 17u) {
        // Squeezed to a line and out of one. The line is where the mark's middle is, so the change
        // has a place rather than only a moment.
        fade.out_uv = vec2(local.x, 0.5 + q.y / max(1.0 - m, 0.02));
        fade.in_uv = vec2(local.x, 0.5 + q.y / max(m, 0.02));
        fade.out_show = 1.0;
        fade.in_show = 1.0;
    } else if (which == 18u) {
        fade.out_uv = vec2(0.5 + q.x / max(1.0 - m, 0.02), local.y);
        fade.in_uv = vec2(0.5 + q.x / max(m, 0.02), local.y);
        fade.out_show = 1.0;
        fade.in_show = 1.0;
    } else if (which == 19u) {
        // Scrambled out and unscrambled in, in slices: the same idea the sort is built on, used as a
        // change rather than as a state.
        float lanes = 26.0;
        float scaled = clamp(local.y, 0.0, 0.99999) * lanes;
        float lane = floor(scaled);
        float within = scaled - lane;
        float jumbled = mod(lane * 7.0 + 3.0, lanes);
        fade.out_uv.y = mix(local.y, (jumbled + within) / lanes, m);
        fade.in_uv.y = mix(local.y, (jumbled + within) / lanes, 1.0 - m);
    } else if (which == 20u) {
        // To the corners and in from them.
        vec2 corner = sign(q + vec2(1e-6));
        float own = logo_fade_cell(local, 16.0, cell);
        fade.out_uv = local - corner * m * m * (0.5 + own * 0.5);
        fade.in_uv = local - corner * (1.0 - m) * (1.0 - m) * (0.5 + own * 0.5);
    } else if (which == 21u) {
        // A cloud front: noise thresholded, so the boundary is ragged and travels.
        float cloud = logo_noise(local * 7.0 + vec2(11.0, 3.0));
        float when = smoothstep(cloud - 0.12, cloud + 0.12, m * 1.3 - 0.15);
        fade.in_show = when;
        fade.out_show = 1.0 - when;
    } else if (which == 22u) {
        // A vortex: the old one wound out, the new one wound in, hardest at the middle.
        float r = length(q);
        float wind = 5.0 * exp(-r * 2.4);
        fade.out_uv = 0.5 + logo_turn(q, wind * m);
        fade.in_uv = 0.5 + logo_turn(q, -wind * (1.0 - m));
    } else if (which == 23u) {
        // Slices jumping sideways, a few at a time, the way a broken signal does. The one transition
        // here that is meant to look like a fault, because a mark that is allowed to be a picture is
        // allowed to be a picture of interference.
        float row = floor(local.y * 30.0);
        float own = logo_cell_random(vec2(row, floor(m * 12.0) + 1.0));
        float jump = (own - 0.5) * 0.5 * (1.0 - abs(m * 2.0 - 1.0));
        fade.out_uv.x = local.x + jump;
        fade.in_uv.x = local.x - jump;
        float when = step(own, m * 1.2);
        fade.in_show = when;
        fade.out_show = 1.0 - when;
    }
    return fade;
}

// The mark. `local` is 0 to 1 across the square the DRAWING occupies -- outside that range on every
// side, because the mark is not in a box (see `kLogoReach`). `seed` is the combination now,
// `previous` the one before it, `morph` how far through the change between them (1 when there is no
// change in progress, which is almost always), and `base` where the host put what it worked out for
// the two of them.
vec4 logo_draw(vec2 local, uint seed, uint previous, float age, float t, float morph, vec3 bias,
               uint base) {
    float m = clamp(morph, 0.0, 1.0);
    LogoFrame frame = logo_frame(base);
    if (m >= 0.999 || previous == seed) return logo_one(seed, local, age, t, bias, frame);

    LogoFade fade = logo_transition(logo_pick(seed, 91u, kLogoTransitions), seed, local, m);
    vec4 now = logo_one(seed, fade.in_uv, age, t, bias, frame);
    now.a *= clamp(fade.in_show, 0.0, 1.0);
    vec4 was = logo_one(previous, fade.out_uv, age + kLogoSettled, t, bias,
                        logo_frame(base + kLogoFrameWords));
    was.a *= clamp(fade.out_show, 0.0, 1.0);

    // Premultiplied, and that is the whole of why this is four lines rather than one `mix`. Where
    // only one of the two has a mark, mixing the COLOURS averages it against a black that is not
    // there, and the change reads as the mark going dark in the middle of itself.
    float total = was.a + now.a;
    if (total <= 0.0001) return vec4(0.0);
    return vec4((was.rgb * was.a + now.rgb * now.a) / total, min(total, 1.0));
}
