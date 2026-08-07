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

vec3 draw_one_box(vec3 colour, vec3 origin, vec3 dir, float depth, vec3 lo, vec3 hi, int state,
                  float wash) {
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
            if (edge_distance(point, lo, hi, a) < width) {
                bool hidden = t > depth;
                colour = mix(colour, hidden ? tint_hidden : tint, hidden ? 0.55 : 0.9);
            }
        }
    }

    // A wash over the volume, so a box drawn in open air is visible as a volume and not
    // only as its outline. Deliberately slight: it sits over whatever the player is trying
    // to line the edit up against.
    Slab box = ray_box(origin, dir, lo, hi);
    if (box.far_t >= max(box.near_t, 0.0) && max(box.near_t, 0.0) < depth) {
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

    for (int b = 0; b < 16; ++b) {
        int state = push.box_min[b].w;
        if (state == 0) continue;
        // Voxel coordinates name the corner, so a box from min to max covers max+1
        // exclusive.
        vec3 lo = vec3(push.box_min[b].xyz);
        vec3 hi = vec3(push.box_max[b].xyz) + vec3(1.0);

        if (state == 5 && push.clip_slot[b].y != 0u) {
            ++g_ghost_boxes;
            // The nearest ghost wins, and one already closer stops the rest being marched.
            GhostHit found = march_ghost(origin, dir, lo, push.clip_slot[b],
                                         push.clip_coarse[b], best.t, budget);
            if (found.hit) best = found;
            // Only the copy being steered is outlined; the rest are their own voxels. See
            // the note where these boxes are filled in â€” six plane tests a pixel, sixteen
            // times over, was most of what put this pass over its budget.
            if ((push.box_max[b].w & 0xFF) != 0) {
                colour = draw_one_box(colour, origin, dir, depth, lo, hi, 2, 0.02);
            }
            ++drawn;
            continue;
        }

        // Sixteen ghosts each washing the frame at full strength would fog the world out,
        // so the wash thins as the row gets longer while the outlines stay crisp.
        float wash = 0.07 / (1.0 + 0.35 * float(drawn));
        colour = draw_one_box(colour, origin, dir, depth, lo, hi, state, wash);

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
            if (all(greaterThan(inner_hi, inner_lo))) {
                colour = draw_one_box(colour, origin, dir, depth, inner_lo, inner_hi, state, 0.0);
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

    for (int m = 0; m < 8; ++m) {
        if (push.marks[m].w == 0) continue;
        vec3 mlo = vec3(push.marks[m].xyz);
        Slab mark = ray_box(origin, dir, mlo, mlo + vec3(1.0));
        float t = max(mark.near_t, 0.0);
        if (mark.far_t < t || t > depth) continue;
        colour = mix(colour, ink_over(colour), 0.75);
    }
    return colour;
}

