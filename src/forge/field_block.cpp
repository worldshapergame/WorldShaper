// Evaluating one expression at MANY points in a single traversal.
//
// # Why this file exists, in one measurement
//
// `Field::eval` answers one point, and D722 counted what that costs on the estate: **632 field-node
// visits at 15.5 nanoseconds a visit**, about ten microseconds an answer. A node of the render tree
// is eight voxels a side, and the sampler's descent asks about **640 points inside a box a quarter
// of a metre across** to fill it. Every one of those 640 walks visits the same nodes, tests the same
// boxes and takes the same turns, because every one of them is about a point inside the same small
// box.
//
// So the traversal is paid 640 times for one shape's worth of information. This walks it once and
// carries all 640 points down it.
//
// # The one promise
//
// **The same answer as `Field::eval`, bit for bit, for every point.** Not nearly, not within an
// epsilon: the sampler settles boxes on these numbers and every world in this repository is gated on
// a content hash, so a last-bit difference is a different world. `tests/test_field_block.cpp` is
// that promise written down, and `clips/sampler.clip` building `d0d5f84c685be847` at 1,430,104 solid
// voxels is it measured end to end.
//
// That promise is what decides the shape of everything here. Arithmetic is done in the same order
// and the same precision as `eval` does it; where `eval` culls a child on its box, this culls the
// same child on the same box for the whole block, which cannot change an answer because a culled
// child was never going to win the minimum anyway.
//
// # Where the saving actually is
//
// Not in vector instructions, or not only. The three things paid once per BLOCK instead of once per
// point are the pointer chase into `nodes_`, the switch on the op, and the box test on each union
// child — and D722 says those are the cost, because 15.5 ns a visit for a handful of flops is a
// memory-bound walk rather than an arithmetic-bound one.

#include "forge/field.hpp"

#include <cmath>
#include <vector>

namespace ws {
namespace forge {

// THE STUB, and it is deliberately correct before it is fast.
//
// One `eval` per point answers the promise above by construction — it IS `eval` — so everything
// downstream of this file can be written, gated and measured against a real API while the traversal
// underneath it is replaced. The replacement is then a change with a control arm rather than a new
// path with nothing to compare against, which is the only way a bit-for-bit claim can be checked.
void Field::eval_block(u32 root, const Vec3* points, usize count, f64* out) const {
    for (usize i = 0; i < count; ++i) out[i] = eval(root, points[i]);
}

}  // namespace forge
}  // namespace ws
