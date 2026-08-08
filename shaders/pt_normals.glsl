// The normal a surface is SHADED by, which is not the normal it is MADE of.
//
// A voxel face is axis-aligned. There are six of them, so a sphere built of voxels reflects in
// exactly six directions and a curved wall shades in six steps. That is not noise and no
// denoiser will touch it: it is the geometry, faithfully rendered. It is also why a polished
// sphere in this renderer stipples, and why every column, dome and baluster in the facility
// will read as a stack of blocks however good the light is.
//
// The fix is to shade with a normal derived from the shape the voxels are approximating rather
// than from the face that was hit. The information is there -- the occupancy of the voxels
// around the hit describes the surface far better than one face does -- and the standard way to
// get at it is a gradient of the occupancy over a small neighbourhood, which is a smooth field
// where the faces are a staircase.
//
// WHAT IS DONE HERE, and why it is a height field and not a gradient of occupancy.
//
// A three-dimensional gradient of occupancy is the obvious construction and it fails the one
// test that matters: it cannot tell a curve from a corner. At the top edge of a cornice, at a
// quoin, and where a wall meets a floor, the occupancy around the hit is two half-spaces at
// right angles, and its gradient points at forty-five degrees to both of them. Every arris in
// the world gets bevelled and every interior one gets a cove that is not there, and widening the
// filter far enough to smooth a sphere widens the damage in proportion.
//
// So the neighbourhood is not read as a blob. It is read in the frame of the face that was hit:
// two axes across the face, and HEIGHT along it. For each neighbouring column of voxels, how far
// above or below the hit's own face does that column's surface sit? The answer is a small
// integer, and the shading normal is the gradient of that height.
//
// That reframing is the whole of the curve-against-corner rule, because it makes the two cases
// differ in kind rather than in degree:
//
//   - A curve is a height field. Where a face is the one that was hit, its axis is the surface's
//     dominant one, so the surface rises or falls by at most one voxel per voxel step across it.
//     Every neighbouring column has a height and it is within reach.
//   - A corner is not. Past a cornice's edge the neighbouring column falls away by the whole
//     depth of the drop; at the foot of a wall it climbs by the whole height of the wall.
//     Neither is a height a slope of one could reach, and both are recognised by the same scan
//     that was looking for the height anyway, so the test costs nothing.
//
// A column that answers "not within reach" is read as LEVEL WITH THE HIT rather than dropped.
// That is what keeps an arris hard: on a cornice's top face the outward columns say nothing, so
// they contribute no slope, so the top shades flat right up to its edge and the vertical face
// below it does the same. The two meet at a knife edge, which is what the mason cut. It also
// makes the fallback automatic rather than a special case -- a neighbourhood that says nothing
// useful gives every height zero, and zero heights give back exactly vec3(face).
//
// Two guarantees this owes the rest of the renderer:
//
//   - It never returns a normal facing away from the surface the eye is looking at. Slopes are
//     clamped to one, so the answer is at worst 54.7 degrees off its own face. A normal that
//     dips below the surface is not a blocky picture, it is light through a wall: the shadow
//     ray's own offset would start underground.
//   - It varies continuously ACROSS a face, not merely from one face to the next. Heights are
//     whole voxels, so a slope built from a plain difference of them can only be one of five
//     values per axis and a sphere shades in twenty-five directions instead of six -- better,
//     and still a count. The heights are fitted rather than differenced, and the fit is read at
//     the point the ray actually landed on. See face_slope.
//
// What it costs: on clips/normal_test.clip at 760x460 with the quality level pinned, the path
// trace went from 3.11 ms to 3.32 ms on the frame filled by the sphere. About twenty occupancy
// probes a shaded pixel, nearly all of them in the brick the one before them was in.

// What a column can say. Both non-answers read as level with the hit, and they are told apart
// only for the sliver rule at the bottom of this file.
const int kNoColumn = 127;     // nothing within reach below it: a drop
const int kAboveReach = 126;   // still solid above the top of the reach: a wall

float height_of(int h) {
    return (h >= 100) ? 0.0 : float(h);
}

// One chunk and one brick, remembered between asks.
//
// Every sample this file takes lies within a couple of voxels of the hit, so nearly all of them
// land in the brick the sample before it did. A cold voxel lookup is five dependent fetches --
// grid cell, chunk record, brick mask, prefix rank, occupancy word -- and remembering turns all
// but the first into one. This runs for every shaded pixel, and twenty-odd samples of five
// dependent fetches each is not affordable where twenty of one is.
struct BrickMemo {
    ivec3 chunk;    // world chunk coordinate the record below belongs to
    uint record;    // kNoRecord when that chunk is not resident
    ivec3 brick;    // world brick coordinate the slot below describes
    uint slot;      // where its voxels live, when it has any
    bool present;   // whether the world has a brick there at all
    bool loaded;
};

// Shifts rather than world.glsl's floor_div, and they are not interchangeable in general: what
// is wanted for a negative world coordinate is the floor and not the truncation. A signed right
// shift is compiled to OpShiftRightArithmetic, which propagates the sign bit and so *is* the
// floor for a power of two. Three divisions per probe, twenty times a pixel, is not.
bool memo_solid(inout BrickMemo memo, ivec3 world_voxel) {
    ivec3 brick = world_voxel >> 3;
    if (!memo.loaded || brick != memo.brick) {
        ivec3 chunk = world_voxel >> 8;
        if (!memo.loaded || chunk != memo.chunk) {
            memo.chunk = chunk;
            // A chunk that is not resident reads as empty, which is safe rather than merely
            // convenient: empty makes a column say "nothing within reach", and a column that
            // says nothing contributes no slope. Unstreamed neighbours therefore hand back the
            // face normal, which is what the renderer had before this file existed.
            memo.record = find_record(chunk);
        }
        memo.brick = brick;
        memo.loaded = true;
        memo.present = false;
        if (memo.record != kNoRecord) {
            ivec3 cell = brick & 31;
            if (mask_bit(memo.record, 0u, cell)) {
                memo.slot = brick_slot(memo.record, cell);
                memo.present = true;
            }
        }
    }
    return memo.present && voxel_solid(memo.slot, world_voxel & 7);
}

// How far above the hit's own face this column's surface sits, in voxels.
//
// Walked outward from the hit's own height rather than inward from the top of the reach, which
// is worth doing rather than merely tidy: most of the world is flat, a flat column answers in
// two probes this way against four the other, and the probes are what this file costs. Counted
// over the eight columns and the two checks on the hit itself, a flat pixel went from thirty
// probes to eighteen and the worst case from forty-two to thirty.
//
// The price of walking outward is that a column with something floating over it reports its own
// surface rather than the thing on top. That is the better answer anyway -- the floor under a
// lintel is still a floor -- and in the case that matters the two agree, because a neighbour
// whose real surface is level with the hit reads as level either way.
int column_height(inout BrickMemo memo, ivec3 base, ivec3 out_dir, int reach) {
    if (memo_solid(memo, base)) {
        // Climb while there is more above. Running out of climb is a wall, not a slope.
        for (int k = 1; k <= reach + 1; ++k) {
            if (!memo_solid(memo, base + out_dir * k)) return k - 1;
        }
        return kAboveReach;
    }
    for (int k = -1; k >= -reach; --k) {
        if (memo_solid(memo, base + out_dir * k)) return k;
    }
    return kNoColumn;
}

// The slope along one axis of the face, from four column heights and where in its own voxel the
// ray landed.
//
// A difference of neighbouring heights is the obvious thing and it is far too coarse. Heights
// are whole voxels, so the two columns either side can only ever say -1, -1/2, 0, 1/2 or 1: a
// sphere shades in twenty-five directions instead of six, which is better and still visibly a
// count, and a plane lying at a shallow angle to the grid alternates between flat and
// forty-five degrees along its length and reads as corduroy.
//
// So the four heights are least-squares fitted with a quadratic instead, over the same four
// samples. Two things fall out of that and both are needed:
//
//   - The slope term averages the columns with weights -2, -1, 1, 2 over ten, so it lands on
//     tenths of a voxel per voxel rather than halves. Worked through on a wall at eighteen
//     degrees to the grid -- a staircase of one voxel in three -- it holds the correct mean of
//     0.325 and ranges over 0.23 to 0.40, where the plain difference ranges over the whole of
//     0 to 1. That is a ripple of five degrees against twenty-two.
//   - The curvature term is what makes the answer vary ACROSS a face rather than only from one
//     face to the next. Read at the point the ray landed on, the fit agrees exactly with the
//     neighbouring voxel's own fit at their shared boundary whenever the surface is locally
//     quadratic -- which a sphere, a dome and a column all are -- so the field has no seams in
//     it and a curve shades as a curve rather than as a fine mosaic.
//
// A constant slope comes back exactly, which is the property that makes the fit safe to use: it
// smooths the staircase without flattening the ramp.
float face_slope(int m2, int m1, int p1, int p2, float where) {
    // Columns that said nothing read as level with the hit rather than being dropped. Dropping
    // them and renormalising would let the remaining side extrapolate out over the edge, which
    // is the arris being rounded off under another name.
    float a = height_of(m2);
    float b = height_of(m1);
    float c = height_of(p1);
    float d = height_of(p2);

    // The two orthogonal contrasts over k = -2..2 with the centre height zero by definition:
    // (-2,-1,0,1,2) for the slope and (2,-1,-2,-1,2) for the curvature, each over its own sum
    // of squares.
    float slope = (2.0 * (d - a) + (c - b)) * 0.1;
    float curve = (2.0 * (d + a) - (b + c)) * (1.0 / 14.0);

    // Clamped last, because a slope above one means the face that was hit is not the surface's
    // dominant axis. That case is worth more than a clamp and is handled below; the clamp is
    // what guarantees the answer never leans more than 54.7 degrees off its own face.
    return clamp(slope + 2.0 * curve * (where - 0.5), -1.0, 1.0);
}

// Whether this axis says the face that was hit is a sliver up the side of a ramp, and which way
// the surface falls away if it is.
//
// This is the one case the height field cannot answer in its own frame, and on a curve it is not
// a rare corner case -- it is every terrace edge. A voxel sphere is a stack of terraces, and the
// riser between two of them is one voxel wide and faces sideways: its own frame sees one
// neighbour climbing out of reach and the other with nothing under it at all, reports flat, and
// shades the riser ninety degrees away from the surface it belongs to. Those risers are the
// rings on the sphere.
//
// One column out of reach ABOVE and the other with nothing below it is the signature, and it is
// specific: a genuine arris always has a good column on one side -- the cornice's own top, the
// floor at the foot of the wall -- because a corner is where two surfaces meet and one of them
// is the one being shaded. Both sides failing, in opposite directions, means neither is, and
// what the pixel is standing on is the side of a step too steep for this frame to describe.
int sliver_direction(int m1, int p1) {
    if (p1 == kNoColumn && m1 == kAboveReach) return 1;    // the surface falls away toward +u
    if (m1 == kNoColumn && p1 == kAboveReach) return -1;
    return 0;
}

vec3 shading_normal(ivec3 voxel, ivec3 face, vec3 point, int level) {
    // Above a brick the marcher is drawing whole blocks and there is no per-voxel occupancy
    // under them to ask about; the block's own face is the honest answer. Levels 0 to 2 all
    // march at single voxels (see world.glsl), so they all get a real normal even when the pixel
    // is too small to resolve one voxel -- which is exactly where a curve most needs it.
    if (level >= 3) return vec3(face);

    BrickMemo memo;
    memo.chunk = ivec3(0);
    memo.record = kNoRecord;
    memo.brick = ivec3(0);
    memo.slot = 0u;
    memo.present = false;
    memo.loaded = false;

    // The hit has to be a face before it can be a height field: this voxel solid and the one in
    // front of it empty. At a grazing angle the point can round to the far side of the boundary,
    // and a height field measured from the wrong voxel is off by one everywhere -- a whole voxel
    // of tilt across a surface that is flat. Two probes to refuse the job is cheaper than that.
    if (!memo_solid(memo, voxel) || memo_solid(memo, voxel + face)) return vec3(face);

    // The hit voxel's own corner, in the same frame `point` is in, so that a sub-voxel position
    // can be taken along any axis -- including the one the hit face is normal to, which fract()
    // of the point would land on a boundary and get wrong by a whole voxel.
    vec3 corner = floor(point - vec3(face) * 0.5);

    ivec3 use = face;
    vec3 answer = vec3(face);
    vec3 own_frame = vec3(face);

    // Twice at most, and a loop rather than a second copy of the body. The second pass is the
    // sliver above: the face that was hit cannot describe the surface, and a perpendicular face
    // of the same voxel -- a real face, exposed, not an invented direction -- can.
    for (int pass = 0; pass < 2; ++pass) {
        int axis = (use.x != 0) ? 0 : ((use.y != 0) ? 1 : 2);
        int iu = (axis + 1) % 3;
        int iv = (axis + 2) % 3;
        ivec3 su = ivec3(0); su[iu] = 1;
        ivec3 sv = ivec3(0); sv[iv] = 1;

        // The near columns are scanned one voxel deep and the far ones two. The near ones cannot
        // need more: a surface whose dominant axis is this face rises at most one voxel per
        // step, so a near column out of reach at one voxel is a corner however far the scan
        // goes, and every voxel of extra reach is another probe on every column.
        int um1 = column_height(memo, voxel - su, use, 1);
        int up1 = column_height(memo, voxel + su, use, 1);
        int vm1 = column_height(memo, voxel - sv, use, 1);
        int vp1 = column_height(memo, voxel + sv, use, 1);

        float du = face_slope(column_height(memo, voxel - su * 2, use, 2), um1, up1,
                              column_height(memo, voxel + su * 2, use, 2),
                              clamp(point[iu] - corner[iu], 0.0, 1.0));
        float dv = face_slope(column_height(memo, voxel - sv * 2, use, 2), vm1, vp1,
                              column_height(memo, voxel + sv * 2, use, 2),
                              clamp(point[iv] - corner[iv], 0.0, 1.0));

        // The normal of a height field: minus its slopes across the face, and one along it.
        vec3 fitted = normalize(vec3(use) - du * vec3(su) - dv * vec3(sv));

        if (pass == 0) {
            own_frame = fitted;
            answer = fitted;
        } else {
            // A frame borrowed from a perpendicular face can lean past the face the eye is
            // actually looking at, and a normal on the far side of the surface from the viewer
            // is worse than a blocky one. When it does, the first answer stands.
            answer = (dot(fitted, vec3(face)) > 0.0) ? fitted : own_frame;
            break;
        }

        int su_dir = sliver_direction(um1, up1);
        int sv_dir = sliver_direction(vm1, vp1);
        // Neither axis, or both of them: both is a spike or a one-voxel bridge, where there is no
        // perpendicular face that describes the surface any better than this one does.
        if ((su_dir != 0) == (sv_dir != 0)) break;

        ivec3 next = (su_dir != 0) ? su * su_dir : sv * sv_dir;
        if (memo_solid(memo, voxel + next)) break;   // not an exposed face; keep what we have
        use = next;
    }

    return answer;
}
