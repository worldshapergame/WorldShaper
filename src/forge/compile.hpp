#pragma once
// The field compiler: the same shape, rewritten so that asking it about a point costs less.
//
// # Why this exists, and what eight measurements before it already refused
//
// Filling one cell of the estate walks **8,231 nodes of an 18,250-node field**, and that walk is
// 76% of what loading a world costs (D682). Eight separate attacks on that number have been
// measured and written down, and the best of them was worth 1.3x:
//
//     the field evaluated on the card              1.30x outdoors, 0.37x indoors  (D677, D681)
//     the descent, on the card                     nothing                        (D678)
//     shrinking the shader's stack frame           nothing                        (D678)
//     the union hierarchy over wide unions         nothing, twice          (D637, D682)
//     the thin-feature rescue off                  11%                            (D681)
//     bounding every paint rule perfectly          at most 1.3x                   (D682)
//     pruning the field to the sample box          2%                             (D683)
//     building the whole world up front            22 s at 50 cm                  (D686)
//
// Every one of those changed *how the field is walked*. **None of them changed the field.** D683's
// own conclusion is the opening this file works in: *"the 8,231 nodes a cell walks are all
// genuinely NEAR it"* and *"the only quantity left that moves the load is how much clip there
// is"* (D686). It then says that is a decision for whoever authors the estate — which is true of
// the GEOMETRY and is not true of the STRUCTURE the geometry is written in.
//
// A large part of that expression is not shape at all. The estate is seven buildings, each bound
// as `part_x = translate { x_assembly }` joining one union of seven, so every shape a paint rule
// names carries a translated twin over whatever it was already made of, and the deepest path is
// 80 nodes (D676). `Field::combine` chains a union of more than four parts into a LEFT-DEEP list,
// so a union of forty parts is thirteen frames deep and its first child's box is the box of
// everything before it — a box no point is ever outside, so the cull can never reject it and the
// whole chain is descended at every sample. None of that is building. All of it is walked.
//
// So: **a translate over a box is a box somewhere else**, and saying so removes a node from the
// walk without removing anything from the building.
//
// # The rule this is held to, and it is the project's worst class of fault
//
// D204: two evaluators that disagree about one world. `documentation/21-renderer-rewrite.md`
// calls that "two renderers computing the same world" and it is the failure this whole file is
// one wrong sign away from. **A compiled field must answer the same distance the original does**,
// and `tests/test_compile.cpp` samples both over a dense grid across the estate's real bounds and
// states the worst deviation in metres against a 3.125 cm voxel.
//
// Most of what follows is bit-exact by construction rather than by tolerance:
//
//   * `min` and `max` are exactly associative and commutative in IEEE-754 for non-NaN operands,
//     so flattening and re-balancing a union changes no bit of the answer.
//   * A chain of `Add`/`Multiply`/`Difference`/`smooth`/`chamfer` is re-emitted in the SAME order
//     `Field::combine` built it in, so their (non-associative) arithmetic is unchanged.
//   * A constant subtree is folded by asking `Field::eval` for its value once, so the folded
//     number is the number the original computed, not a re-derivation of it.
//
// Two rewrites are exact in real arithmetic and differ in the last bits of a double:
//
//   * Absorbing a translate into a primitive's centre computes `p - (c + t)` where the original
//     computed `(p - t) - c`. Same value, different rounding: at most an ulp of the coordinate,
//     which at 100 m from the origin is 1.4e-14 m — eleven orders of magnitude under the 0.47 µm
//     f32 error D676 already accepted for the whole field.
//   * `Field::arc`, `Field::revolve`, `Field::polar_repeat` and `Field::plane` NORMALISE their
//     arguments, and re-emitting an already-normalised node round-trips through that arithmetic
//     a second time. `Field::push` is private, so there is no way to place a node byte-for-byte;
//     the round trip is worth at most an ulp of a turn or of a unit normal.
//
// And one that is exact in value while changing which children get ASKED: re-shaping a union
// changes its internal boxes, so the cull in `Field::eval` makes different decisions. That cull
// is sound for every primitive that reports a true distance and is NOT sound for the four that
// D644 measured under-stating (ellipsoid 0.5877, cone 0.5300, prism 0.8660, platonic 0.5774), so
// a re-shaped union over one of those four can legitimately differ. That is why the gate is a
// measured bound and not an equality, and why the test reports the worst deviation it found
// rather than asserting a number somebody hoped for.
//
// # What it will not do
//
// It will not make the field bigger. Every rewrite here is proved non-increasing in node count
// before it is allowed to fire, and the mechanism that guarantees it is `refs`: a shared
// subexpression is NEVER specialised for one of its parents, so no subtree is ever duplicated and
// no input node is emitted more than once. A compiler that "optimises" a clip into twice the
// nodes is the one failure mode nobody would notice from a screenshot. The witness pass that
// carries NAMES (below) is held to the same promise by the same arithmetic and refuses a name
// rather than breaking it.
//
// It will not prune. `Field::pruned_to` was built, was exact, and was worth 2% (D683); the
// children a union could drop are not there to drop.
//
// It will not re-group a union spatially. That is the accelerator, and it has been measured at
// nothing on two clips (D637, D682). Re-balancing here PRESERVES the author's order and only
// changes how deeply the same list is nested.

#include <vector>

#include "core/types.hpp"
#include "forge/field.hpp"

namespace ws {
namespace forge {

// "this input node does not exist in the output" — the entry `CompileReport::remap` carries for a
// node that was flattened into its parent, folded to a number, or never reached from any root.
//
// It has to be a value and not a zero, because zero is a perfectly good node index and a caller
// that reads a stale name as node 0 gets the first primitive of the clip instead of an error.
constexpr u32 kUnmapped = 0xFFFFFFFFu;

// What a compilation did, in the numbers that decide whether it was worth doing.
//
// Reported rather than inferred, and for D682's reason: `accelerator_count()` read nought on
// every clip ever built and nothing said whether that was a decision or a result. A rewrite that
// fires nought times has to say so in its own line, or the next session measures the whole pass
// and concludes the IDEA is worthless when what happened is that it never ran.
struct CompileReport {
    // The remapped roots, in the order they were handed in. `roots[i]` is the node in the returned
    // field that answers what the caller's `roots[i]` answered in the original.
    //
    // **This is the whole reason the signature is a SET.** A `Script` names many nodes — the solid,
    // the sample bounds, and every paint rule's `test` and `place` — and a compilation renumbers
    // all of them. Compiling from the solid alone and leaving the rest at their old indices is a
    // building painted from whatever shapes now occupy those numbers: no error, no crash, and a
    // wrong building. So there is no single-root answer to read; there is a set in and a set out.
    std::vector<u32> roots;

    // `roots[0]`, for the one-root convenience overload and for the tests that predate the set.
    // Zero when nothing was compiled, which is why `roots` is the thing to read in anger.
    u32 root = 0;

    // Every input node's answer in the output, or `kUnmapped` where it has none.
    //
    // Sized `in.size()`. This is NOT how the roots are remapped — those go through `roots`, which
    // is exact by construction — it is how a caller re-points the NAMES it kept: `Script::parts`,
    // a weathering scope, anything that held an index for diagnostics rather than for building.
    // A node that was flattened into its parent or folded to a number has no output of its own and
    // reads `kUnmapped`, which a caller must treat as "that name is gone" and never as node 0.
    //
    // **A name handed to the `names` overload does not have to rely on this.** `remap` is what the
    // roots' own emission happened to leave behind; `names` below is asked for on purpose.
    std::vector<u32> remap;

    // What each NAME became, in the order the names were handed in, or `kUnmapped` where the name
    // genuinely has no answer.
    //
    // Same length as the `names` argument. This is the witness pass's answer and it is the reason
    // `--compile-field` can be switched on at all: on the estate `remap` alone kept 0 of 5,091
    // names, because `apply_origin` wraps every named part in its own fresh translate that nothing
    // else references, so not one of them is reachable from the building's roots (D690).
    std::vector<u32> names;

    usize names_kept = 0;
    usize names_answered_as_themselves = 0;   // the node survived the rewrite and cost nothing
    usize names_rebuilt = 0;                  // it had dissolved; re-emitted out of the same parts
    usize names_dropped_unhandled = 0;        // an op this pass was never taught, under the name only
    usize names_dropped_over_budget = 0;      // rebuilding it would have made the field bigger
    usize names_dropped_out_of_range = 0;

    // False when the input holds an op this pass cannot rebuild. The returned field is then a
    // COPY of the input and `root` is the input's root, so an unchecked caller still gets a
    // correct field and not a wrong one — trap 7, "I could not" and "the answer is nought" must
    // never be the same reply.
    bool ok = true;
    Op unhandled = Op::Constant;

    usize nodes_before = 0;       // reachable from `root` in the input
    usize nodes_after = 0;        // the whole output field, names included
    usize nodes_in_input = 0;     // `in.size()`, including whatever no longer answers anything
    usize depth_before = 0;       // longest root-to-leaf path, in nodes
    usize depth_after = 0;

    // The output before the witness pass ran: what the ROOTS alone needed, which is the number
    // `nodes_before` is the honest comparison for and the one every figure in D690 was taken as.
    // With no names handed in it is `nodes_after`.
    //
    // Kept separate because a name is not part of the walk. Nothing reaches a name's node from
    // `solid`, so a name costs a node in the array and nought in the sample — and reporting one
    // number for both would say a compiler had stopped compressing when what it had done was
    // start answering `--part`.
    usize nodes_for_roots = 0;
    usize name_nodes_added = 0;   // `nodes_after - nodes_for_roots`

    // Everything from here to `parameter_nodes_kept` counts what the ROOTS' rewrite did, and not
    // what carrying a name did. The witness pass goes through the same `place` and the same
    // `gather`, so leaving them shared would move this whole table the moment somebody asked
    // `--part` a question — a measurement changing under its own instrument.

    // Transform folding.
    usize translates_before = 0;              // Translate nodes reachable in the input
    usize translates_after = 0;
    usize absorbed_by_centre = 0;             // a translate landed on a primitive's centre
    usize absorbed_by_plane = 0;              // ...on a plane's offset
    usize absorbed_by_capsule = 0;            // ...on both ends of a capsule
    usize chains_merged = 0;                  // translate over translate became one vector
    usize pushed_through_transform = 0;       // carried through a rotate, scale or mirror
    usize pushed_through_pointwise = 0;       // distributed over a union, a difference, a displace
    usize materialised = 0;                   // a translate that had to stay, over a barrier
    usize refused_shared = 0;                 // a push refused because the subtree has two parents
    usize refused_costly = 0;                 // ...because it would have cost more than it saved

    // Structure.
    usize combines_flattened = 0;             // a same-op child absorbed into its parent
    usize combines_rebalanced = 0;            // a chain of more than four re-nested as a tree
    usize duplicate_children_dropped = 0;     // min(a, a) is a

    // Value.
    usize constant_subtrees_folded = 0;       // an expression with no point in it, evaluated once
    usize constant_nodes_removed = 0;
    usize identities_removed = 0;             // translate by nought, scale by one, offset by nought
    usize shared_by_cse = 0;                  // structurally identical nodes emitted once

    usize parameter_nodes_kept = 0;           // slots preserved so a dial still moves the shape
};

// How hard the pass is allowed to work, and — the part that matters here — the control arm.
//
// D683's ledger exists because seven levers were measured one at a time. Every rewrite in this
// file is a switch for the same reason: a pass measured whole cannot say which of six ideas paid
// for itself, and "all of them together were worth 1.2x" is exactly the shape of a number that
// sends the next session round the same loop. Take the arms.
struct CompileOptions {
    // --- OFF, and this is the finding rather than a default somebody never got round to -----
    //
    // Folding transforms was the idea this file was written for, and **measured it costs 1.19x.**
    // The estate, 216 points inside the block, best of seven interleaved rounds, repeated over six
    // runs (`tests/test_compile.cpp` prints the table):
    //
    //     original, as the clip parses                     1.000x    18,250 nodes
    //     rebuilt, every rewrite off (the control arm)     1.04x      8,898
    //     the transform fold only                          0.87x      8,814
    //     the fold with no leaf copies                      0.87x      8,843
    //     everything except re-balancing                   1.05x      8,433
    //     flatten and re-balance only                      1.21x      8,776
    //     everything (what ships)                          1.20x      8,536
    //     everything with the fold switched on             1.05x      8,497
    //
    // Run-to-run noise on this harness is about 4%, taken from two rows that are the same
    // configuration by two routes, so 1.20x is 1.20x and 1.05x is the control.
    //
    // The control arm is the row that makes the rest mean anything: a compiled field is a
    // different ARRAY as well as a different expression — only the reachable nodes, freshly
    // allocated, in a different order — and that alone is worth 1.5%. Everything past it is work.
    //
    // **The fold removes nodes and makes the walk slower**, which is not a contradiction once the
    // walk is what is being measured rather than the graph. A translate above a union is visited
    // ONCE per visit to that union. Pushed down, it is visited once per CHILD the cull actually
    // descends into — and on a building, where every part's box spans the block (D637's finding,
    // twice confirmed), that is most of them. Node count went down by 4% and node VISITS went up.
    // It is the same shape of mistake as the union hierarchy: a structure that looks cheaper on
    // paper and costs more per point.
    //
    // Kept rather than deleted, and switchable rather than removed, for D637's reason: a clip of
    // separated shapes that share no vocabulary is the case it was written for, and the estate is
    // not that clip — 665 of its 1,063 translates stand over a subtree with more than one parent,
    // which is `_order.clip`'s vocabulary being placed over and over. On a clip authored the other
    // way this could still pay, and the flag is how somebody finds out in one run instead of
    // rebuilding this file.
    bool fold_transforms = false;     // translate into a centre, and through what it distributes over
    bool absorb_into_shared_leaves = false;   // ...even into a shared leaf, by copying the leaf

    // --- ON, and these are the 1.21x --------------------------------------------------------
    // These two are one lever with two switches, and the second is where all of the 1.20x lives.
    // Flattening on its own is worth NOTHING — 1.05x against a 1.04x control — because gathering a
    // nested union into one long list only for `Field::combine`'s own nesting to chain that list
    // left-deep is DEEPER than the grouping the author wrote: the estate's longest path goes from
    // 146 nodes to 168. Re-balancing is what turns the list back into a tree.
    //
    // Why a tree and a list differ so much, when the arithmetic is identical: the cull in
    // `Field::eval` sorts a union's children by how far the point is from each child's box and
    // stops at the first it can reject. Child 0 of a chain link is the whole rest of the chain, so
    // its box is the union of everything before it — a box no point inside the building is ever
    // outside — so it sorts first, is never rejected, and the walk descends the entire chain at
    // every sample. Nested four at a time, a group that is nowhere near the point is one box test.
    bool flatten_combines = true;     // union { union { a b } c } is union { a b c }
    bool rebalance_combines = true;   // and a chain of forty is a tree of forty, not a list
    bool fold_constants = true;       // an expression with no point in it is a number
    bool remove_identities = true;    // translate by nought is its child
    bool drop_duplicates = true;      // min(a, a) is a
    bool share_identical = true;      // one node where the clip wrote the same thing twice

    // How many parts one combine may be flattened into before the pass stops gathering. A guard
    // rather than a tuning knob: a clip that nests unions a thousand deep should not be turned
    // into one node with a thousand-entry working list inside the compiler.
    usize flatten_limit = 1024;
};

// An equivalent field, cheaper to walk, and the numbers that say by how much.
//
// The returned field carries every parameter slot of the input, by name and in the same slot
// order, so `set_parameter` still moves what it moved before — a compiled clip whose dials have
// silently stopped working is a clip that looks right until somebody drags one.
//
// `Field::build_bounds()` has not been called on the result and must be: the output is built
// entirely through the public builders, so every child still has a lower index than its parent
// and the one forward pass `build_bounds` makes is still valid.
//
// **A SET of roots, and the set is the point.** Every root is compiled into ONE output field with
// one shared table of nodes, so a subexpression two roots have in common is emitted once and stays
// shared — which is also what makes the result no bigger than the input. `report->roots` comes back
// in the same order and the same length as `roots`, and `report->remap` re-points anything else the
// caller was holding.
//
// A root that is out of range, or an op this pass was never taught, refuses the WHOLE compilation:
// the returned field is a copy of the input and `report->roots` is the input's own roots unchanged,
// so a caller that ignores `ok` still has a correct clip rather than a scrambled one. Trap 7 — "I
// could not" and "the answer is nought" must never be the same reply.
Field compile_field(const Field& in, const std::vector<u32>& roots,
                    CompileReport* report = nullptr,
                    const CompileOptions& options = CompileOptions{});

// The same compilation, plus a set of NAMES that must still mean something afterwards.
//
// # Why a name cannot simply be a root, and it is the thing that kept `--compile-field` off
//
// D690 wired the compiler and shipped it OFF for one reason: `script.parts` — the 5,091 pieces the
// estate declares, which `--part` and the tooling look up — survived only where a name's node came
// through the rewrite AS ITSELF, and that was **0 of 5,091**. `apply_origin` wraps every named part
// in its own fresh translate that nothing else references, so no name is reachable from `solid`,
// from the bounds or from any paint rule, and none of them was ever emitted.
//
// **Adding the names to `roots` fixes that and guts the rewrite.** `refs` is this file's whole
// safety mechanism, `gather` only flattens a child with ONE parent, and a name is a parent: hand
// the 5,091 in as roots and every named intermediate of the building — which on a clip written out
// of a shared vocabulary is most of it — reads two parents and stops flattening. The 1.19x is the
// flatten-and-re-balance pair (the fold measured 0.887x and is off), so blocking the flatten is
// spending the entire win to answer a diagnostic.
//
// # So a name is carried WITHOUT being a reference: the witness pass
//
// The names take no part in `survey`, so `refs` is what it would have been and the roots' output is
// node-for-node what it would have been with no names at all. They are emitted AFTERWARDS, through
// the same one door every other node goes through, against an `emitted` table the roots have
// already filled. Three things fall out of that order:
//
//   * A name whose node survived as itself is a table lookup. It costs nothing and it is exact.
//   * A name whose node DISSOLVED — flattened into its parent, folded to a number — is re-emitted
//     out of the parts it dissolved into. That is a node which is EQUIVALENT rather than identical,
//     and it is built by the same rules the roots were, not chosen by resemblance.
//   * A name over a subtree nothing else reaches is built now rather than never.
//
// # Where a name has no answer it is dropped, and that has not been softened
//
// D690 kept `--part` saying "does not name anything" rather than sampling the wrong shape, and that
// property is the reason any of this is trustworthy. Three things still drop a name, each counted
// separately in the report because they are different facts about a clip:
//
//   * it is out of range;
//   * its own subtree holds an op this pass was never taught — the root scan cannot have seen it,
//     because nothing reachable from a root leads there;
//   * rebuilding it would push the output past the input plus ONE NODE A NAME. That is this file's
//     promise — "it will not make the field bigger" — with the one allowance a name genuinely
//     needs: a name that dissolved dissolved into its parent, and putting it back is the single
//     combine node that says where it ended. Names are answered in the order they are handed in and
//     the ones that no longer fit are refused rather than squeezed.
//
//     The flat version of that bound — never past `in.size()` — was tried and is wrong. It refused
//     `slabs` on `clips/sampler.clip`, a 47-node clip whose roots compile to 43 and whose 28 names
//     want 48: one node over, and `--part slabs` came back "does not name anything" while the walk
//     had gone 40 nodes to 43. A name is NOT on the walk, so it costs an array slot and nothing per
//     sample, and refusing one to hold a total under a line spends the thing somebody notices to
//     protect a number nobody reads.
//
// `report->names` comes back in the same order and the same length as `names`, with `kUnmapped`
// wherever one was dropped. A refusal of the whole compilation hands the names back unchanged,
// like `roots` and `remap`, so a caller that ignores `ok` still holds a consistent clip.
Field compile_field(const Field& in, const std::vector<u32>& roots,
                    const std::vector<u32>& names, CompileReport* report = nullptr,
                    const CompileOptions& options = CompileOptions{});

// The one-root case, which is every test in `tests/test_compile.cpp` and no caller in the engine.
// `report->root` and `report->roots[0]` are the same node. Kept because a field with one answer in
// it is the shape most of the reasoning about this pass is done in, and writing `{root}` at fifty
// call sites buys nothing.
Field compile_field(const Field& in, u32 root, CompileReport* report = nullptr,
                    const CompileOptions& options = CompileOptions{});

// The longest root-to-leaf path through an expression, in nodes.
//
// Exposed because it is the number `Field::kMirrorStack` is sized against — 128, being 1.66x the
// 80 the estate measured (D676) — and a pass that claims to make an expression shallower has to
// be checkable against the same quantity the shader budgets for.
usize expression_depth(const Field& f, u32 root);

// The deepest of a set of roots, which is what a stack has to be sized against when a clip has
// several answers in it rather than one.
usize expression_depth(const Field& f, const std::vector<u32>& roots);

// How many nodes a walk from `root` can reach. Not `Field::size()`: a field that has been edited,
// or whose root is one part of a bigger graph, carries nodes that answer nothing.
usize reachable_nodes(const Field& f, u32 root);

// How many DISTINCT nodes the whole set can reach. Not the sum: the roots of one clip overlap
// heavily — every paint rule's test is usually a shape the solid is made of — and adding their
// walks up counts the building once per rule that mentions it.
usize reachable_nodes(const Field& f, const std::vector<u32>& roots);

}  // namespace forge
}  // namespace ws
