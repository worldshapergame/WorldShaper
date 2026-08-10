// The tool previews: the chisel's box, its constraint marks, and the clipboard's ghosts.
//
// Shared because a preview is part of the interface, not part of a renderer. The path tracer
// draws the same ones the real-time pipeline does - a tool you cannot see while looking at
// the thing you are about to edit is not a tool.
//
// Expects push and the clip buffer to be declared already; each shader binds those where
// it likes and includes this afterwards.
// ---------------------------------------------------------------------------------------
// The chisel preview.
//
// Drawn here rather than as geometry because it is not geometry: it is a description of an
// edit that has not happened. Intersecting the box analytically against the ray this pixel
// already has costs a handful of instructions and needs no vertex buffer, no depth pass and
// no second pipeline.
//
// Colour follows documentation/14-ui-style.md. The interface has no palette: the outline is
// the inverse of whatever is behind it, so it reads against sky, stone or snow without
// choosing a colour. The single exception is the one the style document already grants â€”
// a destructive decision is red â€” which is what tells carving apart from placing without
// adding a second hue to the game.

struct Slab { float near_t; float far_t; int near_axis; int far_axis; };

Slab ray_box(vec3 origin, vec3 dir, vec3 lo, vec3 hi) {
    Slab s;
    s.near_t = -1e30;
    s.far_t = 1e30;
    s.near_axis = 0;
    s.far_axis = 0;
    for (int a = 0; a < 3; ++a) {
        if (abs(dir[a]) < 1e-9) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) { s.near_t = 1.0; s.far_t = -1.0; }
            continue;
        }
        float inv = 1.0 / dir[a];
        float t0 = (lo[a] - origin[a]) * inv;
        float t1 = (hi[a] - origin[a]) * inv;
        if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
        if (t0 > s.near_t) { s.near_t = t0; s.near_axis = a; }
        if (t1 < s.far_t) { s.far_t = t1; s.far_axis = a; }
    }
    return s;
}

// How close to a box edge this point on a face is, measured in world units along the two
// axes that are not the face normal.
float edge_distance(vec3 point, vec3 lo, vec3 hi, int axis) {
    float best = 1e30;
    for (int a = 0; a < 3; ++a) {
        if (a == axis) continue;
        best = min(best, min(point[a] - lo[a], hi[a] - point[a]));
    }
    return max(best, 0.0);
}

vec3 ink_over(vec3 backdrop) {
    // The ink rule in miniature: invert, then push away from the backdrop's brightness so a
    // mid-grey behind it does not leave the line invisible. The full formula belongs to the
    // UI pass in Stage 15; an outline needs only that it never vanishes.
    vec3 inverted = vec3(1.0) - backdrop;
    float here = dot(backdrop, vec3(0.2126, 0.7152, 0.0722));
    float away = (here < 0.5) ? 1.0 : 0.0;
    return mix(inverted, vec3(away), 0.45);
}

// The colour the cursor marker and the constraint marks are drawn in: the material itself, not a
// decision about it. Falls back to the ink rule when nothing is in hand.
vec3 tool_ink(vec3 backdrop) {
    return (push.tool_colour.w > 0.5) ? push.tool_colour.xyz : ink_over(backdrop);
}

// Markers are bounded by the voxel they stand for, and are never grown to a legible size in pixels.
//
// That was tried and it is wrong. A marker with a floor in pixels stops being part of the world and
// becomes part of the interface: it hangs in front of geometry at a size that has nothing to do
// with anything, and at range it covers voxels it does not mean. What these say is "this voxel",
// and the honest way to say it is to be exactly that big and to go small when the voxel does.
//
// The only thing measured in pixels is LINE WIDTH, which is a different quantity: a line thinner
// than a sample is a line that flickers, and thickening it does not change what the shape encloses.
float line_width(float along, float pixels) {
    return 2.0 * push.lens.x / float(push.resolution.y) * along * pixels;
}

// Is a preview surface at `t` behind the world surface at `depth`?
//
// With a bias, because the interesting previews are drawn EXACTLY on world geometry and without one
// the comparison is a coin toss per pixel. A marker on the face of a solid voxel is at the same
// distance as that face to the last bit, so half the pixels of the ring decide "in front" and half
// decide "behind" and the shape breaks up into speckle that crawls as the camera moves — the
// classic z-fight, arrived at through a marcher rather than a depth buffer.
//
// Relative rather than absolute, because the error being covered is a relative one: it comes from
// two different pieces of arithmetic arriving at the same surface, and the disagreement scales with
// the distance. A thousandth is five millimetres at five metres, which is well inside a voxel and
// well outside the noise.
bool preview_behind(float t, float depth) { return t > depth * 1.001 + 1e-4; }

// A hollow shape on every face of one voxel: six of them, each lying in the plane of its own face.
// A ring for the cursor marker, a cross for a constraint point.
//
// On the faces rather than billboarded at the camera, because both of these are marks ON something.
// Painted onto the cube they belong to they are unambiguous about which voxel they mean and which
// way it is turned, and they behave like the surface they are on: they foreshorten, they go
// edge-on, and whichever faces you can see carry them. A billboard stays the same size and
// square-on from every angle, which reads as a screen overlay hovering near the voxel rather than
// as a mark on it -- and at range it covers voxels it does not mean.
//
// Hollow, so the surface being marked is still visible through the middle of its own marker. That
// is the whole job of the cursor: it says "this one" without hiding the thing you are lining an
// edit up against.
//
// Depth-tested, unlike the preview BOX. A box is deliberately drawn through geometry because
// carving happens inside rock and a box you cannot see the far side of is one you have to guess the
// size of. A mark on a single voxel is the opposite case: it is telling you what you are pointing
// at, and one that shines through a wall is pointing at something you cannot reach.
vec3 draw_face_marks(vec3 colour, vec3 origin, vec3 dir, float depth, vec3 lo, vec3 hi, vec3 tint,
                     bool cross_shape) {
    vec3 centre = (lo + hi) * 0.5;
    vec3 half_size = (hi - lo) * 0.5;

    // Reject the whole cell before testing six faces of it.
    //
    // Eight marks at six faces each is forty-eight plane tests a pixel, and all but a handful of
    // pixels on the screen are nowhere near any of them. This is the ray's closest approach to the
    // centre against the cell's circumradius: a dozen instructions that answer "not this one" for
    // almost every pixel, and it cannot reject a cell the ray does cross.
    vec3 to_centre = centre - origin;
    float along = dot(to_centre, dir);
    float bound = length(half_size);
    if (along < -bound || dot(to_centre, to_centre) - along * along > bound * bound) return colour;

    for (int a = 0; a < 3; ++a) {
        // Only faces you are actually looking at.
        //
        // A face's normal is its own axis, so this dot product is just the ray's component along
        // it. Near zero the face is edge-on, and a circle or a cross drawn on a face seen edge-on
        // flattens into a short line lying along the voxel's silhouette — three of those and the
        // marker gains a partial box around it, which reads as an outline nobody asked for and
        // hides which face is actually being marked.
        //
        // A fifth keeps the two or three faces a corner view shows (a cube diagonal gives every
        // axis 0.577) and drops the ones that have degenerated. It also costs less, which is
        // incidental: the point is that a shape too foreshortened to be a shape is not one.
        if (abs(dir[a]) < 0.2) continue;
        float inv = 1.0 / dir[a];
        for (int side = 0; side < 2; ++side) {
            float t = ((side == 0 ? lo[a] : hi[a]) - origin[a]) * inv;
            // Behind the eye, or behind the world. Biased, because a mark drawn on the face of a
            // solid voxel is at exactly the distance of that face — see preview_behind.
            if (t < 0.0 || preview_behind(t, depth)) continue;
            vec3 point = origin + dir * t;

            // The two axes that are not the face normal, which are the face's own u and v.
            int au = (a + 1) % 3;
            int av = (a + 2) % 3;
            float u = point[au] - centre[au];
            float v = point[av] - centre[av];
            if (abs(u) > half_size[au] || abs(v) > half_size[av]) continue;

            bool on_shape;
            if (cross_shape) {
                // Rotated into the diagonals, so the two arms are one axis test each rather than a
                // pair of line-segment tests. Corner to corner: the half-DIAGONAL of the face.
                float arm_a = (u + v) * 0.70710678;
                float arm_b = (u - v) * 0.70710678;
                float reach = length(vec2(half_size[au], half_size[av]));
                float width = max(line_width(t, 1.0), reach * 0.10);
                on_shape = (abs(arm_a) < width && abs(arm_b) < reach) ||
                           (abs(arm_b) < width && abs(arm_a) < reach);
            } else {
                // The circle inscribed in the face, and inscribed means the OUTSIDE of the line
                // touches the edge — not the centre of it. Setting the radius to the half-width
                // puts half the line's thickness past the face, so the ring reads as slightly
                // larger than the voxel it is marking and overhangs whatever is next to it.
                float width = max(line_width(t, 1.0), min(half_size[au], half_size[av]) * 0.14);
                float ring = max(min(half_size[au], half_size[av]) - width, width);
                on_shape = abs(length(vec2(u, v)) - ring) < width;
            }
            if (!on_shape) continue;

            colour = mix(colour, tint, 0.95);
        }
    }
    return colour;
}

vec3 draw_one_box(vec3 colour, vec3 origin, vec3 dir, float depth, vec3 lo, vec3 hi, int state,
                  float wash, float face_fill) {
    // A constant line width in pixels: the world width a pixel covers grows with distance.
    float per_pixel = 2.0 * push.lens.x / float(push.resolution.y);

    // Refusal keeps the one colour the style document grants outright (documentation/14
    // Â§"The five permitted colours"): a decision that will not go through is red. The other
    // states take their colour from the material, or from the backdrop.
    vec3 tint = (push.tint_visible.w > 0.5) ? push.tint_visible.xyz : ink_over(colour);
    vec3 tint_hidden = (push.tint_occluded.w > 0.5) ? push.tint_occluded.xyz : ink_over(colour);
    if (state == 3) {
        tint = vec3(0.85, 0.16, 0.16);
        tint_hidden = tint;
    }

    // All six faces, not just the two the ray enters and leaves through. Those two give
    // eight of the twelve edges, and the four they miss are exactly the ones running along
    // the view direction â€” which, looking down into a box from above, is its entire
    // silhouette. The box then reads as a solid pale slab with a rectangle drawn on top of
    // it rather than as a wireframe.
    for (int a = 0; a < 3; ++a) {
        if (abs(dir[a]) < 1e-9) continue;
        float inv = 1.0 / dir[a];
        for (int side = 0; side < 2; ++side) {
            float t = ((side == 0 ? lo[a] : hi[a]) - origin[a]) * inv;
            if (t < 0.0) continue;
            vec3 point = origin + dir * t;
            bool on_face = true;
            for (int b = 0; b < 3; ++b) {
                if (b == a) continue;
                on_face = on_face && point[b] >= lo[b] && point[b] <= hi[b];
            }
            if (!on_face) continue;
            float width = max(per_pixel * t * 1.2, 0.06);
            // Drawn through geometry, dimmed where something is in front of it. Carving
            // happens *inside* rock, so an outline that respected depth would be invisible
            // in exactly the case the tool exists for; and a shape you cannot see the far
            // side of is one you have to guess the size of.
            // Biased, because a preview box lined up against a surface — which is most of what a
            // player does with one — has a face at exactly that surface's distance, and an
            // unbiased test speckles it. See preview_behind.
            bool hidden = preview_behind(t, depth);
            if (edge_distance(point, lo, hi, a) < width) {
                colour = mix(colour, hidden ? tint_hidden : tint, hidden ? 0.55 : 0.9);
            } else if (face_fill > 0.0) {
                // The face itself, in the same two colours the outline uses and by the same
                // rule. An outline says where the box is; the faces say which way it is
                // turned, and a wireframe seen straight on is ambiguous about that in a way
                // that has cost a misplaced edit more than once.
                //
                // Every face the ray crosses, near and far, so the tint deepens where the box
                // is thick. That is a depth cue for free and it is what stops six planes at
                // one opacity reading as a flat card.
                colour = mix(colour, hidden ? tint_hidden : tint,
                             hidden ? face_fill * 0.6 : face_fill);
            }
        }
    }

    // A wash over the volume, so a box drawn in open air is visible as a volume and not
    // only as its outline. Deliberately slight: it sits over whatever the player is trying
    // to line the edit up against.
    Slab box = ray_box(origin, dir, lo, hi);
    if (box.far_t >= max(box.near_t, 0.0) && !preview_behind(max(box.near_t, 0.0), depth)) {
        colour = mix(colour, tint, (state == 3) ? 0.20 : wash);
    }
    return colour;
}

// ---------------------------------------------------------------------------------------
// The clipboard's ghost.
//
// A paste preview has to show the voxels that are about to land, not the box they will land
// in â€” a box tells you nothing about whether the arch lines up with the wall. So the ghost
// is marched: the ray steps through the held clip and takes the first cell with matter in
// it, then that voxel is shaded exactly like a world voxel and blended at partial opacity.
//
// It is marched here rather than in the visibility pass because a ghost is not part of the
// world. Nothing in the world should see it, nothing should stream because of it, and when
// it is put down it becomes ordinary voxels that the marcher finds the ordinary way.

// Step budgets for the ghost march.
//
// Per ghost, *not* shared across them: a single shared budget was a bug, and a visible one.
// The first ghost the loop reached would spend the lot walking through its own empty space,
// and every ghost behind it drew nothing â€” so copies vanished exactly when they lined up
// with each other, which is when a row of them is most worth looking at.
//
// The total is still capped so one pathological view cannot run away. When it runs out the
// far copies stop being drawn, which is the right thing to lose first.
//
// Measured with sixteen copies overlapping and filling the frame: 0.24 ms at 1280x800
// against this pass's 0.80 ms budget, 0.82 ms at 2560x1440 â€” which is 3.6 times the pixels
// the budget was written for, so it is the first number that answers the budget.
const int kGhostStepsEach = 64;
const int kGhostStepsTotal = 320;

struct GhostHit { bool hit; float t; uint type; int face; };

// Debug mode 4 reads these: red where a ghost box was tested, green where the march found
// matter, blue where the clip buffer says it holds nothing to march.
int g_ghost_boxes = 0;
int g_ghost_entered = 0;
bool g_ghost_hit = false;

// Marches one instance of the held clip. `lo` is where its (0,0,0) cell sits; `slot` says
// where in the cell buffer that copy's shape lives and how big it is, and `coarse` says the
// same about its occupancy mask.
//
// A selection is mostly air. Without the mask, a ray entering a clip walks every empty
// voxel between the surface it entered through and the first thing worth drawing â€” so a
// ghost costs what its bounding box costs rather than what its surface costs, and the step
// budget runs out before reaching anything. With it, empty 8-blocks are jumped over whole,
// exactly the way the world marcher skips empty bricks.
GhostHit march_ghost(vec3 origin, vec3 dir, vec3 lo, uvec4 slot, uvec4 coarse, float limit,
                     inout int total) {
    GhostHit result;
    result.hit = false;
    result.t = limit;
    result.type = 0u;
    result.face = 0;

    ivec3 dims = ivec3(slot.yzw);
    ivec3 blocks = ivec3(coarse.yzw);
    vec3 hi = lo + vec3(dims);
    Slab box = ray_box(origin, dir, lo, hi);
    float enter = max(box.near_t, 0.0);
    float leave = min(box.far_t, limit);
    if (leave < enter) return result;
    ++g_ghost_entered;

    vec3 inv;
    ivec3 step_dir;
    vec3 delta;
    for (int a = 0; a < 3; ++a) {
        inv[a] = (abs(dir[a]) < 1e-9) ? 0.0 : (1.0 / dir[a]);
        step_dir[a] = (abs(dir[a]) < 1e-9) ? 0 : ((dir[a] > 0.0) ? 1 : -1);
        delta[a] = (step_dir[a] == 0) ? 1e30 : abs(inv[a]);
    }

    float t = enter;
    int face = box.near_axis * 2 + ((step_dir[box.near_axis] > 0) ? 0 : 1);
    int budget = kGhostStepsEach;

    // Seeded fresh after every skip, which is why the DDA state is built here rather than
    // carried: jumping to the far side of an empty block invalidates it.
    ivec3 cell = ivec3(0);
    vec3 next = vec3(1e30);
    bool seeded = false;

    while (budget > 0 && total > 0 && t <= leave) {
        --budget;
        --total;

        if (!seeded) {
            vec3 at = origin + dir * (t + 1e-4) - lo;
            cell = clamp(ivec3(floor(at)), ivec3(0), dims - ivec3(1));
            for (int a = 0; a < 3; ++a) {
                if (step_dir[a] == 0) {
                    next[a] = 1e30;
                } else {
                    float boundary = float(cell[a] + ((step_dir[a] > 0) ? 1 : 0));
                    next[a] = t + 1e-4 + (boundary - at[a]) * inv[a];
                }
            }
            seeded = true;
        }

        uint value = clip.items[slot.x + uint(cell.x) + uint(cell.y) * slot.y +
                                uint(cell.z) * slot.y * slot.z];
        // 0 is outside the clip, 1 is air inside it; both are see-through.
        if (value > 1u) {
            result.hit = true;
            result.t = t;
            result.type = value - 1u;
            result.face = face;
            return result;
        }

        // Nothing here. If the whole 8-block is empty, leave by its far face in one go.
        ivec3 block = cell >> 3;
        uint occupied = clip.items[coarse.x + uint(block.x) + uint(block.y) * coarse.y +
                                   uint(block.z) * coarse.y * coarse.z];
        if (occupied == 0u) {
            vec3 block_lo = lo + vec3(block << 3);
            float exit_t = 1e30;
            int exit_axis = 0;
            for (int a = 0; a < 3; ++a) {
                if (step_dir[a] == 0) continue;
                float plane = block_lo[a] + ((step_dir[a] > 0) ? 8.0 : 0.0);
                float candidate = (plane - origin[a]) * inv[a];
                if (candidate < exit_t) {
                    exit_t = candidate;
                    exit_axis = a;
                }
            }
            if (exit_t <= t) exit_t = t + 1e-3;   // never stall
            t = exit_t;
            face = exit_axis * 2 + ((step_dir[exit_axis] > 0) ? 0 : 1);
            seeded = false;
            continue;
        }

        int axis = 0;
        if (next[1] < next[axis]) axis = 1;
        if (next[2] < next[axis]) axis = 2;
        if (next[axis] > leave) break;
        t = next[axis];
        cell[axis] += step_dir[axis];
        if (cell[axis] < 0 || cell[axis] >= dims[axis]) break;
        next[axis] += delta[axis];
        face = axis * 2 + ((step_dir[axis] > 0) ? 0 : 1);
    }
    return result;
}

vec3 draw_preview(vec3 colour, vec3 origin, vec3 dir, float depth) {
    int drawn = 0;
    int budget = kGhostStepsTotal;
    GhostHit best;
    best.hit = false;
    best.t = depth;
    best.type = 0u;
    best.face = 0;

    // How strongly each box fills its faces, packed a byte at a time beside the outline flag and
    // the shell thickness. Per box rather than one setting for the frame: a clipboard selection
    // wants a quarter and the ghost it becomes wants none, and both can be on screen in one frame.
    for (int b = 0; b < 16; ++b) {
        int state = push.box_min[b].w;
        if (state == 0) continue;
        // Voxel coordinates name the corner, so a box from min to max covers max+1
        // exclusive.
        vec3 lo = vec3(push.box_min[b].xyz);
        vec3 hi = vec3(push.box_max[b].xyz) + vec3(1.0);
        float face_fill = float((push.box_max[b].w >> 16) & 0xFF) / 255.0;

        // The cursor marker: where the crosshair is pointing, on screen whatever tool is in hand
        // and whatever it is in the middle of. A hollow ring on each face of the voxel rather than
        // a filled cube, and the material's own colour rather than a decision about it. See
        // draw_face_marks.
        if (state == 6) {
            colour = draw_face_marks(colour, origin, dir, depth, lo, hi, tool_ink(colour), false);
            continue;
        }

        if (state == 5 && push.clip_slot[b].y != 0u) {
            ++g_ghost_boxes;
            // The nearest ghost wins, and one already closer stops the rest being marched.
            GhostHit found = march_ghost(origin, dir, lo, push.clip_slot[b],
                                         push.clip_coarse[b], best.t, budget);
            if (found.hit) best = found;
            // Only the copy being steered is outlined; the rest are their own voxels. See
            // the note where these boxes are filled in â€” six plane tests a pixel, sixteen
            // times over, was most of what put this pass over its budget.
            // A held clip shows its own voxels, so its bounding box is only an outline: filling
            // the faces as well would put a coloured pane in front of the very thing the ghost
            // exists to let you line up. face_fill is nought for these and this passes it on
            // rather than hard-coding it, so the two agree by construction.
            if ((push.box_max[b].w & 0xFF) != 0) {
                colour = draw_one_box(colour, origin, dir, depth, lo, hi, 2, 0.02, face_fill);
            }
            ++drawn;
            continue;
        }

        // Sixteen ghosts each washing the frame at full strength would fog the world out,
        // so the wash thins as the row gets longer while the outlines stay crisp.
        float wash = 0.07 / (1.0 + 0.35 * float(drawn));
        colour = draw_one_box(colour, origin, dir, depth, lo, hi, state, wash, face_fill);

        // The shell, drawn as a second outline set in by its thickness.
        //
        // Without it a hollow placement looks exactly like a solid one right up until you let
        // go, and the setting reads as doing nothing at all — which is precisely how it was
        // reported. The inner box is the void that will be left, so seeing it is seeing the
        // wall thickness.
        int shell = (push.box_max[b].w >> 8) & 0xFF;
        if (shell > 0) {
            vec3 inner_lo = lo + vec3(float(shell));
            vec3 inner_hi = hi - vec3(float(shell));
            // Only when there is an inside left to draw; below that the placement is solid
            // anyway and a second outline would be a lie.
            // Outline only. The shell is a line drawn *inside* a box whose faces are already
            // tinted, and filling it too would double the tint over the whole inner volume and
            // hide the very difference it exists to show.
            if (all(greaterThan(inner_hi, inner_lo))) {
                colour =
                    draw_one_box(colour, origin, dir, depth, inner_lo, inner_hi, state, 0.0, 0.0);
            }
        }
        ++drawn;
    }

    g_ghost_hit = best.hit;
    if (best.hit) {
        uint visual_id = types.items[best.type].x;
        vec3 ghost = unpack_rgb(visuals.items[visual_id].x);
        vec3 normal = vec3(0.0);
        if (best.face == 0) normal = vec3(-1, 0, 0);
        else if (best.face == 1) normal = vec3(1, 0, 0);
        else if (best.face == 2) normal = vec3(0, -1, 0);
        else if (best.face == 3) normal = vec3(0, 1, 0);
        else if (best.face == 4) normal = vec3(0, 0, -1);
        else normal = vec3(0, 0, 1);
        ghost *= 0.55 + 0.45 * clamp(dot(normal, normalize(vec3(0.4, 0.85, 0.3))), 0.0, 1.0);
        // Partial opacity is what makes it read as a proposal rather than as the world.
        // Stage 9's path tracer shades it the same way it shades everything else; only this
        // last blend stays.
        colour = mix(colour, ghost, 0.62);
    }

    // The constraint points, as crosses in the material's colour.
    //
    // They used to be solid cells washed in the inverse of the backdrop, which had two faults and
    // both were reported as the same complaint — that you could not tell what you had dropped. A
    // filled cell is indistinguishable from a one-voxel preview box, and an inverted backdrop is a
    // different colour on every surface it lands on, so eight of them across a wall did not read as
    // eight of anything. An X is a shape rather than a shade: it is the key that drops it, it is
    // nothing else the tool draws, and it leaves the middle of the cell visible so the surface
    // being marked can still be seen.
    //
    // Drawn last, over the boxes, because a constraint is what the box has to reach and the box
    // grows to meet it — so it must be legible against the box's own tint.
    for (int m = 0; m < 8; ++m) {
        if (push.marks[m].w == 0) continue;
        vec3 mlo = vec3(push.marks[m].xyz);
        colour = draw_face_marks(colour, origin, dir, depth, mlo, mlo + vec3(1.0),
                                 tool_ink(colour), true);
    }
    return colour;
}

