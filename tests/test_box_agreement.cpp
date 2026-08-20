// Does a box settled in bulk agree with its own cells?
//
// The descent in `src/forge/sample.cpp` has one correctness argument and this is it: a box whose
// centre is further from the surface than its own half-diagonal plus a slack term is entirely on
// one side of that surface, so every cell in it can be filled -- or cleared -- without being asked.
// The argument is only as good as the slack, and when the slack is short the box claims cells the
// per-cell rule would have decided the other way.
//
// **D725 found that it does happen and could not say which side was right.** 220 voxels of
// 1,430,104 on `clips/sampler.clip`, found by accident: an agent batching CELLS instead of LEVELS
// built a world 220 voxels lighter and refused to ship it. `tests/test_sample.cpp` could not see
// it, and the reason is worth being exact about -- its brute-force reference is right, and it is
// only ever pointed at hand-built fields of two or three primitives. The clip that disagrees is the
// one with a stair, a torus, five displaced slabs and a shell in it, and no test ever asked that
// clip the question at all.
//
// So this asks it, over the real file, at the resolution the file names. `set_box_cell_audit` makes
// every bulk-settled box walk its own cells through `Field::eval` at the finest level, with the
// thin-feature rescue and nothing else -- which is exactly what `descend` does to a box of one
// voxel, and shares no reasoning at all with the box test it is checking.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/jobs.hpp"
#include "forge/clip_script.hpp"
#include "forge/sample.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

std::string clip_path(const char* name) {
    return std::string(WS_ASSET_SOURCE_DIR) + "/../clips/" + name;
}

bool same_box(const BoxCellFault& a, const BoxCellFault& b) {
    return a.box_low[0] == b.box_low[0] && a.box_low[1] == b.box_low[1] &&
           a.box_low[2] == b.box_low[2] && a.side[0] == b.side[0] && a.side[1] == b.side[1] &&
           a.side[2] == b.side[2] && a.box_solid == b.box_solid;
}

// The faults, said in a way somebody can go and look at: one line per BOX rather than per cell,
// because a box settling wrong is one event and its cells are the size of it.
void report(const BoxCellAudit& audit, const char* what) {
    std::printf("\n--- box against cell, %s ----------------------------------\n", what);
    std::printf("  boxes settled solid %llu, empty %llu; cells checked %llu\n",
                static_cast<unsigned long long>(audit.boxes_solid),
                static_cast<unsigned long long>(audit.boxes_empty),
                static_cast<unsigned long long>(audit.cells_checked));
    std::printf("  the box said SOLID and the cell says air:    %llu\n",
                static_cast<unsigned long long>(audit.solid_over_claimed));
    std::printf("  the box said EMPTY and the cell says matter: %llu\n",
                static_cast<unsigned long long>(audit.empty_over_claimed));
    if (audit.faults.empty()) return;

    // Where they ALL are, in metres, and how big the boxes were. A fault list that fits in one
    // small box of the clip names the op; one spread over the building names the descent.
    {
        f64 lo[3] = {1e30, 1e30, 1e30};
        f64 hi[3] = {-1e30, -1e30, -1e30};
        usize by_side[9]{};
        for (const BoxCellFault& f : audit.faults) {
            for (u32 axis = 0; axis < 3; ++axis) {
                const f64 at = (static_cast<f64>(f.voxel[axis]) + 0.5) * f.voxel_metres;
                lo[axis] = (at < lo[axis]) ? at : lo[axis];
                hi[axis] = (at > hi[axis]) ? at : hi[axis];
            }
            const i32 side = (f.side[0] > f.side[1]) ? f.side[0] : f.side[1];
            by_side[(side < 9) ? static_cast<usize>(side) : usize{8}] += 1;
        }
        std::printf("  every disagreeing cell lies inside (%.4f, %.4f, %.4f)..(%.4f, %.4f, %.4f)\n",
                    lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        std::printf("  by the settled box's longest side:");
        for (usize i = 1; i < 9; ++i) {
            if (by_side[i] > 0) {
                std::printf("  %llu at %llu%s", static_cast<unsigned long long>(by_side[i]),
                            static_cast<unsigned long long>(i), (i == 8) ? "+" : "");
            }
        }
        std::printf("\n");
    }

    std::vector<usize> first;
    for (usize i = 0; i < audit.faults.size(); ++i) {
        bool seen = false;
        for (const usize at : first) seen = seen || same_box(audit.faults[at], audit.faults[i]);
        if (!seen) first.push_back(i);
    }
    std::printf("  %llu cells over %llu distinct boxes; the first of them:\n",
                static_cast<unsigned long long>(audit.faults.size()),
                static_cast<unsigned long long>(first.size()));
    for (usize k = 0; k < first.size() && k < 40; ++k) {
        const BoxCellFault& f = audit.faults[first[k]];
        usize cells = 0;
        for (const BoxCellFault& g : audit.faults) {
            if (same_box(f, g)) ++cells;
        }
        std::printf(
            "    box at %lld,%lld,%lld  %dx%dx%d cells  said %s  | centre %+.6f  radius %.6f"
            "  reach %.6f (slack %.6f)\n",
            static_cast<long long>(f.box_low[0]), static_cast<long long>(f.box_low[1]),
            static_cast<long long>(f.box_low[2]), f.side[0], f.side[1], f.side[2],
            f.box_solid ? "SOLID" : "EMPTY", f.centre_value, f.radius, f.reach,
            f.reach - f.radius);
        std::printf(
            "        cell %lld,%lld,%lld at (%.4f, %.4f, %.4f) reads %+.6f%s   | %llu cells here\n",
            static_cast<long long>(f.voxel[0]), static_cast<long long>(f.voxel[1]),
            static_cast<long long>(f.voxel[2]),
            (static_cast<f64>(f.voxel[0]) + 0.5) * f.voxel_metres,
            (static_cast<f64>(f.voxel[1]) + 0.5) * f.voxel_metres,
            (static_cast<f64>(f.voxel[2]) + 0.5) * f.voxel_metres, f.cell_value,
            f.cell_rescued ? " (rescued)" : "", static_cast<unsigned long long>(cells));
    }
}

}  // namespace

TEST_CASE("a box settled in bulk agrees with its own cells, on clips/sampler.clip") {
    VoxelTypeTable types;
    TagRegistry tags;
    const Script script = load_clip_script(clip_path("sampler.clip"), types, tags);
    REQUIRE_MESSAGE(script.errors.empty(), "clips/sampler.clip did not parse");

    JobSystem jobs;
    set_box_cell_audit(true);
    const SampleResult result =
        sample(script.field, script.solid, script.paint, script.settings, &jobs);
    set_box_cell_audit(false);
    const BoxCellAudit audit = take_box_cell_audit();
    report(audit, "clips/sampler.clip at its own metre");

    // What the sample cost, in numbers that do not drift. Six other agents can be on this machine
    // and a wall clock taken beside them is noise (D725); an evaluation count is the same in every
    // arm on every machine, so it is what a change to the SLACK has to be judged by -- widening a
    // slack buys correctness with sampling, and this is the price in the only currency that holds
    // still. The audit's own evaluations are deliberately not in any of these.
    std::printf("  cost: %llu shape evals, %llu paint, %llu voxels asked, %llu settled in bulk\n",
                static_cast<unsigned long long>(result.shape_evaluations),
                static_cast<unsigned long long>(result.paint_evaluations),
                static_cast<unsigned long long>(result.voxels_asked),
                static_cast<unsigned long long>(result.voxels_settled));

    CHECK(result.voxels_settled > 0);
    CHECK_MESSAGE(audit.solid_over_claimed == 0,
                  "a box settled SOLID over cells the per-cell rule calls air");
    CHECK_MESSAGE(audit.empty_over_claimed == 0,
                  "a box settled EMPTY over cells the per-cell rule calls matter");
}
