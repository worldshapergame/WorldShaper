// WorldShaper - entry point.
//
// Window, device, world, streaming, and the two render passes: a hierarchical ray march
// that writes a visibility buffer, and a resolve that turns it into pixels. Plus two
// headless audit modes that run in CI, because most of what can go wrong here is not
// visible on screen.

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>

#include "core/arena.hpp"
#include "core/hash.hpp"
#include "core/jobs.hpp"
#include "core/crash.hpp"
#include "core/log.hpp"
#include "game/quality.hpp"
#include "world/light_list.hpp"
#include "app/loading.hpp"
#include "app/updater.hpp"
#include "core/time.hpp"
#include "core/version.hpp"
#include "debug/hud.hpp"
#include "game/camera.hpp"
#include "game/chisel.hpp"
#include "forge/clip_script.hpp"
#include "forge/measure.hpp"
#include "forge/sample.hpp"
#include "game/clipboard.hpp"
#include "game/repeat.hpp"
#include "game/toolbelt.hpp"
#include "gpu/device.hpp"
#include "gpu/feedback.hpp"
#include "gpu/image.hpp"
#include "gpu/loading_screen.hpp"
#include "gpu/face_buffers.hpp"
#include "gpu/face_light.hpp"
#include "gpu/node_buffers.hpp"
#include "gpu/render_params.hpp"
#include "gpu/profiler.hpp"
#include "gpu/screenshot.hpp"
#include "gpu/shader.hpp"
#include "gpu/shell_pass.hpp"
#include "gpu/swapchain.hpp"
#include "gpu/type_tables.hpp"
#include "platform/audio.hpp"
#include "platform/window.hpp"
#include "ui/shell.hpp"
#include "world/history.hpp"
#include "world/op.hpp"
#include "world/raycast.hpp"
#include "world/face_store.hpp"
#include "world/node_pool.hpp"
#include "world/serialize.hpp"
#include "world/world_cache.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

// Room on the GPU for the ghosts. A *preview* budget, not a limit on the tool: a clip too
// big to fit here is still selected, transformed and stamped exactly ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it is drawn as an
// outline instead of as voxels. Thirty-two megabytes each side of the copy.
inline constexpr u64 kMaxClipPoolCells = 8ull * 1024ull * 1024ull;

// How long a scripted run may take before it stops and reports where it got to, in seconds.
//
// Three minutes, measured from the start of the load, because a cold clip cache is 133 s of
// sampling on its own (D241) and a settled run of any camera on this grid is under ten. So a run
// that reaches this is either cold - in which case its figures were never comparable with anything
// anyway - or slow, which is the finding.
//
// It is a default rather than an option somebody remembers to pass, because the runs that most
// need it are the ones nobody expected to be slow. `--max-seconds 0` turns it off, deliberately
// and out loud.
inline constexpr f64 kDefaultMaxSeconds = 180.0;

struct Options {
    bool headless = false;
    u64 ticks = 0;
    u32 width = 1600;
    u32 height = 900;
    bool size_explicit = false;   // an explicit size overrides the fit-to-desktop clamp
    bool vsync = true;
    bool validation = (WS_DEBUG != 0);
    bool help = false;
    // A development build should not go looking for a release older than what is sitting in
    // front of you, and a scripted screenshot should not depend on the network.
    bool no_update_check = (WS_DEBUG != 0);
    bool no_clip_cache = false;   // always rebuild the clip, never read or write the cache
    bool despeckle = true;        // repaint lone voxels of the wrong material (D610)
    // The control arm for the region paste's own pool: put the paste back on the sampler's job
    // system, which is where it was and which is what made it wait for the sample. See D511.
    bool no_paste_pool = false;
    std::string clip_part;        // build only this let name, for looking at one piece

    // A smaller box to sample, overriding the clip's own.
    //
    // Sampling cost is per voxel and per field evaluation, so a representative slice at FULL
    // resolution measures the thing that actually matters, in seconds rather than minutes.
    // Measuring at a coarser --clip-metre instead changes the very thing under test ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â how often
    // a box can settle depends on how large a voxel is ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so it answers a different question,
    // confidently and wrongly.
    std::string clip_bounds;
    // Start the measurement window when the world stops changing, rather than at frame nought.
    // Any figure meant to be compared with another run wants this; see the note at its use.
    bool settle = false;
    // Which marcher walks the world.
    //
    // The node pool is the default. Measured across the whole camera grid it is faster on six
    // views of seven, up to three times faster where distance dominates, holds 4.8 MB against
    // 57.7, and draws a picture that agrees with the old marcher to within one part in three
    // hundred. See documentation/21-renderer-rewrite.md section 8.
    //
    // The chunk marcher stays behind --chunk-marcher rather than being deleted, because R1e has
    // not happened yet and because two marchers that can render the same camera are how any
    // disagreement gets settled. It goes when the old addressing does.
    u32 hollow = 0;            // shell thickness for the scripted edit, and the starting value

    // Automatic quality. Off, or pinned, or aimed at something other than the monitor.
    f32 target_fps = 0.0f;        // 0 means "the monitor's refresh rate"
    bool no_auto_quality = false;
    bool benchmark = false;       // re-run the machine measurement and save the result
    u64 edit_frame = 0;           // apply --edit on this frame instead of frame 100

    // A player chiselling CONTINUOUSLY, which is the worst case this engine has and the one no
    // instrument could reach.
    //
    // `--edit` fires once, at a known frame, so it measures how an already-converged picture
    // recovers from one change. That is not what a player does. A player flies along holding the
    // button down, carving and placing as they go, and every one of those edits reopens every face
    // within `kEditShadowReach` while the camera is still revealing new ones. The two costs land on
    // the same frames and nothing measured them together.
    //
    // Every `chisel_every` frames a cube of half-width `chisel_radius` voxels is carved or filled a
    // few metres in front of the eye, alternating, through the same `apply_group` and the same
    // `invalidate_edited_chunks` the mouse button uses -- so what is being measured is the game and
    // not a parallel implementation of it.
    u64 chisel_every = 0;         // 0 is off
    i64 chisel_radius = 16;       // voxels: half a metre

    // The two levers the light pass gained, as switches, so an A/B is one build with two runs.
    //
    // D407 is why they are switches and not two builds: this pass is a function of how much of the
    // face store is still measuring, that state is not reproducible between batches, and the same
    // build has read 2.41 ms and 3.75 ms on it in one session. Arms that are two builds are not
    // comparable; arms that are two flags are.
    //
    //   --face-gate N       how many frames a face keeps being lit after the last pixel that read
    //                       it. `--no-face-gate` widens it to the whole run, which is the control.
    //   --no-face-worklist  dispatch the shading pass over every live slot again, rather than over
    //                       the compacted list of the ones that owe work.
    //   --no-face-prolong   a newly subdivided face measures its ambient term from nothing again,
    //                       instead of inheriting the fit of the face it came out of.
    u32 face_gate = 4;
    bool face_worklist = true;
    // OFF by default, and that is a measurement rather than a preference. See the prolongation
    // block in shaders/shade_faces.comp: on the facility there are no intermediate levels for a
    // face to inherit from -- the store holds level 0 and its level-3 stand-ins and nothing between
    // -- so it has nothing to do here and measured as nothing (4.92 against 5.05 flying, 8.25
    // against 8.26 while chiselling, both inside the spread). It is kept, switchable, because the
    // case it is for is real in a scene with mid-distance geometry, and because a prior that is
    // correct is worth having written down; it is not on because nothing has measured it helping.
    bool face_prolong = false;

    // A ray tells residency about every brick it crosses, not only the one it stops on.
    //
    // On, because off is the fault: `--no-node-crossings` is the control arm, and it restores what
    // D426 measured -- 92% of all evictions taking nodes that were inside the frustum, 99.94% of
    // them nodes no ray had ever reported reading, and 37,606 of them requested again within two
    // seconds. Switchable rather than two builds for the reason the three above are (D407).
    bool node_crossings = true;

    // A light ray says it is using the geometry it is stopped by, rather than only ever saying
    // that a cell is missing.
    //
    // On, because off is the loop D429 measured: 28,764 of 29,077 rebuilds on a settled, static
    // camera, all of them occluders no primary ray ever reads, each one thrown away and asked for
    // again on a six-hundred-frame cycle. `--no-light-keeps-geometry` is the control arm, and
    // `--light-read-period N` sweeps the trade: how many frames a node may go unstamped, against
    // the feedback entries the stamping costs. A power of two; 0 is off.
    u32 light_read_period = 16;

    // How much confidence a face keeps when an edit announces that the world under it changed.
    //
    // Eight, and `--face-edit-seed 0` is the control arm: it restores the wipe, which is what a
    // player was seeing as the room turning to eight-voxel blocks and flashing on every edit. A
    // face at nought samples is not answerable, so every pixel on it falls to the coarse stand-in
    // three levels up -- and indoors that stand-in is inside the same sixteen-metre box and has
    // been wiped by the same line. See kFaceEditSeed in shaders\node.glsl.
    u32 face_edit_seed = 8;

    // An edit reopens a face's LAMP term only where it can stand between that face and a fitting.
    //
    // On, because off is the reported bug: the lamps were reopened over the sun's sixteen-metre box,
    // so every edit restarted the lamp estimator on every face in the room. Measured at the enclosed
    // camera with one edit a second, `--no-lamp-edit-scope` against the default: not one face of
    // 121,013 held a settled lamp term, and 43% of the frame changed between two consecutive frames.
    bool lamp_edit_scope = true;

    // Press undo, or redo, once, on this frame. Zero is off.
    //
    // The whole point is that they go through the same code the key does. Undo was reported twice
    // as "does nothing", and both times the world had gone back correctly and only the picture had
    // not -- which no test could see, because every test asked the World what it held and the
    // fault was in what the renderer had been told. With this the question is a screenshot: edit,
    // undo, and the frame must come back to what it was before the edit.
    u64 undo_frame = 0;
    u64 redo_frame = 0;
    i32 quality_level = -1;       // -1 means "decide it"

    // A clip authored as a file. With no --screenshot it builds the clip, measures it and
    // prints what it found without opening a window; with one, the clip is stamped into the
    // world at the origin so the ordinary camera and screenshot machinery can look at it.
    std::string clip_file;
    std::vector<std::string> clip_slices;   // "axis,at" or "axis,at,step"
    bool clip_symmetry = false;
    bool clip_align = false;
    i64 clip_at[3]{0, 0, 0};                // where to stamp it, in voxels
    // Sample at a fraction of the authored detail and blow it back up on the way in, so the world
    // is the right size and merely blocky, then sharpen it in the background while it is walked
    // around in. A power of two; 1 is the old behaviour of waiting for the whole thing.
    //
    // Four rather than eight. Eight enters the world in a second and a half against five, and both
    // are inside anybody's patience ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but a quarter-detail build is markedly closer to the real
    // one. At an eighth, thin things that ought to be openings fill in: the frames of the secondary
    // doors come back solid white, and the second-storey window surrounds lose their profile. That
    // is not a fault to be fixed, it is what sampling below the size of a feature MEANS, and the
    // answer is to start above it.
    u32 clip_coarse = 4;
    i32 clip_metre = 0;                     // override the file's resolution, for quick previews

    // Deliberately crash, to prove reporting works on this machine before it is needed.
    // "read", "write", "check", "throw", "divzero", or "report" for a report without dying.
    std::string crash_test;

    // Render a fixed number of frames, save the last one, and exit. This is how a
    // rendering change gets checked without a person having to look at the screen.
    std::string screenshot;
    u64 screenshot_frame = 30;
    u32 debug_mode = 0;   // 0 shaded, 1 step count, 2 face normals
    u32 face_budget = 0;  // faces the store may hold; 0 keeps FaceStoreBudget's own figure
    bool whole_set_retry = false;  // the control arm for D544: clear only when it ALL fitted
    bool face_pressure = true;   // shorten the cold window as the table fills (D502)
    u32 face_pressure_from = 0;  // 0 keeps kFacePressureFrom; 2 means "from half free"
    // How often a face a pixel read reports itself to the store, in frames, a power of two. 0 is off
    // and restores residency hearing only from the request lattice (D508's control arm).
    //
    // Sixty-four, because that is the lattice's own period at the resolutions this runs at, so the
    // clock is no coarser than what it replaces while being exact about WHICH faces it covers — and
    // because it sets the floor under every eviction window: half a million live faces reporting at
    // one in sixty-four is about eight thousand entries a frame against a capacity of 131,072.
    u32 face_read_period = 64;
    // How often a face may ask the store for ONE face a light ray of its own landed on, in frames,
    // a power of two. 0 is off and is R9a's control arm (`--no-secondary-faces`).
    //
    // Sixty-four, so the volume is of the same order as D508's read-reports, which is the other
    // per-face report sharing this buffer: about half a million live faces at one in sixty-four is
    // eight thousand entries a frame, and only the faces still casting a far ray say anything at
    // all. What it buys is how fast the off-screen set fills in, which is paid in frames.
    u32 secondary_period = 64;
    // A hard ceiling on the off-screen set as a divisor of the face table. 0 keeps the store's own
    // figure, which is no fixed ceiling at all -- the class is bounded by the table's SPARE room.
    // R9b: a class that overruns must degrade its own refresh rate and nothing else's, and what it
    // may hold before it overruns is what the on-screen set is not using. `--secondary-share 4`
    // restores the fixed quarter and is the control arm.
    u32 secondary_share = 0;
    // How many faces the OFF-SCREEN set may have shaded in one frame, as a divisor of how many the
    // on-screen set has. 0 is the control arm (`--no-secondary-light`) and restores an off-screen
    // face casting nothing at all, which is the state every figure taken before this was measured in.
    //
    // R9b's ray share, which had never been spent. R9a put faces in the store for the surfaces a
    // gathering ray lands on and `may_cast` then refused every one of them a ray, because that gate
    // is a stamp written by the VISIBILITY pass and the visibility pass only runs on pixels. Measured
    // on the close camera, settled: 229,413 off-screen records, nought samples in all of them, and
    // 12.4% of the frame's gathering rays reading them straight back out as black.
    //
    // Eight, which is to say the class may cost about an eighth of what the screen's own faces cost.
    // The number is a trade and not a constant for D430's reason -- a trade nobody can sweep at run
    // time is a trade somebody guesses at -- and what it buys is how fast the room behind you fills
    // in, paid in frames, against a light pass with a 4.40 ms budget.
    u32 secondary_light_share = 8;
    // R9's bounce: a gathering ray reads the surface it lands on, and the composite adds what it
    // found. It replaced `kIndirectFloor` — the constant that stood in for every bounce of indirect
    // light in the building — and that constant is now DELETED rather than switched off, along with
    // `kGroundBounce`, because a surface that receives no light must be black. So `--no-bounce` is
    // the control arm for the bounce and is not a way back to the old picture: with it off, an
    // interior is lit by the sun, the sky and the lamps it can actually see, and by nothing else.
    bool bounce = true;
    // The least far samples a face takes before its bounce may stop. 0 keeps kBounceMin (512).
    // The trade is unbounded rays against a per-face mottle in every interior, and it is a run-time
    // figure because a trade nobody can sweep at run time is a trade somebody guesses at.
    u32 bounce_min = 0;
    // How many samples the bounce REMEMBERS. 0 keeps kBounceMemory (128).
    //
    // The bounce is the one term in this renderer that measures something still in motion — the
    // other faces, whose own light climbs from black as they take their own samples — so a mean over
    // a face's whole history is a mean of the room's fill-up rather than of the room. That made the
    // picture a function of where the camera had been standing, which is what a player reported as
    // the surfaces they had stood still in front of being brighter and cleaner than everything
    // around them. See kBounceMemory in shaders/node.glsl.
    //
    // The control arm is a memory larger than the far count can reach, which is the cumulative mean
    // exactly rather than an approximation of it: `--bounce-memory 4096`.
    u32 bounce_memory = 0;

    // R9f, in two halves, with a control arm each because they are two rules and either could be
    // the one that pays.
    //
    //   `coarse_keep`   the store gives up a coarse stand-in only under pressure, so the coarse
    //                   pyramid outlives the fine faces under it. `--no-coarse-keep`.
    //   `coarse_bounce` a gathering ray that lands on a surface with no light of its own reads the
    //                   coarse face standing over it rather than nothing. `--no-coarse-bounce`.
    //
    // The first is what makes walking out of a room and back find it lit; the second is what makes
    // light arrive from the half of the room the store has given up on. Neither is worth anything
    // without a face to read, which is why the first is the one to switch off when measuring the
    // second's cost.
    bool coarse_keep = true;
    bool coarse_bounce = true;
    // R5a: a face's far field and bounce are blended with its coplanar neighbours' before the
    // composite reads them. `--no-face-denoise` is the control arm, and it leaves the write in place
    // and takes the eight neighbour lookups out, so the two arms differ by the filter rather than by
    // a branch in the reader.
    bool face_denoise = true;
    // R4a: a face works out what the surface under it is MADE of and keeps the answer.
    // `--no-face-materials` is the control arm and no face ever asks, which is the renderer exactly
    // as it was before R4. It has to exist because the picture is identical in both arms by
    // construction -- nothing shades with this yet -- so the only evidence about what it costs is a
    // timing, and a timing wants two flags of one build rather than two builds (D407).
    bool face_materials = true;
    // R4c: a face keeps a block of outgoing bins and the composite reads the one the eye is looking
    // down. `--no-face-lobe` is the control arm and no face holds a block, so a metal is drawn with
    // its lobe standing in as the hemispherical mean it already stores -- which is the picture
    // before this stage. The sun's highlight is arithmetic and is drawn in both arms, so the two
    // differ by exactly the bins and by nothing else.
    bool face_lobe = true;
    // How much a face's lobe has to be worth before it asks the pool for one, negative meaning the
    // shader's own figure. A CAPACITY dial and not a material one -- see kLobeWorthFloor in
    // shaders/face_terms.glsl, and note that nought means every face with a material asks.
    f32 lobe_floor = -1.0f;
    // R4b: a face that holds a lobe casts a ray of its own, aimed into the cone each bin gathers
    // from. `--no-lobe-ray` is its control arm and leaves the bins to be filled by the far ray
    // alone, which is what R4c landed with and what D592 measured as unable to carry a reflection.
    bool lobe_ray = true;
    // R4b's second size class: a face whose material is sharper than thirty-six bins can hold gets
    // four blocks and a hundred and forty-four.
    //
    // **OFF by default, and that is a measurement rather than caution** (D599). Four times the bins
    // is four times the rays for the same noise, and the ray budget will not pay it: at eight
    // samples a bin the great door speckles outright, and at twenty-four -- the same rate the cheap
    // class uses, so four times the rays -- it still speckles, because twenty-four samples of a
    // RADIANCE is what it is however many bins there are. `--lobe-coverage` turns it on for
    // whoever comes back to it with a bigger budget; the machinery, the sizes and the audit line
    // are all built and priced.
    bool lobe_coverage = false;
    // R4d: a light ray carries on through glass and water instead of stopping dead on them, so a
    // room behind a window is lit through it. `--no-see-through` is the control arm and a window
    // blocks the sun exactly as a wall does, which is what this renderer has always done.
    bool see_through = true;
    // R9f's fold: a coarse face's sky and bounce are the average of the four faces under it rather
    // than its own rays at its own scale.
    //
    // **OFF by default and it is a trade rather than a failure** (D590). It is worth a third more
    // light in the bounce -- gathering rays landing on a lit face go **31.0% to 41.8%** -- and it
    // costs the faces pass **3.7 to 6.5 ms** at the close camera standing still, against a 4.40 ms
    // budget. Three ways of making it cheaper were built and measured and none worked. `--face-fold`
    // turns it on.
    bool face_fold = false;
    // R9c, the halo: the primary pass claims faces over a frustum widened by however far the camera
    // will have turned in `halo_lead` frames, so a face has started measuring before it arrives.
    //
    // **OFF by default, on a measurement rather than on caution** (D586). It works and it costs no
    // frame time -- but it claims faces into the store, the SUN's ray budget is divided by how many
    // the store holds, and the stride went 6 to 7 in both arms of an interleaved pair. That is 17%
    // fewer sun samples for every face on screen, bought for about a quarter of one edge's ambient
    // deficit, and it is invisible in a pass table: D527, D557 and now this are the same fault
    // three times. `--halo` turns it on and the margin is otherwise nought, which is a dispatch
    // exactly the size of the screen and no second code path anywhere.
    bool halo = false;
    // R9g: each chunk's emissive cells are kept between rebuilds and only what an edit touched is
    // rescanned. False rediscovers the whole world every time, which is D587's control arm.
    bool emitter_cache = true;
    // How many frames of head start to aim for. The ambient burst is kSkyBurst a frame, so this
    // times sixteen is roughly how many samples a face gains before it comes on screen -- and the
    // deficit it is closing is 112 samples against 707 (D585). Larger is a wider margin and more
    // faces measuring at once; the cost is the peak while turning, never the total, because these
    // are the same faces that were about to be measured anyway.
    u32 halo_lead = 24;
    // R6a's light meter. False zeroes both of the meter's slots every frame, which is the shader's
    // own "nothing has been measured" path and applies the constant `kPreviewExposure` this pass
    // used before the meter existed -- so `--no-auto-exposure` is the picture every figure in the
    // decision log above R6 was measured against, exactly, and not an approximation of it.
    bool auto_exposure = true;
    // The ceiling the meter may not expose past, as a multiplier. 0 keeps the shader's own figure.
    //
    // A light meter makes every scene average to the same grey, so without a ceiling there is no
    // such thing as a dark room: a sealed unlit corner measured **429x** and read as a lit-looking
    // picture of a room with no light in it. That is D541-D543's deleted light floor arriving
    // through the exposure rather than through the light. `--exposure-max N`.
    f32 exposure_max = 0.0f;
    // How hard R5a's filter weighs a neighbour against what this face already holds. Negative keeps
    // the shader's own figure and **0 is the control arm**: no agreement test at all, which is the
    // filter exactly as it was when the 3x3 speckle was reported. `--denoise-edge N`.
    f32 denoise_edge = -1.0f;
    // The store gives its three kinds of record up in an order — the off-screen class first, the
    // on-screen set's history next, the coarse pyramid last. `--no-class-eviction` puts them all
    // back on one clock, and with `--secondary-share 4` it is the full control arm for the class
    // being bounded by the table's spare room rather than by a fixed quarter.
    bool class_eviction = true;
    // The gathering ray's own counters, printed at the screenshot audit. See kLightProbeWords in
    // shaders/node.glsl for the word map. `--no-light-probe` is the arm that costs nothing.
    bool light_probe = true;
    // Wall-clock deadline for a scripted run, in seconds. A frame count cannot bound a run whose
    // frames are the thing that got slow, so every scripted run has one whether it asked or not:
    // this is filled in from kDefaultMaxSeconds after parsing when a screenshot, a tick audit or a
    // benchmark was asked for. `--max-seconds 0` is the way to say "no deadline", and it has to be
    // said out loud.
    //
    // Note what that sentence does NOT cover: a run that asked for no scripted mode at all, which
    // is the game, which is supposed to stay open. So a scripted run whose mode was misspelt is a
    // game window with no deadline, and it will sit there for ever. That is exactly how `test.bat`
    // spent months not running its third stage (D605).
    //
    // It is not a safety net, it is the reporting path. A build that makes the renderer ten times
    // slower is exactly the one whose measurement matters most, and it is the one that used to
    // hang until somebody closed the window - five times in this rewrite, each time destroying the
    // measurement that would have said so (D355, D357).
    f64 max_seconds = -1.0;   // -1 means "not asked for"; resolved below

    // "x,y,z,yaw,pitch" in metres and degrees. Lets a measurement be repeated exactly,
    // which is what makes frame times comparable between builds.
    std::string camera;

    // "vx,vy,vz,vyaw" in metres and degrees per second, applied every frame.
    //
    // A still camera is the one case the path tracer is never judged in. It accumulates
    // hundreds of samples per pixel and every measurement taken that way says the picture is
    // clean, while the picture a player sees is one sample deep because moving resets the
    // accumulator every frame. Every rendering conclusion drawn from a static screenshot has
    // been about an image nobody ever looks at.
    std::string fly;
    // radius,height,degrees a second,how far in and out. Circles the origin looking at it, so the
    // subject stays in frame for the whole run ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which a constant velocity cannot do, and a
    // benchmark that flies past the building in the first second measures empty sky.
    std::string orbit;

    // frame,x,y,z,yaw,pitch ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the camera jumps, once, at that measured frame.
    //
    // The instrument for anything that has to catch up with the view: light, streaming, residency.
    // Every other camera here moves smoothly, and smooth motion reveals a sliver of new world per
    // frame, so it measures the *rate* a system converges at while hiding what it does when handed
    // a whole screen at once. A cut is the worst case and it is also the ordinary one ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â turning
    // round in a doorway is a cut as far as the face store is concerned.
    //
    // REPEATABLE, and the second cut is a different measurement from the first. One cut asks what
    // ARRIVING somewhere costs. Two ask what LEAVING costs: stand at A, cut to B for longer than the
    // face store's cold window, cut back to A, and diff against a run that never left. That is
    // "walking out of a room and back", which is R9f's own gate wording, and one cut cannot express
    // it -- which is why the case a player reported as the world relighting itself had never been
    // measured.
    //
    // Counted in MEASURED frames, so with --settle it fires after the world has stopped building
    // and the only thing missing from the new view is the thing under test. Each cut carries its own
    // absolute frame rather than a delay since the last one, so the flags read in the order they
    // happen and a run's cuts can be reordered without rewriting their numbers.
    std::vector<std::string> cuts;

    // Scripted chisel, for checking the tool without a person holding the mouse.
    // --edit "x0,y0,z0,x1,y1,z1,material" applies one edit through the history at startup;
    // material 0 carves. --preview takes the same six numbers plus a state (1 carve,
    // 2 place, 3 refused) and forces the preview box on. Both are in voxels.
    std::string edit;
    std::string preview;

    // Constraint points, "x,y,z" in voxels, repeatable up to kMaxPreviewMarks.
    //
    // Here for the same reason --preview is: the marks are drawn as crosses in the material's
    // colour and there is otherwise no way to put one on screen without a hand on the keyboard,
    // which makes the one preview element that cannot be checked from a screenshot. A shape
    // nobody can photograph is a shape nobody notices has stopped being drawn.
    std::vector<std::string> preview_marks;
    u32 material = 0;   // which entry of the chisel's palette starts selected

    // Scripted clipboard: --clip "x0,y0,z0,x1,y1,z1,dx,dy,dz,copies,turn" captures that box
    // at frame 100, offsets the ghost by (dx,dy,dz), fans out `copies`, and turns it `turn`
    // degrees about y. Everything after the box is optional.
    std::string clip;

    // Open this world at once, without the title. A path to a `.wsworld` in the player's own
    // library, or to a clip: the shell's *open* fills this in, and so does a shortcut.
    std::string world;
    // Skip the title even when nothing else would. The inverse of a scripted run asking for it.
    bool no_title = false;

    // Photograph the title after `title_frames` frames of it, then exit.
    //
    // The instrument the shell would otherwise not have. Every other measurement in this file is
    // taken by a flag that walks PAST the title, so without this the one screen the game opens on
    // is the one screen no automated run ever looks at — and documentation/14-ui-style.md's own
    // rule about the tool previews applies to it exactly: a shape nobody can photograph is a shape
    // nobody notices has stopped being drawn. `--title-open worlds|settings` opens a window first,
    // so the docked panels are photographable too.
    std::string title_shot;
    u64 title_frames = 30;
    std::string title_open;

    // `--icon-sheet`: draw the whole icon vocabulary instead of the title, every drawing at four
    // sizes and across five steps of its own animation, and photograph THAT.
    //
    // The same argument `--title-shot` is made of, one level down. An icon is only ever on screen
    // where some window happens to put it, so most of the twenty-four were drawn by nothing any
    // automated run looked at, and the smallest size — sixteen device pixels, which is what a
    // 1366x768 desktop gets — was the size nobody ever checked them at. That is exactly where a
    // drawing stops being legible, and it is not a thing an argument can settle: it has to be
    // looked at. Use with `--title-shot`.
    bool icon_sheet = false;

    // `--shelf clips`: which shelf the library window shows. Every shelf but *worlds* was reachable
    // only by clicking, so a report about one of them could only be answered by clicking too.
    std::string shelf;

    // Which combination the logo draws, fixed. 0 is "whatever the seed the shell chose says", which
    // is different on every launch on purpose (src/ui/logo.hpp) — and a photograph of a thing that
    // is different every time cannot be compared with the last one, so a photograph of the mark
    // needs this. It is also the only way to look at a particular one of the four billion on purpose.
    u32 logo_seed = 0;
    // And ask for another combination on this title frame, so the morph BETWEEN two of them is
    // photographable too. Both seeds are derived from the pinned one, so the transition is the same
    // twice; `--title-frames` a little after it is a picture of the change in progress.
    u64 logo_change = 0;

    // Open a world, play it for N frames, LEAVE it, and show the title again before exiting.
    //
    // The one path D458 exists for and the one path nothing exercised: a world torn down while the
    // window, the card and the interface carry on. The first thing it found was a crash — the
    // developer HUD's SDL event hook outlived the ImGui context it points into, so the first mouse
    // move on the title afterwards read address 8. That is a fault no unit test can reach and no
    // screenshot can show, and it happened on the very first attempt at leaving a world.
    u64 cycle = 0;

    // Whether this run is SCRIPTED, and therefore walks straight past the title
    // (documentation/23-shell-and-libraries.md §0).
    //
    // This is not a convenience. Every measurement in this project is taken by one of these flags,
    // and a menu that a harness has to click through would end measurement here. The list is
    // exactly the one that document names, plus the two that imply a fixed subject.
    bool scripted() const {
        return no_title || cycle > 0 || !title_shot.empty() || !screenshot.empty() || settle ||
               !camera.empty() || !fly.empty() ||
               !orbit.empty() || !cuts.empty() || chisel_every > 0 || ticks > 0 ||
               !crash_test.empty() || benchmark || !edit.empty() ||
               !preview.empty() || !clip.empty() || !clip_file.empty() || max_seconds > 0.0;
    }

    // Air that is not empty: --fog "extinction,albedo,g,scale-height,base".
    //
    // Authored per METRE and in metres, because that is the unit a person thinks in; the
    // shader wants per voxel, and the conversion happens once here against whatever resolution
    // the clip was built at rather than in every place a length appears. See pt_media.glsl.
    //
    // A flag rather than a clip statement, and that is a decision worth stating: fog is a
    // property of the weather and not of the building, so a clip that carried one would make a
    // beam through an oculus part of the architecture. When there is weather it belongs there
    // and not here.
    std::string fog;
};

// Reads up to `count` comma-separated numbers. Missing ones keep their defaults, so a
// short string is a partial override rather than an error.
void parse_numbers(const std::string& text, i64* out, u32 count) {
    const char* cursor = text.c_str();
    for (u32 i = 0; i < count && *cursor != '\0'; ++i) {
        out[i] = std::strtoll(cursor, const_cast<char**>(&cursor), 10);
        if (*cursor == ',') ++cursor;
    }
}

// The same, for the numbers that are not whole ones.
void parse_reals(const std::string& text, f64* out, u32 count) {
    const char* cursor = text.c_str();
    for (u32 i = 0; i < count && *cursor != '\0'; ++i) {
        out[i] = std::strtod(cursor, const_cast<char**>(&cursor));
        if (*cursor == ',') ++cursor;
    }
}

// Frame parameters live in a uniform buffer rather than push constants; see
// gpu/render_params.hpp for why.

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_number = [&](u64 fallback) -> u64 {
            if (i + 1 < argc) return std::strtoull(argv[++i], nullptr, 10);
            return fallback;
        };

        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--world") {
            if (i + 1 < argc) options.world = argv[++i];
        } else if (arg == "--no-title") {
            options.no_title = true;
        } else if (arg == "--title-shot") {
            if (i + 1 < argc) options.title_shot = argv[++i];
        } else if (arg == "--title-frames") {
            options.title_frames = next_number(options.title_frames);
        } else if (arg == "--title-open") {
            if (i + 1 < argc) options.title_open = argv[++i];
        } else if (arg == "--icon-sheet") {
            options.icon_sheet = true;
        } else if (arg == "--shelf") {
            if (i + 1 < argc) options.shelf = argv[++i];
        } else if (arg == "--logo-seed") {
            options.logo_seed = static_cast<u32>(next_number(options.logo_seed));
        } else if (arg == "--logo-change") {
            options.logo_change = next_number(60);
        } else if (arg == "--cycle") {
            options.cycle = next_number(120);
        } else if (arg == "--ticks") {
            options.ticks = next_number(0);
            options.headless = true;
        } else if (arg == "--width") {
            options.width = static_cast<u32>(next_number(options.width));
            options.size_explicit = true;
        } else if (arg == "--height") {
            options.height = static_cast<u32>(next_number(options.height));
            options.size_explicit = true;
        } else if (arg == "--cam") {
            if (i + 1 < argc) options.camera = argv[++i];
        } else if (arg == "--orbit") {
            if (i + 1 < argc) options.orbit = argv[++i];
        } else if (arg == "--fly") {
            if (i + 1 < argc) options.fly = argv[++i];
        } else if (arg == "--cut") {
            if (i + 1 < argc) options.cuts.push_back(argv[++i]);
        } else if (arg == "--edit") {
            if (i + 1 < argc) options.edit = argv[++i];
        } else if (arg == "--preview") {
            if (i + 1 < argc) options.preview = argv[++i];
        } else if (arg == "--preview-mark") {
            if (i + 1 < argc) options.preview_marks.push_back(argv[++i]);
        } else if (arg == "--clip") {
            if (i + 1 < argc) options.clip = argv[++i];
        } else if (arg == "--fog") {
            if (i + 1 < argc) options.fog = argv[++i];
        } else if (arg == "--clip-file") {
            if (i + 1 < argc) options.clip_file = argv[++i];
        } else if (arg == "--clip-slice") {
            if (i + 1 < argc) options.clip_slices.push_back(argv[++i]);
        } else if (arg == "--clip-symmetry") {
            options.clip_symmetry = true;
        } else if (arg == "--clip-align") {
            options.clip_align = true;
        } else if (arg == "--clip-metre") {
            options.clip_metre = static_cast<i32>(next_number(0));
        } else if (arg == "--no-despeckle") {
            // The control arm for D610. Two flags of one build: with it, every lone voxel of the
            // wrong material stays exactly where the sampler put it.
            options.despeckle = false;
        } else if (arg == "--no-clip-cache") {
            options.no_clip_cache = true;
        } else if (arg == "--no-paste-pool") {
            options.no_paste_pool = true;
        } else if (arg == "--clip-part") {
            if (i + 1 < argc) options.clip_part = argv[++i];
        } else if (arg == "--clip-bounds") {
            if (i + 1 < argc) options.clip_bounds = argv[++i];
        } else if (arg == "--clip-at") {
            if (i + 1 < argc) parse_numbers(argv[++i], options.clip_at, 3);
        } else if (arg == "--material") {
            options.material = static_cast<u32>(next_number(0));
        } else if (arg == "--debug-mode") {
            options.debug_mode = static_cast<u32>(next_number(0));
        } else if (arg == "--screenshot") {
            if (i + 1 < argc) options.screenshot = argv[++i];
        } else if (arg == "--screenshot-frame") {
            options.screenshot_frame = next_number(30);
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--target-fps" && i + 1 < argc) {
            options.target_fps = static_cast<f32>(std::atof(argv[++i]));
        } else if (arg == "--max-seconds" && i + 1 < argc) {
            options.max_seconds = std::atof(argv[++i]);
        } else if (arg == "--undo-frame" && i + 1 < argc) {
            options.undo_frame = static_cast<u64>(std::atoll(argv[++i]));
        } else if (arg == "--redo-frame" && i + 1 < argc) {
            options.redo_frame = static_cast<u64>(std::atoll(argv[++i]));
        } else if (arg == "--edit-frame" && i + 1 < argc) {
            options.edit_frame = static_cast<u64>(std::atoll(argv[++i]));
        } else if (arg == "--face-gate" && i + 1 < argc) {
            options.face_gate = static_cast<u32>(std::atoll(argv[++i]));
        } else if (arg == "--no-face-gate") {
            options.face_gate = 0x7FFFFFFFu;
        } else if (arg == "--no-face-worklist") {
            options.face_worklist = false;
        } else if (arg == "--no-face-prolong") {
            options.face_prolong = false;
        } else if (arg == "--no-node-crossings") {
            options.node_crossings = false;
        } else if (arg == "--no-light-keeps-geometry") {
            options.light_read_period = 0;
        } else if (arg == "--light-read-period" && i + 1 < argc) {
            // Rounded UP to a power of two, because the shader tests a mask. Saying so beats a
            // silent floor: `-Extra "--light-read-period 24"` would otherwise measure 16 and be
            // written down as 24.
            const u32 asked = static_cast<u32>(std::atoll(argv[++i]));
            u32 period = 1;
            while (period < asked) period <<= 1;
            options.light_read_period = (asked == 0) ? 0 : period;
            if (options.light_read_period != asked) {
                WS_LOG_WARN("app", "--light-read-period {} rounded up to {}, which is what the "
                                   "shader's mask can express",
                            asked, options.light_read_period);
            }
        } else if (arg == "--face-edit-seed" && i + 1 < argc) {
            options.face_edit_seed = static_cast<u32>(std::atoll(argv[++i]));
        } else if (arg == "--no-lamp-edit-scope") {
            options.lamp_edit_scope = false;
        } else if (arg == "--chisel" && i + 1 < argc) {
            // EVERY[,RADIUS] -- carve and fill in front of the camera, for ever.
            i64 values[2]{0, 16};
            parse_numbers(argv[++i], values, 2);
            options.chisel_every = static_cast<u64>(values[0] > 0 ? values[0] : 0);
            options.chisel_radius = values[1] > 0 ? values[1] : 16;
        } else if (arg == "--benchmark") {
            options.benchmark = true;
        } else if (arg == "--no-auto-quality") {
            options.no_auto_quality = true;
        } else if (arg == "--quality" && i + 1 < argc) {
            options.quality_level = std::atoi(argv[++i]);
        } else if (arg == "--face-budget" && i + 1 < argc) {
            // How many faces the store may hold. Here so the full-table path can be reached from
            // one camera in one run: at the real budget it takes a player moving about for a
            // while, which is precisely why "the shadowed faces stop being produced" was found by
            // playing rather than by any test.
            options.face_budget = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--face-read-period" && i + 1 < argc) {
            // A power of two, or 0 for off. Not rounded here: a figure that is silently changed is
            // a figure an A/B cannot be read against.
            options.face_read_period = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--no-face-reads") {
            options.face_read_period = 0;   // D508's control arm
        } else if (arg == "--secondary-period" && i + 1 < argc) {
            // A power of two, or 0 for off. R9a's dial: how fast the off-screen set fills in,
            // against how much of the feedback buffer it takes to fill it.
            options.secondary_period = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--no-secondary-faces") {
            options.secondary_period = 0;   // R9a's control arm
        } else if (arg == "--secondary-share" && i + 1 < argc) {
            // A hard ceiling on the off-screen class, as a divisor of the table. The default is
            // none, and the class is bounded by what the table has spare; 4 is the fixed quarter
            // this used to be and is the control arm for that change.
            options.secondary_share = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--secondary-light-share" && i + 1 < argc) {
            // R9b's ray share: how much of the on-screen set's shading rate the off-screen set gets,
            // as a divisor. Larger is cheaper and slower. Not rounded and not clamped here, so the
            // figure an A/B is read against is the figure that was asked for.
            options.secondary_light_share = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--no-secondary-light") {
            // R9b's control arm: a face nobody is looking at casts nothing, whatever is reading it.
            // This is the state every figure taken before this change was measured in.
            options.secondary_light_share = 0;
        } else if (arg == "--denoise-edge" && i + 1 < argc) {
            // R5a's agreement test. 0 is the control arm and restores the filter that smeared each
            // lit face into a ring of eight that should have stayed dark.
            options.denoise_edge = static_cast<f32>(std::atof(argv[++i]));
        } else if (arg == "--exposure-max" && i + 1 < argc) {
            // How far the meter may lift a dark scene. Larger recovers more of the dark and lets a
            // room with no light in it read as lit; smaller crushes more of it. Measured against
            // `clips/exposure_range.clip`, which legitimately needs 33x.
            options.exposure_max = static_cast<f32>(std::atof(argv[++i]));
        } else if (arg == "--no-auto-exposure") {
            // R6a's control arm: the fixed 3.2 this pass applied for the whole of the rewrite.
            options.auto_exposure = false;
        } else if (arg == "--no-face-denoise") {
            // R5a's control arm. Nothing in this renderer filtered across faces before it, so this
            // is the state every figure taken before R5 was measured in.
            options.face_denoise = false;
        } else if (arg == "--no-emitter-cache") {
            // R9g's control arm: rediscover every chunk's emitters on every announced change.
            options.emitter_cache = false;
        } else if (arg == "--halo") {
            // R9c on: claim a margin past the screen, sized by how fast the camera is turning.
            options.halo = true;
        } else if (arg == "--no-halo") {
            options.halo = false;   // the default, and the state every figure before R9c was in
        } else if (arg == "--halo-lead" && i + 1 < argc) {
            options.halo_lead = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--face-fold") {
            // R9f on: a coarse face takes its sky and its bounce from the four faces under it.
            options.face_fold = true;
        } else if (arg == "--no-face-fold") {
            options.face_fold = false;   // the default, and a coarse face measures itself
        } else if (arg == "--no-face-materials") {
            // R4a's control arm: no face asks what it is made of, so the descent, the two table
            // reads and the load that finds out all go. The picture is the same in both arms.
            options.face_materials = false;
        } else if (arg == "--no-see-through") {
            // R4d's control arm: transmissive matter stops a light ray dead, as it always has.
            options.see_through = false;
        } else if (arg == "--lobe-coverage") {
            // R4b's second size class on. Off by default -- see the option for the measurement.
            options.lobe_coverage = true;
        } else if (arg == "--no-lobe-coverage") {
            options.lobe_coverage = false;   // the default, and every lobe is the cheap class
        } else if (arg == "--no-lobe-ray") {
            // R4b's control arm: the pool, the bins and the energy split all stay, and only the
            // march goes -- so an A/B prices the ray and nothing else.
            options.lobe_ray = false;
        } else if (arg == "--no-face-lobe") {
            // R4c's control arm: no face holds a block of outgoing bins and no pixel probes for
            // one, so a metal reflects its own hemispherical mean in every direction at once.
            options.face_lobe = false;
        } else if (arg == "--lobe-floor" && i + 1 < argc) {
            // The worth itself, in [0, 1], so the dial and `face_lobe_worth` speak the same units
            // and a figure from the census can be typed in without being converted. The gaps that
            // matter are narrow -- glass and water sit at 0.040 and marble at 0.036 -- and a dial
            // that could not tell those apart could not answer the question it exists for.
            // `--lobe-floor 0` gives a block to every face that knows what it is made of, which is
            // the arm that says what the pool's size is costing.
            options.lobe_floor = static_cast<f32>(std::atof(argv[++i]));
        } else if (arg == "--no-class-eviction") {
            // Every cold record on one clock again, whoever asked for it, and the coarse pyramid
            // spent at the first step of the squeeze. Pair it with `--secondary-share 4` for the
            // whole control arm.
            options.class_eviction = false;
        } else if (arg == "--no-coarse-keep") {
            // R9f's first control arm: the store gives a coarse stand-in up on the same clock as
            // any other face, which is what it did before. Two flags of one build, as D407
            // requires -- and this pair has to be separable from the one below, because they are
            // two rules that happen to arrive together: one is about what the store KEEPS and one
            // about what a gathering ray READS.
            options.coarse_keep = false;
        } else if (arg == "--no-coarse-bounce") {
            options.coarse_bounce = false;   // R9f's second control arm
        } else if (arg == "--no-light-probe") {
            // The instrument itself, off. It is on by default because a counter nobody switches on
            // is a counter nobody reads (D510), and switchable because an instrument whose cost is
            // unmeasured is trap 20 waiting to happen.
            options.light_probe = false;
        } else if (arg == "--no-bounce") {
            options.bounce = false;   // R9's control arm: kIndirectFloor everywhere again
        } else if (arg == "--bounce-min" && i + 1 < argc) {
            options.bounce_min = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--bounce-memory" && i + 1 < argc) {
            options.bounce_memory = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--face-pressure-from" && i + 1 < argc) {
            options.face_pressure_from = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--whole-set-retry") {
            // The control arm for D544: an upload that runs out of staging clears nothing and
            // resends the whole dirty set next frame, which is what it did before. Two flags of one
            // build, as D407 requires.
            options.whole_set_retry = true;
        } else if (arg == "--no-face-pressure") {
            // The control arm for D502: the store waits until it is FULL before giving anything up,
            // which is what it did before. Two flags of one build, as D407 requires.
            options.face_pressure = false;
        } else if (arg == "--crash-test" && i + 1 < argc) {
            options.crash_test = argv[++i];
        } else if (arg == "--hollow" && i + 1 < argc) {
            options.hollow = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--clip-coarse" && i + 1 < argc) {
            options.clip_coarse = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--settle") {
            options.settle = true;
        } else if (arg == "--no-update-check") {
            options.no_update_check = true;
        } else if (arg == "--version") {
            std::printf("WorldShaper %s\n", kVersion);
            options.help = true;
        } else if (arg == "--no-validation") {
            options.validation = false;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            WS_LOG_WARN("app", "unknown argument '{}'", arg);
        }
    }

    // Every scripted run ends on the clock. See Options::max_seconds: a run bounded only by a
    // frame count cannot report the one thing it most needs to - that the frames got slow.
    if (options.max_seconds < 0.0) {
        const bool scripted =
            !options.screenshot.empty() || options.ticks > 0 || options.benchmark;
        options.max_seconds = scripted ? kDefaultMaxSeconds : 0.0;
    }
    return options;
}

void print_help() {
    std::puts(
        "WorldShaper\n"
        "  --headless            run with no window or GPU\n"
        "  --world FILE          open this world at once, skipping the title\n"
        "  --no-title            skip the title and open whatever would have been opened\n"
        "  --title-shot FILE     photograph the title after --title-frames N and exit\n"
        "  --title-open WHICH    open a window on it first: worlds, settings, or both\n"
        "  --logo-seed N         draw the mark from this seed rather than a new one, so a\n"
        "                        photograph of the title is comparable with the last one\n"
        "  --logo-change N       ask the mark for another combination on title frame N, so the\n"
        "                        morph between two of them can be photographed\n"
        "  --cycle N             play N frames, LEAVE the world, show the title again, exit.\n"
        "                        The tear-down path, walked without a hand on the keyboard\n"
        "  --ticks N             headless world audit over N ops, then exit\n"
        "  --width N --height N  window size (default 1600x900)\n"
        "  --no-vsync            uncapped presentation\n"
        "  --validation          force Vulkan validation layers on\n"
        "  --no-validation       force them off\n"
        "  --cam x,y,z,yaw,pitch scripted camera (metres, degrees)\n"
        "  --screenshot FILE     save frame --screenshot-frame N and exit\n"
        "  --debug-mode N        0 shaded, 1 steps, 2 normals, 3 detail, 4 clip ghost,\n"
        "                        5 face cache, 6 why a path-traced pixel is dark,\n"
        "                        7 what the primary ray hit, 8 what the bounce found,\n"
        "                        9 the sun's visibility on its own, 10 no tone curve,\n"
        "                        11 this pixel's face key as four exact bytes, for counting\n"
        "                        distinct visible faces (tools\\facecount.ps1),\n"
        "                        16 the sun term alone, 17 sky visibility, 18 the near field,\n"
        "                        19 how far each face is through its ambient rays: green\n"
        "                        converged and silent, red held short of it by unbuilt\n"
        "                        geometry, grey the progress between,\n"
        "                        20 the lamp term alone: what the emitters deliver to each\n"
        "                        face, with blue for a face that has not measured yet\n"
        "  --settle              start the measurement window once the world stops sharpening,\n"
        "                        rather than at frame nought. Any figure to be compared with\n"
        "                        another run needs this\n"
        "  --preview x0,..,z1,s  force a preview box on: six voxel coordinates then a state\n"
        "                        (1 carve, 2 place, 3 refused, 6 the cursor marker)\n"
        "  --preview-mark x,y,z  drop a constraint cross, repeatable\n"
        "  --undo-frame N        press undo once on frame N; --redo-frame N the same for redo.\n"
        "                        Raw frames, like --edit-frame, and through the same code the\n"
        "                        key takes\n"
        "  --face-edit-seed N    samples a face keeps when an edit says the world under it moved\n"
        "                        (default 8). 0 restores the wipe, which is the control arm for\n"
        "                        the blocky flashing an edit used to cause\n"
        "  --no-lamp-edit-scope  an edit reopens the lamp term of every face within sixteen metres\n"
        "                        again, rather than only those it can stand in the light of. The\n"
        "                        control arm for the flicker while building\n"
        "  --no-auto-exposure    the fixed brightness multiplier of 3.2 this pass applied before\n"
        "                        the light meter existed. R6a's control arm\n"
        "  --exposure-max N      how far the light meter may lift a dark scene, as a multiplier\n"
        "                        (default 64). Smaller lets darkness stay dark; larger lifts a\n"
        "                        room with almost no light in it until it reads as lit\n"
        "  --no-face-denoise     a face's far field and bounce are read raw rather than blended\n"
        "                        with its coplanar neighbours'. R5a's control arm\n"
        "  --no-face-materials   no face works out what the surface under it is made of. R4a's\n"
        "                        control arm, and the picture is identical in both\n"
        "  --halo                claim faces past the edge of the screen, over a margin sized by\n"
        "                        how fast the camera is turning, so they are measuring before they\n"
        "                        arrive. Off by default: it costs the sun's refresh rate (D586)\n"
        "  --halo-lead N         how many frames of head start to aim for (24)\n"
        "  --no-class-eviction   the store gives every cold record up on one clock again, whoever\n"
        "                        asked for it, and spends the coarse pyramid at the first step of\n"
        "                        the squeeze. Pair with --secondary-share 4 for the whole control\n"
        "                        arm of the off-screen class growing into the table's spare room\n"
        "  --no-coarse-keep      the store gives a coarse stand-in up on the same clock as any\n"
        "                        other face, so the light of a room is gone ten seconds after you\n"
        "                        leave it. R9f's first control arm\n"
        "  --no-coarse-bounce    a gathering ray that lands on a surface with no light of its own\n"
        "                        reads nothing rather than the coarse face over it. The second\n"
        "  --no-light-probe      stop counting what gathering rays land on. The counters are\n"
        "                        printed at every screenshot and this is what costs nothing\n"
        "  --max-seconds N       wall-clock deadline for a scripted run. Every run that ends by\n"
        "                        itself has one (default 180 s): it reports where it got to\n"
        "                        rather than running until somebody closes it. 0 for none\n"
        "  --target-fps N        frame rate to hold (default: the monitor's refresh rate)\n"
        "  --quality N           pin the quality level 0-7 instead of deciding it\n"
        "  --no-auto-quality     leave quality where it is and never adjust it\n"
        "  --benchmark           measure this machine again and save the result\n"
        "  --fly vx,vy,vz,vyaw   move the camera every frame (m/s, deg/s), so a screenshot\n"
        "                        is of the moving picture rather than a settled one\n"
        "  --cut f,x,y,z,yaw,pitch  jump the camera at measured frame f. The worst case for\n"
        "                        anything that has to catch up with the view, and what turning\n"
        "                        round in a doorway looks like to the face store. REPEATABLE:\n"
        "                        two cuts are leaving somewhere and coming back to it\n"
        "  --crash-test KIND     prove crash reporting works: read, write, check, throw,\n"
        "                        divzero, frame (faults in-game), report (no crash)\n"
        "  --clip x0,..,z1,dx,dy,dz,copies,turn   scripted clipboard ghost\n"
        "  --clip-file FILE      build a clip from its file. Alone, it measures it and prints\n"
        "                        what it found; with --screenshot it stamps it in the world\n"
        "  --clip-slice a,at[,n] print a slice through it, one character per n voxels\n"
        "  --clip-symmetry       report how far it is from being mirror symmetric\n"
        "  --clip-align          report parts that nearly line up but do not\n"
        "  --clip-at x,y,z       where to stamp it, in voxels (default the origin)\n"
        "  --clip-metre N        sample at N voxels per metre instead of the file's\n"
        "  --edit x0,..,z1,mat   apply one chisel edit at startup (mat 0 carves)\n"
        "  --preview x0,..,z1,s  force the preview box on (s: 1 carve, 2 place, 3 refused)\n"
        "  --fog e,albedo,g,h,y  air that is not empty: extinction per metre, single-scatter\n"
        "                        albedo, Henyey-Greenstein g, scale height and base in metres\n"
        "\n"
        "In game:  F1 developer panel   F2 overlay   F5 reload shaders\n"
        "          F11 toggle vsync     Esc quit\n"
        "  chisel: hold LMB carve   RMB place   G+wheel distance   MMB constraint\n"
        "          Z undo   X redo   R clear points   C cancel   Q/E material\n"
        "          H+wheel hollow shell thickness (0 = solid)");
}

// Per-dispatch parameters for the path tracer. Push constants rather than another field on
// the shared block: the tracer is the only thing that reads them, and the shared block is read
// by two other shaders that would have to be kept in step for no reason.
// How much of a frame the shutter is open for.
//
// A half is the film convention ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a 180-degree shutter ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and it is what an eye raised on film
// expects. It is also the setting that does the job this was asked for: a frame rate that reads as
// choppy is one where each frame is a sharp, still picture and the eye is handed a series of
// unrelated stills. A streak between them is what tells the eye the two frames are the same scene
// moving, and that reads as fluid at a frame rate where the sharp version does not.
// How much faster the world's clock runs than the player's. One second at the keyboard is one
// minute in the world.
constexpr f32 kGameSecondsPerSecond = 60.0f;

// Faces the shading pass will trace a shadow ray for in one frame. A budget on the pass rather
// than on the store: everything visible keeps a face, and how often a settled one is refreshed is
// what gives. Measured at 0.185 ms for 19,196 faces, so this is aimed at rather under a
// millisecond against the pass's 4.4 ms budget, leaving room for the sky and lamp rays R3c adds
// to the same invocation.
constexpr u32 kFacesPerFrame = 96u * 1024u;

// Must match kSkyBurst and kSkyConverged in shaders/node.glsl. Nothing here meters the ambient
// burst -- three ways of doing that were built and measured and all three were slower than none;
// shade_faces.comp records the table. These are kept because the audit and the comments refer to
// them, and because a number that lives in two files should be declared in both.
constexpr u32 kSkyBurstMax = 32u;
constexpr u32 kSkyConverged = 2048u;

constexpr f32 kShutterFraction = 0.5f;

// The longest streak, in pixels. A bound on cost rather than on looks: a fast spin can put a
// point most of the way across the screen in a frame, and there is no picture in a streak that
// long ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â only taps.
constexpr f32 kLongestStreak = 24.0f;

// The sun's radiance at the reference hour, in one place because two passes evaluate the sky from
// it now: the composite, which draws it, and the face pass, whose gathering rays read it when they
// escape. Two copies would be indirect light lit by a sun the picture does not have.
//
// Chosen so a 0.5-albedo surface facing the sun lands near mid-grey; see make_trace_push.
constexpr f32 kSunColour[3] = {3.2f, 3.05f, 2.75f};

struct TracePush {
    f32 sun[4]{};          // xyz towards the sun, w cos of its angular radius
    f32 sun_colour[4]{};
    // x sample index, z frame, w world changed. y is spare: it used to carry a bounce limit,
    // which the shader never read and could not have used ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â see src/game/quality.hpp for why
    // this renderer has no such number.
    u32 control[4]{};
    u32 quality[4]{};      // x refine stride, y shadow sample target
    // Participating media. Per *voxel* of path and not per metre: the metre belongs to the
    // clip (`metre 32` at the top of one), so the conversion happens once here rather than in
    // every shader that has a length in it. fog_shape.z is an absolute world voxel height.
    f32 fog[4]{};          // xyz scattering coefficient, w extinction; both per voxel of path
    f32 fog_shape[4]{};    // x Henyey-Greenstein g, y height scale in voxels, z the world
                           //   height the coefficients are quoted at, w spare
};
// Ninety-six bytes, which is inside the 128 every Vulkan implementation is required to offer ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
// the same bound src/gpu/render_params.hpp records having already been walked into once.
static_assert(sizeof(TracePush) == 96, "TracePush must match the shader's push block");

// Where the compiled shaders are.
//
// Beside the running executable first, which is what makes an unzipped release work wherever
// it is put. Only if they are not there does it fall back to the directory this build was
// configured with ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â useful for a developer running the exe from somewhere odd, and useless to
// anyone else, since that path exists on exactly one machine.
//
// Using the build-time path alone is how v0.6.0 shipped broken: it resolved perfectly here
// and to nothing at all on every other computer, so the window opened black and closed with
// the reason scrolling past in a console nobody could read.
static std::filesystem::path compiled_shader_dir() {
    const std::string base = Window::base_path();
    if (!base.empty()) {
        const std::filesystem::path beside = std::filesystem::path(base) / "shaders";
        std::error_code ec;
        if (std::filesystem::exists(beside / "visibility.comp.spv", ec)) return beside;
    }
    return std::filesystem::path(WS_EXE_SHADER_DIR);
}

// Headless is a first-class mode from Stage 0 (documentation/02, "Headless mode").
// Conservation-of-matter audits, determinism checks and multiplayer desync tests all run
// through this path in CI, so it must never be allowed to rot.
//
// This is the Stage 1 exit check: apply a large random op stream to a real world, assert
// every invariant, audit the matter ledger against a full recount, round-trip the save,
// and report the memory numbers documentation/03 section 8 depends on.
int run_headless(const Options& options) {
    const u64 op_count = (options.ticks > 0) ? options.ticks : 100000;
    WS_LOG_INFO("app", "headless world audit: {} ops", op_count);

    JobSystem jobs;
    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World world;
    MatterLedger ledger;
    OpLog log;

    // A handful of materials sharing one behaviour but differing in colour, which is the
    // shape real terrain has and the one the interning design is tuned for.
    BehaviourRecord stone{};
    stone.material = 1;
    stone.tags.add(tags.find("stone"));
    stone.tags.add(tags.find("solid"));
    stone.properties.set(props::kDensity, PropertyValue::from_uint(2600));

    std::vector<VoxelTypeId> palette;
    for (u32 i = 0; i < 8; ++i) {
        VisualRecord visual{};
        visual.red = static_cast<u8>(90 + i * 8);
        visual.green = static_cast<u8>(88 + i * 7);
        visual.blue = static_cast<u8>(80 + i * 6);
        visual.opacity = 255;
        palette.push_back(types.intern(visual, stone));
    }

    const u64 build_start = now_ns();
    u64 voxels_written = 0;
    for (u64 step = 0; step < op_count; ++step) {
        // On the clock, like every other scripted run. See Options::max_seconds.
        if (options.max_seconds > 0.0 &&
            ns_to_ms(now_ns() - build_start) > options.max_seconds * 1000.0) {
            WS_LOG_WARN("audit", "deadline: {:.0f} s elapsed at op {} of {}; auditing what exists",
                        options.max_seconds, step, op_count);
            break;
        }
        const u64 h = hash_cell(static_cast<i64>(step), 17, 23, step, 0x57534831ull);
        const i64 x = static_cast<i64>(hash_range(h, 512)) - 256;
        const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 256)) - 128;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h + 2), 512)) - 256;
        const i64 size = static_cast<i64>(hash_range(hash_mix(h + 3), 4));
        const u32 roll = hash_range(hash_mix(h + 4), 100);
        const VoxelTypeId type =
            (roll < 25) ? kAir : palette[hash_range(hash_mix(h + 5), 8)];

        const Op op = Op::fill_box(step, static_cast<u32>(step % 4), x, y, z, x + size,
                                   y + size, z + size, type,
                                   type == kAir ? MatterReason::PlayerBreak
                                                : MatterReason::PlayerPlace);
        log.append(op);
        voxels_written += apply_op(world, op, ledger).voxels_changed;
    }
    const f64 build_ms = ns_to_ms(now_ns() - build_start);

    world.compact();

    bool ok = true;
    if (!world.validate()) {
        WS_LOG_ERROR("audit", "world invariants FAILED");
        ok = false;
    }
    if (!ledger.audit(world)) {
        WS_LOG_ERROR("audit", "matter ledger disagrees with a full recount: {} types differ",
                     ledger.audit_failures(world).size());
        ok = false;
    }

    // Save, load, save. The two byte streams must be identical.
    WorldSave source{&tags, &properties, &types, &world};
    ByteWriter first;
    write_save(first, source);

    TagRegistry loaded_tags;
    PropertyRegistry loaded_properties;
    VoxelTypeTable loaded_types;
    World loaded_world;
    WorldSave target{&loaded_tags, &loaded_properties, &loaded_types, &loaded_world};

    ByteReader reader(first.data());
    if (!read_save(reader, target)) {
        WS_LOG_ERROR("audit", "reloading the save FAILED");
        ok = false;
    }

    ByteWriter second;
    write_save(second, target);
    if (first.data() != second.data()) {
        WS_LOG_ERROR("audit", "save -> load -> save is NOT byte-identical ({} vs {} bytes)",
                     first.size(), second.size());
        ok = false;
    }
    if (loaded_world.content_hash() != world.content_hash()) {
        WS_LOG_ERROR("audit", "the reloaded world has a different content hash");
        ok = false;
    }

    const WorldStats stats = world.stats();
    const VoxelTypeStats type_stats = types.stats();

    WS_LOG_INFO("audit", "ops {}  voxels written {}  ({:.1f} M writes/s)", op_count,
                voxels_written,
                (build_ms > 0.0) ? static_cast<f64>(voxels_written) / build_ms / 1000.0 : 0.0);
    WS_LOG_INFO("audit", "chunks {}  bricks {}  solid voxels {}", stats.chunks, stats.bricks,
                stats.solid_voxels);
    // Note on the numbers: this stress world scatters eight materials at random, which is
    // the worst case for palette compression. Coherent terrain, which is what a real world
    // looks like, sits near the 0.4 bytes/voxel documentation/03 ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â§3 budgets for.
    WS_LOG_INFO("audit", "world memory {} KB  ({:.3f} bytes/voxel, {:.2f} per solid voxel)",
                stats.bytes / 1024, stats.bytes_per_voxel(), stats.bytes_per_solid_voxel());
    WS_LOG_INFO("audit", "voxel types {}  visuals {}  behaviours {}  dedup {:.4f}",
                type_stats.types, type_stats.visual_records, type_stats.behaviour_records,
                type_stats.dedup_rate());
    WS_LOG_INFO("audit", "save {} KB  world hash {:#018x}  op log hash {:#018x}",
                first.size() / 1024, world.content_hash(), log.rolling_hash());
    WS_LOG_INFO("audit", "job workers {}", jobs.worker_count());

    if (!ok) {
        WS_LOG_FATAL("audit", "FAILED");
        return 1;
    }
    WS_LOG_INFO("audit", "all invariants hold");
    return 0;
}

// The Stage 2 exit check, with no GPU involved: build the scripted test scene, fly a
// camera path over it, and assert after every frame that what the streamer holds is
// bit-identical to what the world holds. Eviction, re-upload and pool churn are all
// exercised because the budget is deliberately too small to hold the scene at once.
// Where the facility lives, looked for in the places it can be.
//
// Beside the compiled shaders in a shipped build, and up from the source shaders in a
// development one. A clip is content, so it is a file on disk rather than something compiled in
// ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is the whole point of the format, and it means the scene can be edited without a
// build.
// The version of how a clip becomes a world. Bump it when a change would make the same clip
// produce a different world, so every cached world built the old way is thrown away.
constexpr u64 kWorldBuildVersion = 1;

// The directories holding the code that decides what a clip builds into. Nothing else counts:
// the renderer, the HUD and the updater can all change without a single voxel moving.
const char* const kBuildInputDirs[] = {"src/forge", "src/world"};
const char* const kBuildInputFiles[] = {"src/game/clip.cpp", "src/game/clip.hpp"};

// What the cached worlds are keyed on, beyond the clip itself.
//
// This used to be the executable's modification time, which is correct and unusable: it throws
// away every built world on every relink, so changing a menu label costs a minute of resampling a
// building that nobody touched. What actually matters is whether the code that decides the build
// changed, so that is what is measured ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the newest modification time across the forge and the
// world format, folded together with the version above.
//
// In a shipped tree those sources are not present and the walk finds nothing, which leaves the
// version constant alone as the key. That is the right answer there: a released build's clips are
// built one way, and the way only changes when a release says it does.
u64 build_stamp() {
    u64 stamp = kWorldBuildVersion * 0x9E3779B97F4A7C15ull;

    const auto fold = [&stamp](const std::filesystem::path& file) {
        std::error_code error;
        const auto when = std::filesystem::last_write_time(file, error);
        if (error) return;
        const u64 at = static_cast<u64>(when.time_since_epoch().count());
        // A maximum, not a mix: reverting a file must give back the key it had before it was
        // touched, and a running total would not.
        stamp = at > stamp ? at : stamp;
    };

    const std::filesystem::path root = std::filesystem::path(WS_SHADER_SOURCE_DIR).parent_path();
    std::error_code error;
    for (const char* directory : kBuildInputDirs) {
        for (std::filesystem::directory_iterator it(root / directory, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) fold(it->path());
        }
        error.clear();
    }
    for (const char* file : kBuildInputFiles) fold(root / file);
    return stamp;
}

std::string default_clip_path() {
    const std::filesystem::path candidates[] = {
        std::filesystem::path("clips") / "facility.clip",
        compiled_shader_dir().parent_path() / "clips" / "facility.clip",
        std::filesystem::path(WS_SHADER_SOURCE_DIR).parent_path() / "clips" / "facility.clip",
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) return candidate.string();
    }
    return (std::filesystem::path("clips") / "facility.clip").string();
}

// The chunk-mirror streaming audit that stood here went with `world/residency.*` (R1e). What it
// checked -- that the CPU's copy of every resident chunk still hashes to what the world holds --
// is now `NodePool::stale_leaves` and `stale_masks`, which check the same property of the tree the
// renderer actually walks, run at every screenshot rather than in a mode of their own.
// One world, from the loading screen to the way out.
//
// Everything below the window is HERE, and nothing above it is: the device, the swapchain, the
// interface and its sound outlive a world and are passed in. That split is D441's, and it is what
// makes `02-architecture-overview.md`'s rule — **a world is torn down on the way out, never
// shared** — true by construction rather than by discipline. Opening a second world builds a
// second Application; every pool, every table and every counter in it is new, so two worlds cannot
// contaminate each other however badly either of them behaves.
//
// It is a large object and it is deliberately never on the stack: see `run_windowed`.
class Application {
public:
    Application(Window& window, Device& device, Swapchain& swapchain, GpuProfiler& profiler,
                ShellPass& shell_pass, ui::Shell& shell)
        : window_(window),
          device_(device),
          swapchain_(swapchain),
          profiler_(profiler),
          shell_pass_(shell_pass),
          shell_(shell) {}

    // Plays one world. Returns 0 when it ended normally, whether that was the window closing or
    // the player going back to the title; `wants_title` says which.
    int play(const Options& options);
    bool wants_title() const { return wants_title_; }
    // Set instead when the way out was *into another world*: the tear-down is the same, and this
    // is what the loop opens next instead of showing the title.
    const std::string& wants_world() const { return wants_world_; }

private:
    bool create_render_target(u32 width, u32 height);
    void destroy_render_target();
    // The window scaled by render_scale_, which is what the render targets are sized to.
    VkExtent2D scaled_extent() const;
    void handle_resize();
    void record_frame(f32 time_seconds);

    // The sun, the weather and the air, for whichever renderer is running.
    //
    // Both of them need it. It used to be built inside the path tracer's branch, which is a large
    // part of why none of the sky work reached the game: the pass that runs by default could not
    // see any of it, so it drew a hardcoded gradient with no sun, no cloud and no air in it.
    TracePush make_trace_push();

    // The cloud volume, into the buffer both renderers read. Also hoisted out of the path tracer's
    // branch, and for the same reason: in raster mode the buffer was never written at all, so
    // binding it to that pass would have shown a stale image or an empty one.
    void dispatch_clouds(VkCommandBuffer cmd, TracePush& trace, VkExtent2D render_extent,
                         u32 trace_offset);
    void update_quality();
    void update_lights();
    void apply_quality();
    void load_settings();
    void save_settings();

    void build_world();

    // One frame of the loading screen, drawn from wherever startup happens to be.
    //
    // Called at every point where startup passes from one piece of work to the next, so the bar
    // keeps moving through the parts that are NOT the world build ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the pipelines, the residency,
    // the first upload. Those are the parts a bar usually leaves out, which is exactly why it
    // reaches ninety-nine per cent and then sits there.
    void draw_loading();
    std::string loading_cache_path() const;
    // Where a built world and its load timings are kept: under the data root, not beside the world.
    static std::string cache_file_for(const std::string& world_path, const char* suffix);

    void stream(f64 seconds);
    void update_tools(const InputState& input, bool chisel_has_wheel, bool clipboard_has_wheel,
                      f64 dt);
    void invalidate_edited_chunks(const std::vector<Op>& ops);
    // The same, for a writer that is not made of ops. See the function.
    void announce_world_change(const i64 lo[3], const i64 hi[3]);
    void refresh_world_bounds();

    // The interface, one frame of it, and what it decided. Drawn after the world's own composite
    // and before the present, into a surface at the WINDOW's resolution rather than the render
    // scale's — a three-pixel-tall letter does not survive being stretched.
    void run_shell(f64 seconds);
    void seed_knobs();
    void apply_knobs();

    Options options_;
    // Copied out of the options so the sampler thread never reads them while the main thread is
    // in the middle of a reload. One bool, set once at startup.
    bool despeckle_ = true;
    Window& window_;
    Device& device_;
    Swapchain& swapchain_;
    GpuProfiler& profiler_;
    ShellPass& shell_pass_;
    ui::Shell& shell_;
    bool wants_title_ = false;
    std::string wants_world_;
    // Why this world came up empty, if it did. Said once, on the screen, by the first frame of
    // interface after the loading screen goes.
    std::string world_trouble_;
    bool shell_drawn_ = false;   // this frame's list had something in it
    Hud hud_;

    // The load, and the screen that reports it. The screen is torn down once the game is up ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it
    // holds a full-resolution image and nothing after startup needs it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but the progress itself
    // stays, because a level change is the same operation and will want to report the same way.
    LoadProgress progress_;
    LoadingScreen loading_screen_;
    // ---- sharpening the world while it is being walked around in --------------------------
    //
    // The world is built coarse so the player is in it in a second and a half, then sampled again
    // at twice the detail, and again, until it reaches what the clip asked for. Each pass replaces
    // the last: there is only ever ONE world, getting finer, rather than levels of detail sitting
    // beside each other waiting to be swapped by distance.
    //
    // Sampling happens on a thread of its own with its own workers. Everything that touches shared
    // state ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â interning the varied materials, writing voxels, replaying the edit log ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â happens on
    // the main thread between frames, because the type table and the world are not thread-safe and
    // making them so to save a few milliseconds a minute would be a poor trade.
    std::unique_ptr<forge::Script> refine_script_;
    std::unique_ptr<JobSystem> refine_jobs_;
    // The paste's own workers, and the reason they are not the sampler's is measured (D511).
    //
    // `JobSystem` is one FIFO queue, and `parallel_for` puts a take-LOOP on it: a worker that
    // picks one up stays inside it until that submitter's whole range is consumed. So while the
    // background sampler is running, every worker of `refine_jobs_` is occupied for the length of
    // the SAMPLE, the paste's own entries sit behind them, and `wait()` -- which helps with queued
    // work so that a waiting thread is never idle -- pops the sampler's entries and runs them on
    // the MAIN thread. The paste therefore cost whatever the sample beside it cost, and the
    // instrument said so in one column: the same 991-brick region pasted in 146 ms and in 7,076,
    // and the only region with no sample running beside it pasted in 75.
    //
    // Foreground work and background work must not share a queue. This is the foreground one.
    std::unique_ptr<JobSystem> paste_jobs_;
    std::unique_ptr<forge::SampleResult> refine_result_;
    std::thread refine_thread_;
    std::atomic<bool> refine_ready_{false};
    bool refine_running_ = false;
    u32 refine_scale_ = 1;      // what the world is at now; 1 is the clip's own detail
    i32 refine_authored_ = 0;   // voxels per metre the clip asked for
    i64 refine_at_[3]{0, 0, 0};
    std::string refine_cache_path_;
    u64 refine_cache_key_ = 0;

    // The clip cut into boxes, each refined to full detail on its own and nearest first.
    //
    // Refining the whole world a rung at a time is the wrong shape for this. Every rung is eight
    // times the last, so the final one is minutes, and until it lands EVERYTHING is coarse ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the
    // wall you are standing at included, however long you look at it. Sampling the box you are
    // standing in instead is a second, and it is the only part of the world anybody can see.
    struct RefineRegion {
        forge::Vec3 low;
        forge::Vec3 high;
        bool done = false;
    };
    std::vector<RefineRegion> refine_regions_;
    usize refine_region_ = 0;   // the one being sampled right now
    bool refine_wants_compact_ = false;
    // How many boxes the world on disk already has. The cache is written whenever refinement runs
    // out of things it can do and this has moved since, which is once per camera rather than once
    // per box: a six-hundred-megabyte file is not worth rewriting to record one more box, and it
    // is very much worth rewriting to record the fourteen a camera just paid two minutes for.
    usize refine_saved_regions_ = 0;
    // No region this camera can improve, which is where a still camera settles. Not "the world is
    // finished" ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â see the note in start_refinement ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but it IS a fixed point, so it is the state a
    // measurement can be taken in and compared against another run.
    bool refine_settled_ = false;
    bool settled_seen_ = false;   // --settle: the settled state has held long enough
    u64 settle_frame_ = 0;        // and this is the frame it was declared on
    u32 settle_streak_ = 0;       // consecutive frames with nothing left to sharpen
    // Long enough that a paste landing between the pick and the world being updated cannot be
    // mistaken for the end of the build. A region takes seconds; this is a fraction of one.
    static constexpr u32 kSettleFrames = 240;
    // Past this, take the measurement rather than never taking one. Generous: a cold facility
    // settles in a few thousand frames, so reaching this means something is actively unsettling
    // it, which an edit does.
    static constexpr u64 kSettleGiveUp = 30000;
    f64 refine_sample_ms_ = 0.0;   // the background half, which the paste timing never saw
    u64 refine_asked_ = 0;
    void start_refinement();
    void pump_refinement();
    // The ladder's boxes, from the clip's own bounds. One function so the run that builds the
    // world and the run that loads a half-built one plan the same grid ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â if they did not, the
    // flags in the cache would be read against boxes they were never about.
    void plan_refine_regions(const forge::Script& script);
    // Pick a half-built world back up: adopt the cache's flags onto the planned grid and leave
    // the ladder standing if anything is still coarse.
    void resume_refinement(forge::Script&& script, const WorldCache& cache,
                           const std::string& cache_path, u64 key, u32 coarse);
    // Keep what has been sharpened so far, if it is more than the file already holds.
    void save_refined_world();

    LoadHistory load_history_;
    u64 load_began_ns_ = 0;
    u64 loading_drawn_ns_ = 0;    // when the last loading frame went out, so it can be paced
    bool loading_quit_ = false;   // the window was closed while it was still building

    ComputePipeline resolve_;
    // R3: one invocation per face, working out light on the surface instead of on the screen.
    ComputePipeline shade_faces_;

    // The cloud volume, marched once per four-by-four block. See shaders/clouds.comp.
    ComputePipeline clouds_;
    GpuImage cloud_image_;
    GpuImage cloud_image_prev_;
    GpuImage cloud_marched_;
    bool cloud_ready_ = false;   // transitioned out of UNDEFINED once, then left in GENERAL
    u32 cloud_parity_ = 0;       // which of the two the cloud pass writes this frame
    // The set the cloud pass runs on: the parameter block and its three images, and nothing else.
    // It was the path tracer's whole set until R1e trimmed it to what a shader actually declares.
    VkDescriptorSetLayout cloud_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet cloud_set_ = VK_NULL_HANDLE;
    // The frame-statistics buffer went with them. It was the tracer's exposure meter, nothing has
    // written it since R3d, and a buffer bound to a set no shader declares is invisible waste.
    // R6's exposure meter needs a writer of its own -- see resolve.comp's kPreviewExposure.

    // Holds the frame rate by spending detail where it is worth most. Measured on the machine
    // it is running on, once, the first time the game starts. See documentation/19.
    // Frames left during which every surface re-measures its shadow, set by an edit. About two
    // seconds: long enough for a distant face covered by one pixel to gather the samples it
    // needs, short enough that it is over before anyone places the next voxel.
    u32 shadow_refresh_frames_ = 0;
    static constexpr u32 kShadowRefreshFrames = 120;

    // Set on the frame an edit lands, cleared as soon as the shader has been told once.
    //
    // Re-measuring faster is not enough on its own, and the second report of "undo is slow" is what
    // showed why. A face keeps two counts, and `face_accumulate` only throws its history away when
    // a sample contradicts a UNANIMOUS one (D319) -- which is the right conservative test for a
    // sample that might be noise, and no test at all for a face that was still mid-transition when
    // the next edit arrived. Delete the roof and a terrace face starts climbing from black towards
    // white; undo before it gets there and it is unanimous about nothing, so it has no history to
    // throw away and simply averages back down over hundreds of frames. Measured: fully shadowed
    // faces flat at ~42,000 against the 105,848 the same camera has when it was never edited, with
    // the mean drifting 0.44 -> 0.35 -> 0.31 over four hundred frames.
    //
    // The host does not have to INFER that the world changed. It knows, and it knows exactly where.
    // So faces inside the edited box are told outright to drop their history, once, on the frame it
    // happens -- exact information instead of a guess, and it leaves D319's rule untouched for the
    // case it was written for.
    bool edit_window_opened_ = false;

    // How many constraint points the last frame drew, so the refusal warning is said once when it
    // changes rather than sixty times a second while it holds.
    u32 last_marks_reported_ = 0;
    // The points, camera-relative, sorted and grouped for the shader. Kept between frames so it is
    // not reallocated on every one.
    std::vector<std::array<i32, 3>> mark_points_;
    u64 mark_upload_words_ = 0;   // what this frame packed into the clip buffer's tail
    u32 mark_upload_at_ = 0;

    // Where the last edit was, in absolute world voxels, grown by how far its shadow can fall.
    // Faces inside are made to re-measure at once; faces outside carry on as they were.
    i64 edit_lo_[3]{};
    i64 edit_hi_[3]{};
    // How far past the edit a shadow it casts can land. Sixteen metres, which covers anything
    // built by hand at any sun angle worth looking at, and is cheap because the cost is only
    // that those faces trace for the couple of seconds the region is live.
    static constexpr i64 kEditShadowReach = 512;

    // The emitters the tracer aims at, and the buffer they live in on the GPU. Rebuilt when
    // the world changes, which is the only time they move.
    GpuBuffer light_buffer_;
    u32 light_count_ = 0;
    bool lights_dirty_ = true;
    // What the emitter scan costs over a run, because it is O(world) and runs on every announced
    // change to the world. See update_lights, and R9g in documentation/21-renderer-rewrite.md.
    u64 light_build_ns_ = 0;
    u64 light_build_worst_ns_ = 0;
    u64 light_builds_ = 0;
    // R9g: the emissive cells of each chunk, kept between rebuilds. A chunk is dropped from here
    // when the world changes inside it and rescanned on the next rebuild; everything else is
    // reused. See announce_world_change for why a cell can never straddle a chunk.
    std::unordered_map<ChunkCoord, std::vector<EmissiveCell>, ChunkCoordHash> emitter_cache_;
    // How many chunks the last rebuild had to look at, against how many it did not. The figure
    // this stage is judged on, and it belongs beside the time for the reason trap 20 gives.
    u32 last_emitter_scans_ = 0;
    u32 last_emitter_reused_ = 0;
    // Floor division for the chunk a voxel falls in, which is not `/` for a negative coordinate --
    // and the origin is INSIDE this building, so half the world has negative coordinates.
    static i64 floor_div_i64(i64 value, i64 divisor) {
        return (value >= 0) ? (value / divisor) : -(((-value) + divisor - 1) / divisor);
    }
    // The identity of the list on the card, and a counter over it.
    //
    // The face pass accumulates lamp light per face and then stops casting rays, so it cannot
    // notice a lamp being placed, deleted or dimmed unless it is told. `light_hash_` is what
    // decides whether anything actually changed -- a rebuild triggered by an edit twenty metres
    // from the nearest sconce usually produces the identical list, and re-measuring the whole store
    // for that would cost a second of rays to arrive at the number already held. `light_version_`
    // is what the shader compares against, because a hash does not fit in the sixteen bits a face
    // has room for and a counter does.
    u64 light_hash_ = 0;
    u32 light_version_ = 1;   // 1 rather than 0, so a zeroed face record can never read as current
    bool light_changed_ = false;

    // The medium, already in the shader's units. Converted once, when the clip's resolution is
    // known, rather than per frame or per shader: see the note on --fog.
    f32 fog_[4]{};
    f32 fog_shape_[4]{};

    AutoQuality quality_;
    u32 applied_quality_level_ = 0xFFFFFFFFu;   // what the renderer is currently set to
    bool benchmark_pending_ = false;
    u64 benchmark_until_ = 0;
    f64 benchmark_total_ms_ = 0.0;
    u32 benchmark_frames_ = 0;
    // The worst frame WITHIN the measured window, which is not what FrameStats::max_ms reports.
    //
    // The rolling window is two hundred and forty frames and the benchmark is a hundred and eighty
    // of them, so the first frame ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â three hundred milliseconds of driver warm-up, every time ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â is
    // still inside it when the summary is printed. Reported as "worst", it turns a perfectly
    // steady run into evidence of a stutter, and this measurement was read that way once before
    // anyone checked which frame it was.
    f64 benchmark_worst_ms_ = 0.0;
    // Long enough for shaders to finish compiling and the first chunks to arrive: timing those
    // measures the loading, not the machine. Then a second or so of actual frames.
    static constexpr u64 kBenchmarkWarmupFrames = 90;
    static constexpr u64 kBenchmarkFrames = 90;

    // Scripted camera motion, so a measurement can be taken of the moving picture rather than
    // the settled one. See Options::fly.
    bool flying_ = false;
    f64 fly_state_[5]{-22.0, 5.0, -22.0, 45.0, -8.0};   // x, y, z, yaw, pitch
    f64 orbit_[4]{30.0, 6.0, 25.0, 26.0};   // radius, height, degrees a second, how far in and out
    f64 orbit_angle_ = 0.0;
    bool orbiting_ = false;

    // --cut: where the camera jumps to, and at which measured frame. See Options::cuts.
    //
    // A list, in the order they were given, with `next_cut_` the one still owed. Kept in order
    // rather than sorted by frame, because a cut whose frame is behind the one before it is a
    // typo the run should report rather than silently reorder -- and a harness that writes the
    // frames in the wrong order has produced a measurement of something nobody asked for, which
    // is trap 15's shape.
    struct Cut {
        f64 pose[5]{};
        u64 at = 0;
    };
    std::vector<Cut> cuts_;
    usize next_cut_ = 0;
    f64 fly_velocity_[4]{};                             // vx, vy, vz, vyaw
    // Shell thickness in voxels for anything the tools place. 0 is solid. Shared by the
    // chisel and the clipboard, because it is a property of how you are building rather than
    // of which tool is in your hand.
    u32 hollow_ = 0;
    f32 trace_camera_[6]{};
    f32 trace_forward_[3]{};

    // Last frame's camera, for the motion blur. See the fill in record_frame.
    f64 prev_origin_[3]{};
    f32 prev_forward_[3]{0.0f, 0.0f, 1.0f};
    f32 prev_right_[3]{1.0f, 0.0f, 0.0f};
    f32 prev_up_[3]{0.0f, 1.0f, 0.0f};
    i64 prev_camera_chunk_[3]{};
    bool motion_blur_ = true;

    // The weather. 0 is a clear sky and 1 an overcast; a shade under a half is a fair-weather day
    // with cumulus in it, which is what a building wants to be photographed under.
    f32 cloud_coverage_ = 0.45f;
    // The low deck's wind in metres a second. Six is a gentle breeze; the higher decks derive
    // their own from it, faster and veered, in shaders/pt_clouds.glsl.
    // The low deck's wind, in metres a second of GAME time. Real cumulus drift at five to fifteen
    // metres a second of REAL time, and the world's clock runs sixty times faster than the
    // player's ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so honest weather at honest speed crosses the sky at a few hundred metres a
    // second, which does not read as weather at all. It reads as smoke in a wind tunnel. The
    // coupling to game time is kept, because a cloud should cross a field in an in-game hour and
    // not an in-game week; the speed is set by how it looks.
    f32 cloud_wind_[2]{0.40f, 0.16f};
    f32 prev_cloud_time_ = 0.0f;
    bool face_ready_ = false;    // same, and for the same reason: it must survive a frame that
                                 // does not write it
    bool face_cleared_ = false;  // and holds kNoFace everywhere, so no clear is owed
    GpuImage visibility_image_;
    // Which face store slot each pixel's surface lives in. One word a pixel, resolved by the
    // marcher where the key is already known and read by the composite.
    GpuImage face_image_;
    // R4d. What the primary ray reached AFTER passing through transmissive matter — the world
    // behind a window, packed the way the visibility buffer packs the surface in front of it. Four
    // words a pixel, written only where the ray met glass and zero everywhere else, which is how
    // the composite knows whether there is a second layer to blend at all. See shaders/
    // visibility.comp for the packing and shaders/resolve.comp for what it does with it.
    GpuImage behind_image_;
    GpuImage render_target_;
    GpuImage depth_target_;
    VkDescriptorSetLayout resolve_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set_ = VK_NULL_HANDLE;
    Camera camera_;
    u32 debug_mode_ = 0;
    f32 detail_bias_ = 1.0f;
    // What fraction of the window the world is rendered at before being scaled up to it.
    // Every render target is sized from this and every dispatch is sized from the targets, so
    // this is the one place the saving comes from; the present blit already scales whatever it
    // is given up to the swapchain.
    f32 render_scale_ = 1.0f;
    bool mouse_look_ = false;
    // The click that captures the mouse must not also start a cut. Set when capture
    // happens, cleared when every button has come back up.
    bool swallow_click_ = false;

    // The chisel and its history. One player for now; the id is threaded through anyway
    // because undo is per player and retrofitting that later means revisiting every call.
    Chisel chisel_;
    Clipboard clipboard_;
    Toolbelt toolbelt_;
    // Undo and redo repeat while held: thirty steps back should be one long press.
    KeyRepeat repeat_undo_;
    KeyRepeat repeat_redo_;
    KeyRepeat repeat_more_;
    KeyRepeat repeat_fewer_;
    KeyRepeat repeat_turn_[4];   // left, right, up, down
    // X. Held, it drops a constraint point steadily instead of one per press, so a line of them can
    // be swept out with the mouse rather than tapped out one at a time. The chisel refuses a point
    // on the voxel it just marked, so holding it still adds one and then nothing.
    KeyRepeat repeat_add_point_;
    EditHistory history_;
    OpLog op_log_;
    std::vector<VoxelTypeId> materials_;
    usize material_index_ = 0;
    u64 tick_ = 0;
    u64 last_edit_voxels_ = 0;
    f64 last_edit_ms_ = 0.0;

    // World and streaming. There is no camera yet (Stage 3), so a debug focus point
    // orbits the scene and drives the same demand-driven path the renderer will.
    TagRegistry tags_;
    PropertyRegistry properties_;
    VoxelTypeTable types_;
    World world_;
    MatterLedger ledger_;
    Updater updater_;

    ChunkCoord world_min_{};
    ChunkCoord world_max_{};
    bool world_bounds_valid_ = false;
    TypeTables type_tables_;
    FeedbackBuffer feedback_;
    GpuBuffer params_buffer_;
    // The clipboard's held clip, as the resolve pass sees it: one cell per voxel, holding
    // the type id plus one so that zero can mean "outside the clip". Device-local, because
    // a ghost that fills the screen is read once per pixel and reading that over the bus
    // would make the preview cost more than the world behind it.
    GpuBuffer clip_buffer_;
    // The light meter's two slots (R6a). Device local, thirty-two bytes, never read back by the
    // host -- it rotates and zeroes them and the composite does the rest.
    GpuBuffer frame_stats_;
    GpuBuffer frame_stats_readback_;
    // The tracer's world-space face cache -- a quarter of a gigabyte of it -- is gone with R1e.
    // It had been read by nothing since R3d and was kept only because its binding number was one
    // the shared layout agreed about, which is exactly what trimming that layout released.
    GpuBuffer ballast_;
    GpuBuffer clip_staging_;
    struct ClipSlot {
        u32 first_cell = 0;
        u32 first_block = 0;
        u32 size[3]{};
        u32 blocks[3]{};
    };
    std::vector<ClipSlot> clip_slots_;
    u64 clip_uploaded_revision_ = 0;
    bool clip_upload_pending_ = false;
    u64 params_stride_ = 256;
    u64 frame_counter_ = 0;
    u32 last_feedback_ = 0;
    u32 last_feedback_truncated_ = 0;
    // Miss reports for a place the world has nothing at. See the note where it is counted.
    u32 last_feedback_phantom_ = 0;

    // The same, for the pool. The chunk path has been timed since Stage 2 and the node path never
    // was, so the CPU cost of building the tree has never appeared in any figure -- which is a
    // problem when a frame is 275 ms, the GPU is 7 ms, and nothing accounts for the rest.
    f64 node_ms_ = 0.0;
    f64 worst_node_ms_ = 0.0;
    // WHICH frame was the worst, which is the question three attempts at making it smaller should
    // have started with: a worst-of-run taken over startup is not something steady play can feel.
    u64 worst_node_frame_ = 0;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    // What both passes that walk the tree are told about it. One struct rather than two argument
    // lists, because the visibility pass and the shading pass probe the SAME two tables and a
    // capacity that disagreed between them would read a face at the wrong bucket -- silently, and
    // only for the faces whose probe run crossed the boundary.
    struct NodePush {
        u32 control[4];       // entry capacity, entry probes, face count, frame
        f32 sun[4];
        u32 face_capacity;    // buckets in the face table
        u32 face_probes;
        u32 face_stride;      // shade one face in this many each frame
        u32 provisional_base; // where the card's own faces start in the same array (R3e)
        u32 face_first;       // shading: the first slot this dispatch owns
        u32 light_count;      // live entries of the emitter list. 0 means the scene has no lamps
        u32 light_version;    // bumped when the list's CONTENTS change (light_list_hash)
        u32 light_reset;      // 1 on the one frame it changed, which reopens every idle face
        // Read the slot out of the compacted work list rather than from the invocation index.
        // Off for the provisional dispatch, which is its own small contiguous range.
        u32 from_worklist;
        u32 seen_window;      // frames a face keeps being lit after the last pixel that read it
        u32 prolong;          // a subdivided face inherits its parent's fit rather than remeasuring
        // A ray reports the bricks it crosses as read, not only the one it stops on (D427).
        u32 report_crossings;
        // How often a light ray may stamp a node it reads, in frames, a power of two. 0 is off.
        u32 light_read_period;
        // How many samples a face keeps when the host announces the world under it changed.
        // 0 restores the wipe, which is this change's control arm. See kFaceEditSeed.
        u32 edit_seed;
        // An edit reopens a face's lamp term only where it can stand between that face and a
        // fitting. 0 reopens the whole sixteen-metre box, which is the control arm.
        u32 lamp_edit_scope;
        // How often a face a pixel read may say so down the feedback buffer, in frames, a power of
        // two. 0 is off and leaves residency hearing only the request lattice, which is D508's
        // control arm and the state D502 was measured in.
        u32 face_read_period;
        // How often a face may name one face a light ray of its own landed on, in frames, a power
        // of two. 0 is off and is R9a's control arm.
        u32 secondary_period;
        // 1: a gathering ray reads the surface it lands on and the composite reads the result
        // instead of `kIndirectFloor`. 0 is the bounce control arm and restores that constant.
        u32 bounce;
        // The least far samples a face takes before its bounce may stop; 0 keeps kBounceMin.
        u32 bounce_min;
        // How many samples the bounce remembers; 0 keeps kBounceMemory. See that constant in
        // shaders/node.glsl for why the one term that measures other faces may not use a cumulative
        // mean, and Options::bounce_memory above for what a player saw when it did.
        //
        // These four bytes were `pad_before_sun_colour`, and the alignment argument that put a pad
        // here is unchanged and still load-bearing. A `vec4` in a shader's push block is aligned to
        // sixteen bytes; a `f32[4]` in a C++ struct is aligned to four. So `sun_colour` below started
        // at 112 on the card and at 108 here, the shader declared 128 bytes against a range of 124,
        // and every field from it on was read four bytes early — params.glsl's D168 fault arriving in
        // the push block, named by `--validation` in one line and by nothing else. A u32 is exactly
        // what the pad was, so the alignment it was there for cannot be lost by using it.
        u32 bounce_memory;
        // The sun's colour at the reference hour, so the face pass evaluates the same sky the
        // composite draws. The same four floats `TracePush::sun_colour` carries, from the same
        // place, because a gathering ray that escapes reads the sky and two plausible skies is one
        // sky too many. This takes the block to 116 bytes of the 128 that are guaranteed.
        f32 sun_colour[4];
    };
    static_assert(sizeof(NodePush) == 128,
                  "the node push block must be exactly what shaders/node.glsl declares, and no "
                  "larger than the 128 bytes Vulkan guarantees. Equality rather than a bound: a "
                  "host struct SMALLER than the shader's block is a range the pipeline rejects "
                  "under validation and reads past silently without it");
    NodePush make_node_push(u32 face_count) const;
    // How often a face nobody is looking at may cast, in frames. R9b's ray share; see
    // `secondary_light_share` and kProbeSecondaryStride. Not in the push block because that block is
    // exactly 128 bytes full, which is what the probe buffer's spare words are for.
    u32 secondary_light_stride() const;


    // The node pool, beside the chunk grid rather than replacing it yet. See
    // documentation/21-renderer-rewrite.md section 8, sub-step R1c.
    NodePool node_pool_;
    NodeBuffers node_buffers_;
    // R3. Claimed from what the marcher reports and mirrored to the card; nothing shades it yet.
    FaceStore face_store_;
    // What the table was sized at, so the audit can say "N live of M" rather than a bare count.
    // A count with nothing to measure it against cannot show a store that is nearly full, which is
    // the state that produces the reported picture and the one nothing was reporting.
    u32 face_budget_max_ = 0;
    // How many faces said "a pixel is reading me" this frame. The volume of D508's reports,
    // which is the one cost of that rule and is bounded by the feedback buffer it shares.
    u32 last_faces_read_reported_ = 0;
    // R9a's two counts, over the run: how many faces light rays asked for, and how many of those
    // were faces the store did not already have. Offered against claimed is the whole of what this
    // rule costs -- the difference is repeats, which cost one probe each and buy the residency stamp.
    u64 faces_secondary_offered_ = 0;
    u64 faces_secondary_claimed_ = 0;
    // For the in-play warning: refusals as of last frame, and when it last said anything.
    u64 last_face_refusals_ = 0;
    u64 last_face_warn_frame_ = 0;
    FaceBuffers face_buffers_;
    FaceLight face_light_;
    // One word a slot: the frame a pixel last read that face. Written by the visibility pass, read
    // by the shading pass, and the reason the light pass stopped shading the six hundred frames of
    // scenery behind the camera. See `face_seen` in shaders/node.glsl.
    FaceLight face_seen_;
    // One word a slot: the frame a GATHERING RAY last read that face. Written and read by the face
    // pass alone, and the reason the off-screen set stopped being a table of empty records. See
    // `face_gathered` in shaders/node.glsl for why it is a second array and not a second meaning for
    // the one above.
    FaceLight face_gathered_;
    // One word a node slot: when the card last REPORTED that node as read by a light ray. A
    // deduplicator, and the reason D429's rule fits down the feedback buffer at all (D430).
    FaceLight node_seen_;
    // When each face slot last REPORTED itself to the store as read by a pixel. The same shape and
    // the same guarantee as the two above: one word a slot, device local, written and read by the
    // card and by nothing else. See `face_read` in shaders/node.glsl.
    FaceLight face_read_;
    // One word a slot: what the surface under that face is MADE of -- its roughness, its metalness
    // and its flags, resolved by the light pass out of the interned tables and kept. R4a, and see
    // `face_material` in shaders/node.glsl for why it is the card that asks and why it cannot live
    // in `GpuFace::bins`, which is the field the plan reserved for it.
    FaceLight face_material_;
    // R4c's pool of outgoing bins: what each face that earns one REFLECTS along sixteen directions.
    // Not a word a slot like the four above it -- a lobe is thirty-four words and nine faces in ten
    // have no use for one, so it is a small cache faces hold blocks in rather than an array they
    // index. See `face_lobe` in shaders/node.glsl for the layout and for why it is held and not
    // owned. It borrows FaceLight for the same reason `light_probe_` does: what that class provides
    // is a device-local word array the host allocates, zeroes and never writes again.
    FaceLight face_lobe_;
    // One word a slot: what the surface under that face lets THROUGH -- its opacity, its index of
    // refraction and its translucency, resolved by the light pass on the same visit that resolves
    // the material and out of the same two tables. R4d, and see `face_medium` in shaders/node.glsl
    // for the word and for why it is a second one rather than six spare bits of the first.
    FaceLight face_medium_;
    // R9c. How far past the screen the primary pass claims, in pixels each side, and how sparsely
    // it samples out there. Both are worked out every frame from how fast the camera is turning, and
    // are nought when it is not. See the block that fills them for what they are measured against.
    u32 halo_margin_ = 0;
    u32 halo_stride_ = 8;
    f32 halo_prev_forward_[3]{};
    bool halo_forward_valid_ = false;
    // The gathering ray's counters, and the dials it reads. Not per slot -- see kLightProbeWords in
    // shaders/node.glsl. It borrows FaceLight because what that class provides is exactly what this
    // wants: a device-local word array, zeroed at creation, that the host can fill and copy back.
    FaceLight light_probe_;
    // The compacted dispatch: three words of VkDispatchIndirectCommand, a count, then the slots
    // that owe work. Written by `face_worklist.comp` and read by the shading pass, both on the
    // card; the host only ever zeroes the header. See face_worklist.comp for why it exists.
    GpuBuffer face_work_;
    ComputePipeline face_worklist_;
    u32 last_faces_seen_ = 0;
    // What `--chisel` actually did, so a run that carved nothing cannot be read as a run that
    // carved and cost nothing.
    u64 chisels_fired_ = 0;
    u64 chisel_voxels_ = 0;
    // Firings where the camera was looking at nothing within reach. Counted rather than ignored:
    // a run that missed every time and a run that never fired print the same pass table.
    u64 chisels_missed_ = 0;
    u64 chisel_apply_ns_ = 0;
    u64 chisel_bounds_ns_ = 0;
    u64 chisel_invalidate_ns_ = 0;
    ComputePipeline visibility_;
    VkDescriptorSetLayout node_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet node_set_ = VK_NULL_HANDLE;
    u32 last_node_built_ = 0;
    u32 last_node_evicted_ = 0;
    u32 last_node_evicted_nodes_ = 0;
    u32 last_node_evicted_on_screen_ = 0;
    u32 last_node_churned_ = 0;
    u32 last_node_deferred_ = 0;
    f64 stream_ms_ = 0.0;    // reading the feedback buffer and acting on it
    f64 uploads_ms_ = 0.0;   // the face mirror and the type tables
    f64 report_ms_ = 0.0;    // what the overlay is handed
    // "I could not fit it", which is never the same answer as "nothing is here" (trap 7).
    bool last_node_out_of_memory_ = false;
    FrameStats stats_;
    // A device that dies of a timeout and a device that dies of a bad address leave exactly the
    // same message behind. The difference is in the frames just before it, and those are gone by
    // the time anyone reads the report ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so they are kept here, and a frame slow enough to be
    // heading for the driver's patience says so at the time.
    f64 worst_frame_ms_ = 0.0;
    u64 worst_frame_at_ = 0;
};

bool Application::create_render_target(u32 width, u32 height) {
    visibility_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32G32B32A32_UINT,
                                             "visibility");
    face_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32_UINT, "face slots");
    // R4d's second layer. Same format as the visibility buffer because it carries the same kind of
    // answer about a different surface: what the ray reached once the glass let it past.
    behind_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32G32B32A32_UINT,
                                         "behind glass");
    render_target_ = create_storage_image(device_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                          "render_target");
    depth_target_ = create_storage_image(device_, width, height, VK_FORMAT_R32_SFLOAT,
                                         "depth_target");
    // The cloud history, FULL resolution and two deep. The pass writes one and reads the other,
    // alternating, so a frame can carry forward what the last one marched without reading the
    // image it is writing. See shaders/clouds.comp.
    cloud_image_ = create_storage_image(device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                        "cloud history a");
    cloud_image_prev_ = create_storage_image(device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                             "cloud history b");
    // And where this frame's marches land, packed one per four-by-four block. Small on purpose:
    // every invocation of the marching dispatch marches, so no warp is held up by lanes that are
    // only reprojecting. See the head of shaders/clouds.comp.
    cloud_marched_ = create_storage_image(device_, (width + kCloudScale - 1) / kCloudScale,
                                          (height + kCloudScale - 1) / kCloudScale,
                                          VK_FORMAT_R16G16B16A16_SFLOAT, "cloud marches");
    cloud_ready_ = false;
    cloud_parity_ = 0;

    face_ready_ = false;

    // Images are the only bindings that change on resize; the world buffers are created
    // once and never move.
    VkDescriptorImageInfo vis_info{};
    vis_info.imageView = visibility_image_.view;
    vis_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo depth_info{};
    depth_info.imageView = depth_target_.view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo colour_info{};
    colour_info.imageView = render_target_.view;
    colour_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo face_info{};
    face_info.imageView = face_image_.view;
    face_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo behind_info{};
    behind_info.imageView = behind_image_.view;
    behind_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo cloud_info[2]{};
    cloud_info[0].imageView = cloud_image_.view;
    cloud_info[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    cloud_info[1].imageView = cloud_image_prev_.view;
    cloud_info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Seven, not eleven: the chunk marcher's set is gone with R1e and the cloud pass does not
    // write a colour image, so what is left is the composite's pair, the cloud pair each way,
    // and the face-slot and behind-glass images.
    //
    // They were once missing, and the failure was silent in the worst way. The node pipeline ran,
    // did all its work, and stored its result into an unwritten descriptor - so the visibility
    // image kept whatever was in it and the picture never changed no matter what the marcher did.
    // Five separate changes to the traversal produced bit-identical images while the *timing*
    // moved with every one of them, which is the signature: the shader is running and its output
    // is going nowhere.
    //
    // Descriptors for the images are the only bindings that change on resize, which is why they
    // live here rather than beside the buffer writes where the node set was otherwise assembled -
    // and that split is exactly how they came to be forgotten.
    //
    // `write_count` below is a literal beside the array, which is the shape D518 caught: removing
    // a write means renumbering everything after it AND the count, and only `--validation` says so.
    VkWriteDescriptorSet writes[11]{};
    for (VkWriteDescriptorSet& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].dstSet = resolve_set_;
    writes[0].dstBinding = 0;
    writes[0].pImageInfo = &vis_info;
    writes[1].dstSet = resolve_set_;
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &colour_info;
    VkDescriptorImageInfo marched_info{};
    marched_info.imageView = cloud_marched_.view;
    marched_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[2].dstSet = cloud_set_;
    writes[2].dstBinding = kCloudBinding;
    writes[2].descriptorCount = 2;
    writes[2].pImageInfo = cloud_info;
    writes[3].dstSet = cloud_set_;
    writes[3].dstBinding = kCloudMarchedBinding;
    writes[3].pImageInfo = &marched_info;
    // And the same pair to the raster pass, which draws the sky the player actually sees.
    writes[4].dstSet = resolve_set_;
    writes[4].dstBinding = kCloudBinding;
    writes[4].descriptorCount = 2;
    writes[4].pImageInfo = cloud_info;
    // The node marcher writes the same two images the chunk marcher does, so resolve reads one
    // buffer whichever produced it.
    //
    // Guarded, because the render target is created before the descriptor sets are allocated on
    // the first pass through startup - and a write to a null set is invalid rather than ignored.
    // The node set is bound again where it is created, so the first frame has it either way.
    //
    // The composite's copy is NOT guarded, and the difference matters. The node set may not exist
    // yet on the first pass through startup, but the resolve set does - it is written three lines
    // up. Leaving binding 7 inside the guard means it is written only if a resize happens to run
    // this function again later, and a session without a resize dispatches the composite against a
    // descriptor that was never filled in. Validation says so plainly; without it the read is
    // whatever the pool held.
    writes[5].dstSet = resolve_set_;
    writes[5].dstBinding = 7;
    writes[5].pImageInfo = &face_info;
    // R4d's second layer, to the composite for the same reason and on the same terms.
    writes[6].dstSet = resolve_set_;
    writes[6].dstBinding = 14;
    writes[6].pImageInfo = &behind_info;
    u32 write_count = 7;
    if (node_set_ != VK_NULL_HANDLE) {
        writes[7].dstSet = node_set_;
        writes[7].dstBinding = 0;
        writes[7].pImageInfo = &vis_info;
        writes[8].dstSet = node_set_;
        writes[8].dstBinding = 1;
        writes[8].pImageInfo = &depth_info;
        writes[9].dstSet = node_set_;
        writes[9].dstBinding = 11;
        writes[9].pImageInfo = &face_info;
        writes[10].dstSet = node_set_;
        writes[10].dstBinding = 26;
        writes[10].pImageInfo = &behind_info;
        write_count = 11;
    }
    vkUpdateDescriptorSets(device_.handle(), write_count, writes, 0, nullptr);
    return true;
}

void Application::destroy_render_target() {
    if (visibility_image_.valid()) destroy_image(device_, visibility_image_);
    if (face_image_.valid()) destroy_image(device_, face_image_);
    if (behind_image_.valid()) destroy_image(device_, behind_image_);
    if (render_target_.valid()) destroy_image(device_, render_target_);
    if (depth_target_.valid()) destroy_image(device_, depth_target_);
    // The cloud history and its march buffer, created here with the rest and until now not
    // released with them. Three images and their memory, leaked once per resize and once per
    // run; validation names them at vkDestroyDevice.
    if (cloud_image_.valid()) destroy_image(device_, cloud_image_);
    if (cloud_image_prev_.valid()) destroy_image(device_, cloud_image_prev_);
    if (cloud_marched_.valid()) destroy_image(device_, cloud_marched_);
}

// Both axes scale by the same number and neither is rounded to the workgroup, because the
// aspect ratio has to survive: the dispatch already rounds up and the shaders already discard
// invocations past the resolution in the parameter block ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â they must, since a 1600x900 window
// was never a multiple of eight either. Rounding the height up to 592 from 585 would stretch
// the picture by a percent on one axis only, which is exactly the kind of fault that gets
// blamed on the lens.
VkExtent2D Application::scaled_extent() const {
    const VkExtent2D window = swapchain_.extent();
    const f32 scale = std::clamp(render_scale_, 0.25f, 1.0f);
    auto axis = [scale](u32 pixels) {
        return std::max(8u, static_cast<u32>(static_cast<f32>(pixels) * scale + 0.5f));
    };
    return {axis(window.width), axis(window.height)};
}

void Application::handle_resize() {
    // The window reports a size change on the first frame even when nothing moved;
    // rebuilding the swapchain for that is pure waste.
    if (!swapchain_.needs_recreate() && swapchain_.extent().width == window_.width() &&
        swapchain_.extent().height == window_.height()) {
        return;
    }
    device_.wait_idle();
    if (!swapchain_.recreate(window_.width(), window_.height())) return;
    destroy_render_target();
    const VkExtent2D render = scaled_extent();
    create_render_target(render.width, render.height);
}


// A count a person can read at a glance. Thirty-one million voxels is a number nobody parses;
// "31.0M" is one they can watch move, which is the entire job of putting it on screen.
std::string short_count(u64 value) {
    char buffer[32];
    if (value >= 1000000000ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1fB", static_cast<f64>(value) * 1e-9);
    } else if (value >= 1000000ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM", static_cast<f64>(value) * 1e-6);
    } else if (value >= 1000ull) {
        std::snprintf(buffer, sizeof(buffer), "%.0fK", static_cast<f64>(value) * 1e-3);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    }
    return buffer;
}

// How much longer, in the coarsest unit that is still honest.
//
// Rounded DOWN to whole seconds and never to zero while there is work left, because a countdown
// that reaches nought and keeps going is worse than no countdown: it does not just fail to inform,
// it actively tells the player something is wrong when nothing is.
std::string time_left(f64 seconds) {
    if (seconds < 0.0) return {};                    // too early to say, so say nothing
    if (seconds < 2.0) return "ALMOST THERE";
    char buffer[32];
    if (seconds < 90.0) {
        std::snprintf(buffer, sizeof(buffer), "%d SECONDS LEFT", static_cast<i32>(seconds));
    } else {
        const i32 minutes = static_cast<i32>(seconds / 60.0);
        const i32 rest = static_cast<i32>(seconds) - minutes * 60;
        std::snprintf(buffer, sizeof(buffer), "%d MIN %d SEC LEFT", minutes, rest);
    }
    return buffer;
}

// Where what this machine worked out about a world is kept: under the data root, in `cache\`, and
// NOT beside the world (D493).
//
// Reported as *why are there multiple files for the same world, a world should be just one file* —
// and the shelf was showing three: the `.wsworld`, a nineteen-megabyte `.world` built from it, and
// a `.load` of how long each stage took. A library is a file manager over a real folder, so
// everything in that folder is on the screen, and two of those three are not things a player has
// any reason to see, move, copy or think about. They are also not *theirs*: they are what this
// machine derived, they are worthless on any other machine, and a backup of the folder should not
// be carrying a third of a gigabyte of them.
//
// The name carries a hash of the world's full path as well as its stem, because two worlds called
// `house` in two folders are two worlds. A collision would only cost a rebuild — the file is keyed
// by content and a mismatch is discarded — but a collision that costs a rebuild every time you
// swap between them is a cache that stopped working for the one player who hit it.
std::string Application::cache_file_for(const std::string& world_path, const char* suffix) {
    std::error_code error;
    const std::filesystem::path full =
        std::filesystem::absolute(std::filesystem::path(world_path), error);
    const std::string text = error ? world_path : full.lexically_normal().string();
    u64 hash = 0xCBF29CE484222325ull;
    for (char c : text) hash = hash_combine(hash, static_cast<u64>(static_cast<u8>(c)));
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "-%016llx", static_cast<unsigned long long>(hash));
    const std::filesystem::path into = ui::default_root() / "cache";
    std::filesystem::create_directories(into, error);
    return (into / (std::filesystem::path(world_path).stem().string() + stamp + suffix)).string();
}

std::string Application::loading_cache_path() const {
    const std::string clip =
        options_.clip_file.empty() ? default_clip_path() : options_.clip_file;
    return cache_file_for(clip, ".load");
}

void Application::draw_loading() {
    if (!loading_screen_.valid() || loading_quit_) return;

    // Thirty frames a second, and not one more. The build wants every core it can get and this
    // costs a dispatch over the window; the requirement was that watching the load must not make
    // the load longer, and the way to keep that promise is to draw rarely enough that it cannot.
    const u64 at = now_ns();
    if (loading_drawn_ns_ != 0 && at - loading_drawn_ns_ < 33000000ull) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        return;
    }
    loading_drawn_ns_ = at;

    // The window still has to answer the OS, or it greys out and the player is told by their
    // desktop that the game has stopped responding while it is in fact working perfectly.
    if (!window_.pump()) {
        loading_quit_ = true;
        return;
    }
    if (swapchain_.needs_recreate() || window_.width() != swapchain_.extent().width ||
        window_.height() != swapchain_.extent().height) {
        if (window_.width() == 0 || window_.height() == 0) return;   // minimised
        vkDeviceWaitIdle(device_.handle());
        swapchain_.recreate(window_.width(), window_.height());
    }

    const LoadProgress::Snapshot look = progress_.look();

    LoadingFrame frame;
    frame.fraction = static_cast<f32>(look.fraction);
    frame.seconds = static_cast<f32>(static_cast<f64>(at - load_began_ns_) * 1e-9);
    frame.stage = static_cast<u32>(look.stage);
    frame.stage_text = stage_name(look.stage);
    if (look.expected > 0) {
        frame.count_text = short_count(look.done) + " OF " + short_count(look.expected) + " VOXELS";
    } else if (look.done > 0) {
        frame.count_text = short_count(look.done) + " VOXELS";
    }
    frame.left_text = time_left(look.seconds_left);

    // The accent is left at its default. It is meant to be the player's choice, and there is no
    // setting for it yet ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but it is only ever reached in the narrow band where inversion has
    // nothing to say, so a default here is a colour almost nobody will see rather than a decision
    // quietly made on the player's behalf.

    loading_screen_.present(swapchain_, frame);
}

// Begin sampling the next rung, if there is one.
//
// Halving the scale each time doubles the detail and multiplies the work by eight, so the ladder
// spends almost all of its total on the last step. That is the property that makes this worth
// doing: every rung before the last is nearly free next to it, and the player sees the world from
// the first one.
void Application::start_refinement() {
    if (refine_running_) return;
    if (refine_script_ == nullptr) {
        // There is no ladder at all ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the clip was built at its authored detail in one pass, or
        // the world came back from the cache already finished. Nothing here can be improved, and
        // that is precisely what settled means.
        //
        // It used to return without saying so, and --settle waits on this flag: a run with no
        // ladder therefore waited for a fixed point that had already happened and never took its
        // measurement at all. That was reachable before ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `--clip-coarse 1 --settle` hangs ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and
        // it is reachable constantly now that a finished world is cached and read back.
        refine_settled_ = true;
        return;
    }

    // Nearest first, measured from where the camera is now rather than from where it was when the
    // list was made. Somebody who walks across the building while it sharpens should have the far
    // side come good as they arrive, not have the near side finished behind them.
    const f64 cx = camera_.metres_x();
    const f64 cy = camera_.metres_y();
    const f64 cz = camera_.metres_z();

    // Where the camera is pointing, so that what is ON SCREEN is sharpened before what is behind
    // the player. Distance alone spends the first minute on the room they have their back to.
    const f64 cp = std::cos(camera_.pitch());
    const f64 fx = std::cos(camera_.yaw()) * cp;
    const f64 fy = std::sin(camera_.pitch());
    const f64 fz = std::sin(camera_.yaw()) * cp;

    usize best = refine_regions_.size();
    f64 keenest = 0.0;
    for (usize i = 0; i < refine_regions_.size(); ++i) {
        const RefineRegion& box = refine_regions_[i];
        if (box.done) continue;

        // Distance to the box, nought inside it, so the one you are standing in always wins.
        const f64 dx = std::max({box.low.x - cx, 0.0, cx - box.high.x});
        const f64 dy = std::max({box.low.y - cy, 0.0, cy - box.high.y});
        const f64 dz = std::max({box.low.z - cz, 0.0, cz - box.high.z});
        const f64 away = std::sqrt(dx * dx + dy * dy + dz * dz);

        // How big it is on screen: its size over its distance, which is the projected angle to
        // within a constant and is the whole of the pixel criterion. A box that covers more of the
        // view has more to gain from detail than one that covers less, whatever their distances.
        const f64 across = std::max({box.high.x - box.low.x, box.high.y - box.low.y,
                                     box.high.z - box.low.z});
        f64 keen = across / std::max(away, 0.5);

        // And whether it is in front. Not a frustum test ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a box behind the player is not merely
        // small on screen, it is absent, and no amount of nearness should buy it a turn ahead of
        // something visible. Halved rather than refused, because turning round should find the
        // world improved rather than untouched.
        const f64 to_x = (box.low.x + box.high.x) * 0.5 - cx;
        const f64 to_y = (box.low.y + box.high.y) * 0.5 - cy;
        const f64 to_z = (box.low.z + box.high.z) * 0.5 - cz;
        const f64 reach = std::sqrt(to_x * to_x + to_y * to_y + to_z * to_z);
        if (reach > 1e-6) {
            const f64 facing = (to_x * fx + to_y * fy + to_z * fz) / reach;
            if (facing < 0.0) keen *= 0.05;
        }

        // And whether anything is in the way.
        //
        // Facing the camera is not the same as being seen. A room behind a wall is squarely in
        // front of the player and scores as though it were on screen, which on a building of a
        // hundred rooms is most of the work spent on geometry nobody can look at. One ray through
        // the world answers it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the world the ray crosses is the coarse one, and a blocky wall
        // occludes exactly as well as a sharp one for this purpose.
        //
        // Asked last, and only of a box that is already the front runner, because a raycast is far
        // dearer than the arithmetic above and most boxes are eliminated by it.
        if (best != refine_regions_.size() && keen <= keenest) continue;

        if (reach > 1e-6) {
            const f64 v = static_cast<f64>(kVoxelsPerMetre);
            const RayHit blocked = raycast(world_, cx * v, cy * v, cz * v, to_x, to_y, to_z,
                                           reach * v);
            // Something solid, and not merely the box's own front face ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â anything within its own
            // extent is the thing itself arriving, not an obstruction.
            if (blocked.hit && blocked.distance < (reach - across) * v) continue;
        }

        best = i;
        keenest = keen;
    }
    if (best == refine_regions_.size()) {
        // Nothing this camera can improve. Not the same as "every box is sharp": a box behind a
        // wall is skipped by the occlusion test above and stays coarse for as long as the camera
        // stands here, so the facility settles at four regions left and never writes its cache.
        //
        // It is a fixed point all the same, and that is what a measurement needs. See --settle.
        refine_settled_ = true;
        return;
    }

    refine_settled_ = false;
    refine_region_ = best;
    refine_script_->settings.voxels_per_metre = refine_authored_;
    refine_script_->settings.low = refine_regions_[best].low;
    refine_script_->settings.high = refine_regions_[best].high;

    if (refine_jobs_ == nullptr) {
        // Fewer workers than the main system, and deliberately. This runs while somebody is
        // playing; taking every core would sharpen the world by stuttering it.
        const u32 hardware = std::thread::hardware_concurrency();
        refine_jobs_ = std::make_unique<JobSystem>(hardware > 4 ? hardware / 2 : 1);
    }

    refine_ready_.store(false, std::memory_order_release);
    refine_running_ = true;
    refine_thread_ = std::thread([this] {
        const u64 began = now_ns();
        auto built = std::make_unique<forge::SampleResult>(forge::sample(
            refine_script_->field, refine_script_->solid, refine_script_->paint,
            refine_script_->settings, refine_jobs_.get(), {}));
        // Every region the ladder sharpens, as well as the first build below. A region that came
        // back speckled and was pasted in would put the fault back after the coarse world had been
        // cleaned of it, and a seam between a despeckled chunk and a speckled one is worse than
        // either alone. This lambda is already off the main thread, so it costs nobody a frame.
        if (despeckle_) forge::despeckle(built->clip);
        refine_sample_ms_ = ns_to_ms(now_ns() - began);
        refine_asked_ = built->voxels_asked;
        refine_result_ = std::move(built);
        refine_ready_.store(true, std::memory_order_release);
    });
}

// Take delivery of a finished box, if one is ready, and put it in the world.
void Application::pump_refinement() {
    if (!refine_running_) {
        start_refinement();
        // Refinement has run out of things this camera can improve, and ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â because this branch is
        // only reached with no box in flight ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the last one has landed. That is the moment the
        // world on disk can be brought up to date, and it is the only moment when doing so cannot
        // catch the world half-pasted.
        if (!refine_running_) save_refined_world();
        return;
    }
    if (!refine_ready_.load(std::memory_order_acquire)) return;

    refine_thread_.join();
    refine_running_ = false;
    if (refine_result_ == nullptr) return;

    const u64 began = now_ns();

    // Take the finished box off the shared slot and set the NEXT one sampling before pasting this
    // one, rather than after.
    //
    // The two were serialised: the worker sat idle for the whole paste, then the main thread sat
    // idle for the whole sample, and the world sharpened at the sum of the two instead of the
    // larger. Overlapped, the sampler is never waiting on a paste it takes no part in ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which very
    // nearly halves how long it takes for what you are looking at to come good.
    //
    // Safe because nothing below reads the script: the paste needs only the result, and variation ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
    // the one thing that did read it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â no longer runs per region. The box is marked done first, so
    // the choice made below cannot land on the box being pasted.
    std::unique_ptr<forge::SampleResult> finished = std::move(refine_result_);
    const usize pasted_region = refine_region_;
    refine_regions_[pasted_region].done = true;
    start_refinement();

    // NOT varied, and this is the second time that lesson has been learned.
    //
    // Variation gives every voxel its own version of its material, so it interns something close to
    // one type per voxel. Done once over a whole clip that is a million of them, which is already
    // most of the table. Done per REGION it is a million per region and they do not share: the same
    // perturbed colour is interned again in every box that happens to contain it. Three hundred and
    // seventy-eight boxes reached 2,105,602 types and overran the buffer the table is uploaded
    // through ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â an assert, mid-play, after several minutes of hitching.
    //
    // It was also most of the hitch. Interning a million materials on the main thread is what those
    // two-hundred to nine-hundred millisecond frames were.
    //
    // So the world keeps the flat materials its paint rules give it. The no-two-voxels-alike
    // shading is lost until there is somewhere to put it that does not scale with how the world was
    // divided up ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is a real gap and is recorded as one, not a decision that this looks
    // better.

    // Sized like any foreground pool rather than like the sampler, which is deliberately held to
    // half the machine because it runs while somebody is playing. The paste is not background
    // work: it is the frame the player is waiting inside, it lasts about a tenth of a second, and
    // for that tenth of a second it is worth more than the sample it briefly oversubscribes.
    if (paste_jobs_ == nullptr && !options_.no_paste_pool) {
        paste_jobs_ = std::make_unique<JobSystem>();
    }
    JobSystem* const paste_pool =
        options_.no_paste_pool ? refine_jobs_.get() : paste_jobs_.get();

    // REPLACE, so the box supersedes the coarse voxels standing in for it. Stamped instead, the
    // blocky overshoot survives outside the finer surface and the world only ever grows.
    const u64 paste_began = now_ns();
    const PasteStats stamped = paste_clip(
        world_, ledger_, finished->clip, finished->origin_voxel[0] + refine_at_[0],
        finished->origin_voxel[1] + refine_at_[1],
        finished->origin_voxel[2] + refine_at_[2], PasteMode::Replace,
        MatterReason::PlayerPlace, 1, paste_pool, types_.type_count(), 1);
    const f64 paste_ms = ns_to_ms(now_ns() - paste_began);
    // NOT compacted here, and that was the hiccup.
    //
    // compact() walks the whole world to find chunks that have been emptied. Called once at the end
    // of a build that is the right thing; called after every REGION it is the whole world walked
    // three hundred times over, and it does not care how small the region was. That is why halving
    // the region size made the stall MORE frequent rather than smaller: the cost of finishing a box
    // is fixed, not proportional to its volume, and the fixed part was this.
    //
    // A region paste replaces coarse voxels with fine ones in the same place, so it very rarely
    // empties a chunk at all. Left to the end, where there is one walk instead of hundreds.
    if (stamped.chunks_left_empty) refine_wants_compact_ = true;

    // Everything the player did, done again. An op is a SHAPE ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â FillBox carries two corners in
    // world voxels, not the voxels it happened to change ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so replaying it against finer geometry
    // re-cuts the same volume at the new detail. The cut re-measures itself.
    const u64 replay_began = now_ns();
    const std::vector<Op>& done = op_log_.ops();
    if (!done.empty()) apply_ops(world_, done, ledger_);
    const f64 replay_ms = ns_to_ms(now_ns() - replay_began);

    // And the renderer is told, which for the whole life of the clip ladder it was not.
    //
    // This paste has just rewritten a box of the world at four times the detail it held a moment
    // ago, and everything downstream is holding a copy of the blocky version: the node pool's
    // leaves are copies taken at build time, and a face's ambient occlusion is a measurement taken
    // through them which, since R10d, converges and then stops casting rays for ever. Neither can
    // notice on its own -- the pool's feedback reports what a ray could not FIND, and a brick that
    // is resident but out of date is found; and a face's own samples cannot see a change that
    // arrives after they have stopped being taken, which is D373's lesson from the other end.
    //
    // The box is the clip's own extent where it landed, and the announcement is the same one an
    // edit makes, because it is the same event: the world here is not what you were told it was.
    // What it costs is bounded by that box and is charged against a paste that already measures in
    // seconds. See D397.
    const i64 paste_lo[3] = {finished->origin_voxel[0] + refine_at_[0],
                             finished->origin_voxel[1] + refine_at_[1],
                             finished->origin_voxel[2] + refine_at_[2]};
    const i64 paste_hi[3] = {paste_lo[0] + std::max(finished->clip.size[0] - 1, 0),
                             paste_lo[1] + std::max(finished->clip.size[1] - 1, 0),
                             paste_lo[2] + std::max(finished->clip.size[2] - 1, 0)};
    const u64 announce_began = now_ns();
    announce_world_change(paste_lo, paste_hi);
    const f64 announce_ms = ns_to_ms(now_ns() - announce_began);

    finished.reset();

    usize left = 0;
    for (const RefineRegion& box : refine_regions_) {
        if (!box.done) ++left;
    }
    // Split, because "pasted N ms" was three different things in one number and they do not scale
    // with the same quantity: the paste itself is O(the box), the replay is O(what the player has
    // done), and the announcement is O(the box in BRICKS). A region that asked for 440,142 voxels
    // took 7,099 ms and one that asked for 3,295,122 took 81 -- which no reading of a single
    // figure can explain, and which the split answers in one line.
    WS_LOG_INFO("clip",
                "region: sampled {:.0f} ms ({} voxels asked), pasted {:.0f} ms "
                "(paste {:.0f} + replay {:.0f} + announce {:.0f}), {} bricks, {} left",
                refine_sample_ms_, refine_asked_, ns_to_ms(now_ns() - began), paste_ms, replay_ms,
                announce_ms, stamped.bricks_written, left);

    if (left == 0) {
        // The one walk, now that there is nothing left to empty.
        if (refine_wants_compact_) world_.compact();
        const WorldStats now = world_.stats();
        WS_LOG_INFO("clip", "world fully sharpened: {} chunks, {} solid voxels", now.chunks,
                    now.solid_voxels);
        save_refined_world();
        if (!refine_cache_path_.empty()) {
            WS_LOG_INFO("clip", "kept the finished world; the next launch reads it back");
            refine_cache_path_.clear();
        }
        refine_script_.reset();
        refine_jobs_.reset();
        paste_jobs_.reset();   // nothing left to paste, so nothing left for these to do
        return;
    }

    start_refinement();
}

// What the world on disk is worth, and when it is worth writing.
//
// It used to be written only when the LAST box landed, and the last box never lands: a box behind
// a wall is skipped by the occlusion test in start_refinement and stays coarse for as long as the
// camera stands where it does. The facility settles at fourteen boxes of eighteen from its own
// default camera, so the cache was never written once, and every launch ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and every one of the
// forty-two runs of the measurement grid ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â rebuilt a hundred and twenty-five million voxels from
// the field. Two minutes, every time, for a file that was already sitting there in every sense
// except that nobody had saved it.
//
// So it is written at the fixed point instead, with the flags that say what it is. The next run
// loads it in a second, and if it stands somewhere else it sharpens what it can see from there and
// writes again ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the world converges across runs rather than being thrown away at the end of each.
void Application::save_refined_world() {
    if (refine_cache_path_.empty() || refine_regions_.empty()) return;

    usize done = 0;
    for (const RefineRegion& box : refine_regions_) {
        if (box.done) ++done;
    }
    // Nothing has been sharpened since the file was written. Rewriting six hundred megabytes to
    // say the same thing is the sort of cost that only shows up as a stutter nobody can explain.
    if (done <= refine_saved_regions_) return;

    // AN EDITED WORLD IS NEVER CACHED, finished or not.
    //
    // This used to refuse only a HALF-BUILT edited world, on the argument that a later region
    // paste is a Replace over its box and would put pristine clip geometry back over anything
    // carved inside it, while a world that is FINISHED has no later box to undo the edits and
    // so could be cached as it stood. Every word of that is true and it answers the wrong
    // question. It asks whether the cache would be SELF-CONSISTENT. The question is what the
    // cache IS.
    //
    // This file is keyed on the CLIP and handed to every world built from that clip, in this run
    // and in every run after it. So a player who carved a square into the floor of a finished
    // facility had that square written into the clip's cache -- and then every new world they
    // made from the facility came up with the square already in it. Reported exactly that way,
    // and made baffling by the game having no world saving yet, so the one thing that could not
    // be happening appeared to be. The persistence was not the world's; it was the clip's.
    //
    // What it costs: somebody who builds before the ladder finishes gets no cache written that
    // run, so the next launch resamples. That is the right way round -- a cache is an
    // optimisation and a world coming back with somebody else's edits in it is a wrong answer --
    // and it costs nothing in the ordinary case, because the ladder settles long before anyone
    // has walked to the far side of the building.
    if (!op_log_.ops().empty()) {
        WS_LOG_INFO("clip",
                    "{} of {} regions sharpened, but the world has been edited; not caching it "
                    "as the clip's own -- the cache is keyed on the clip, so every world built "
                    "from it would come up with these edits",
                    done, refine_regions_.size());
        return;
    }

    WorldCache cache;
    cache.tags = &tags_;
    cache.properties = &properties_;
    cache.types = &types_;
    cache.world = &world_;
    cache.ledger = &ledger_;
    cache.materials = materials_;
    cache.regions.reserve(refine_regions_.size());
    for (const RefineRegion& box : refine_regions_) {
        CachedRegion out;
        out.low[0] = box.low.x;
        out.low[1] = box.low.y;
        out.low[2] = box.low.z;
        out.high[0] = box.high.x;
        out.high[1] = box.high.y;
        out.high[2] = box.high.z;
        out.done = box.done;
        cache.regions.push_back(out);
    }
    // Where the lamps are, so the run that loads this file does not have to read every brick of
    // every chunk to find out. R9g, and D587 is what it costs when nobody keeps it: 14 ms of scan
    // to rediscover twenty-one fittings.
    //
    // Scanned here for anything not already known rather than assumed complete. The cache is a
    // fixed point of refinement and a chunk may never have been asked about -- writing only what
    // happens to be in the map would put a world on disk whose lamps depend on where the camera
    // stood while it was built, which is precisely the fault R9 as a whole is about.
    world_.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
        auto found = emitter_cache_.find(coord);
        if (found == emitter_cache_.end()) {
            found = emitter_cache_
                        .emplace(coord, scan_chunk_emitters(
                                            chunk, coord.x * static_cast<i64>(kChunkEdge),
                                            coord.y * static_cast<i64>(kChunkEdge),
                                            coord.z * static_cast<i64>(kChunkEdge), types_))
                        .first;
        }
        cache.emitters.push_back(CachedEmitters{coord.x, coord.y, coord.z, found->second});
    });
    if (!write_world_cache(refine_cache_path_, refine_cache_key_, cache)) return;
    refine_saved_regions_ = done;
    WS_LOG_INFO("clip", "kept the world with {} of {} regions sharpened", done,
                refine_regions_.size());
}

// Boxes of about twelve metres, cut from the clip's own bounds.
//
// Refining the whole world a rung at a time is the wrong shape for this: every rung is eight times
// the last, so the final one is minutes, and until it lands EVERYTHING is coarse ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the wall you are
// standing at included. Sampling the box you are standing in instead is a second.
//
// Twelve metres, and the number comes from measuring where the time goes. Sampling a four-metre box
// took about a hundred milliseconds for ten thousand voxels ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â ten microseconds each, against barely
// one when the whole clip is sampled in a single call. Almost none of that is the voxels. It is the
// fixed cost of a sample: allocating the clip, starting the workers, descending from the root of a
// field that describes the entire building however small the box asked for is. Three hundred and
// seventy-eight boxes paid it three hundred and seventy-eight times, and the world sharpened at
// eleven boxes in twenty-two seconds.
//
// Four was chosen to keep the stall short, and that reasoning was wrong twice over: the paste
// measures ZERO milliseconds, and what actually stalled was the world compaction, which is now done
// once at the end. A small box buys nothing. Twelve is twenty-seven times the volume for very nearly
// the same fixed cost, and still fine enough that the box in front of you is a small part of the
// building rather than half of it.
//
// It is a pure function of the clip's bounds on purpose. The cache records which of these boxes have
// been sharpened, and a flag is only meaningful against a grid that comes out the same way twice.
void Application::plan_refine_regions(const forge::Script& script) {
    const forge::Vec3 lo = script.settings.low;
    const forge::Vec3 hi = script.settings.high;
    const f64 want = 12.0;
    const auto steps = [&](f64 a, f64 b) {
        return std::max<i32>(1, static_cast<i32>(std::ceil((b - a) / want)));
    };
    const i32 nx = steps(lo.x, hi.x);
    const i32 ny = steps(lo.y, hi.y);
    const i32 nz = steps(lo.z, hi.z);
    refine_regions_.clear();
    refine_regions_.reserve(static_cast<usize>(nx) * static_cast<usize>(ny) *
                            static_cast<usize>(nz));
    for (i32 z = 0; z < nz; ++z) {
        for (i32 y = 0; y < ny; ++y) {
            for (i32 x = 0; x < nx; ++x) {
                RefineRegion box;
                box.low = {lo.x + (hi.x - lo.x) * x / nx, lo.y + (hi.y - lo.y) * y / ny,
                           lo.z + (hi.z - lo.z) * z / nz};
                box.high = {lo.x + (hi.x - lo.x) * (x + 1) / nx,
                            lo.y + (hi.y - lo.y) * (y + 1) / ny,
                            lo.z + (hi.z - lo.z) * (z + 1) / nz};
                refine_regions_.push_back(box);
            }
        }
    }
}

// A cached world is not necessarily a finished one, and this is what tells them apart.
//
// The ladder is stood back up over the world that came off the disk, and the boxes the file says
// were sharpened are marked so. What is left carries on from wherever this run happens to be
// standing ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is usually somewhere else, which is how a world that no single camera can finish
// still finishes.
//
// The boxes are checked rather than trusted, and this is not belt and braces. The cache key covers
// the clip's text, the resolution, and the modification times of src/forge, src/world and
// src/game/clip.* ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â deliberately, so that editing a menu label does not throw away a minute of
// resampling. plan_refine_regions is in NEITHER, so changing how the grid is cut leaves every
// existing cache file matching its key while its flags refer to boxes that no longer exist. The
// alternative ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â adding this file to the key ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â would invalidate every built world on every edit to
// five thousand lines of renderer, HUD and command line, which is the cost that list was drawn up
// to avoid. So the corners are compared instead, and a grid that has moved re-sharpens from
// scratch: slow once, rather than a building that is quietly coarse in the wrong places for ever.
void Application::resume_refinement(forge::Script&& script, const WorldCache& cache,
                                    const std::string& cache_path, u64 key, u32 coarse) {
    refine_cache_path_ = options_.no_clip_cache ? std::string() : cache_path;
    refine_cache_key_ = key;
    refine_authored_ = (options_.clip_metre > 0)
                           ? options_.clip_metre
                           : script.settings.voxels_per_metre * static_cast<i32>(coarse);
    refine_scale_ = coarse;
    refine_at_[0] = options_.clip_at[0];
    refine_at_[1] = options_.clip_at[1];
    refine_at_[2] = options_.clip_at[2];
    refine_script_ = std::make_unique<forge::Script>(std::move(script));
    plan_refine_regions(*refine_script_);

    const bool same_grid = cache.regions.size() == refine_regions_.size();
    bool same_boxes = same_grid;
    if (same_grid) {
        for (usize i = 0; i < refine_regions_.size() && same_boxes; ++i) {
            const CachedRegion& from = cache.regions[i];
            const RefineRegion& box = refine_regions_[i];
            same_boxes = from.low[0] == box.low.x && from.low[1] == box.low.y &&
                         from.low[2] == box.low.z && from.high[0] == box.high.x &&
                         from.high[1] == box.high.y && from.high[2] == box.high.z;
        }
    }
    if (!same_boxes) {
        if (!cache.regions.empty()) {
            WS_LOG_WARN("clip",
                        "the cached world's {} regions do not match the {} this build plans; "
                        "sharpening all of them again",
                        cache.regions.size(), refine_regions_.size());
        }
        // An empty list is the other case entirely: a world built at its authored detail in one
        // pass, with no ladder behind it and nothing to carry on. Nothing is coarse, so nothing
        // needs sharpening, and standing the ladder up over it would re-sample the whole building
        // to arrive back where it already is.
        if (cache.regions.empty()) {
            refine_script_.reset();
            refine_regions_.clear();
            refine_cache_path_.clear();
            return;
        }
    } else {
        for (usize i = 0; i < refine_regions_.size(); ++i) {
            refine_regions_[i].done = cache.regions[i].done;
        }
    }

    usize done = 0;
    for (const RefineRegion& box : refine_regions_) {
        if (box.done) ++done;
    }
    refine_saved_regions_ = done;
    if (done == refine_regions_.size()) {
        // Finished, so there is no ladder to stand up and no reason to keep the field alive.
        refine_script_.reset();
        refine_regions_.clear();
        refine_cache_path_.clear();
        refine_saved_regions_ = 0;
        WS_LOG_INFO("clip", "the cached world is fully sharpened");
        return;
    }
    WS_LOG_INFO("clip", "cached world has {} of {} regions sharpened; carrying on from here", done,
                refine_regions_.size());
}

void Application::build_world() {
    const u64 start = now_ns();

    // The facility *is* the scene. There is no scripted fallback any more: a hand-written scene
    // in C++ and a clip file were two ways of saying the same thing, and only one of them can be
    // edited without a compiler, measured by the forge, or weathered.
    {
        // What to build, in the order of who asked most specifically: a world chosen in the
        // library, then a clip named on the command line, then the scene the game ships with.
        //
        // A `.wsworld` is a clip script today. The single-file container with append-only
        // journaling is Stage 15's own and is not built yet, so the shelf holds what a world
        // actually is at this point in the project — the description it is grown from — and the
        // extension is already the one the container will use. Nothing above here knows the
        // difference, which is the point of the path being the only thing passed down.
        const std::string path = !options_.world.empty()  ? options_.world
                                 : options_.clip_file.empty() ? default_clip_path()
                                                              : options_.clip_file;

        // The clip is read once, here, with everything it includes spliced in, and that spliced
        // text is what everything downstream works from ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the cache key as well as the parser.
        // Keying the cache on the whole assembly and not just the manifest is what makes editing
        // one fragment of a twenty-fragment building rebuild the building.
        progress_.enter(LoadStage::Reading);

        std::vector<forge::SourceLine> origin;
        std::vector<forge::ScriptError> trouble;
        // The clips the game ships with are where an include that is not beside its own file is
        // looked for (D494). That is what lets `facility.wsworld` be one file on the shelf: the
        // twenty-two pieces it is assembled out of live with the game, are never copied anywhere,
        // and therefore cannot be deleted out from under it.
        const std::string source = forge::expand_includes(
            path, origin, trouble, (std::filesystem::path(Window::base_path()) / "clips").string());

        // AND THE FILE ITSELF, when it is a copy of one the game ships.
        //
        // D607 taught the game to say when an INCLUDE beside a world shadows a shipped fragment.
        // This is the same fault one level up and it took a second report to find, because moving
        // the stale fragments out fixed nothing: the world on the shelf is `facility.wsworld`, a
        // copy of `clips/facility.clip` taken on some earlier day, and the manifest is where the
        // assembly lives. So the fix that put the furniture back into the halls went into the
        // game's manifest and never into the copy, and from the player's chair the building was
        // unchanged for the third time running.
        //
        // The worlds shelf lists no built-ins -- `shipped_kinds()` gives the worlds kind no
        // shipped folder, the facility is on the CLIPS shelf -- so this copy is the only facility
        // world a player has and deleting it would leave them none. Which is exactly why it has
        // to announce itself rather than be quietly correct or quietly wrong.
        //
        // Compared on the file's own text with line endings and the author tag taken out: the tag
        // is stamped in as a world is copied to the shelf (D447) and is not a difference in what
        // the file builds.
        if (!source.empty()) {
            const std::filesystem::path shipped =
                std::filesystem::path(Window::base_path()) / "clips" /
                (std::filesystem::path(path).stem().string() + ".clip");
            std::error_code there;
            if (std::filesystem::exists(shipped, there) && !there &&
                shipped.lexically_normal() !=
                    std::filesystem::absolute(std::filesystem::path(path), there)
                        .lexically_normal()) {
                const auto flatten = [](const std::string& from) {
                    std::ifstream in(from, std::ios::binary);
                    std::string text((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    std::string out;
                    out.reserve(text.size());
                    for (char c : text) {
                        if (c != '\r') out += c;
                    }
                    return ui::without_author(out);
                };
                if (flatten(path) != flatten(shipped.string())) {
                    WS_LOG_WARN("clip",
                                "'{}' is a COPY of the game's own '{}' and the two have drifted "
                                "apart. This world is built from the copy, so nothing the game "
                                "ships changes it. Duplicate the built-in again to catch up",
                                std::filesystem::path(path).filename().string(),
                                shipped.filename().string());
                }
            }
        }

        JobSystem jobs;

        forge::Script script = forge::parse_clip_script(source, types_, tags_);
        script.errors.insert(script.errors.begin(), trouble.begin(), trouble.end());
        for (forge::ScriptError& error : script.errors) {
            if (error.line > 0 && error.line <= origin.size()) {
                const forge::SourceLine& from = origin[error.line - 1];
                WS_LOG_ERROR("clip", "{}:{}: {}", from.file, from.line, error.message);
            } else {
                WS_LOG_ERROR("clip", "line {}: {}", error.line, error.message);
            }
        }
        if (options_.clip_metre > 0) script.settings.voxels_per_metre = options_.clip_metre;

        // Coarse sampling, scaled back up when it is pasted.
        //
        // Sampling is the expensive half of loading and it goes as the CUBE of the resolution, so
        // an eighth of the detail is around five hundred times less work. What it is not is a
        // smaller building: voxels_per_metre decides how big a metre is, so dividing it alone would
        // shrink the whole thing. The scale on paste is what puts the size back.
        u32 coarse = (options_.clip_coarse > 0) ? options_.clip_coarse : 1u;
        {
            u32 shift = 0;
            while ((1u << (shift + 1)) <= coarse) ++shift;
            coarse = 1u << shift;
        }
        if (coarse > 1) {
            const i32 asked = script.settings.voxels_per_metre;
            script.settings.voxels_per_metre = std::max<i32>(1, asked / static_cast<i32>(coarse));
            WS_LOG_INFO("clip", "coarse build: sampling at metre {} and scaling {}x on paste",
                        script.settings.voxels_per_metre, coarse);
        }

        // The air, in the shader's units, now that the clip has said how big a metre is.
        //
        // Authored as extinction per metre, a single-scattering albedo, an asymmetry and a scale
        // height in metres. Extinction is what the fog takes out of a beam; the albedo is how
        // much of that it puts back rather than absorbing, and splitting them that way is what
        // lets smoke and mist be told apart with one number rather than four.
        {
            // The air is NOT empty by default any more.
            //
            // It was, and that is why none of the fog and haze work could be seen without a flag:
            // every scene was a vacuum unless --fog said otherwise, so the honest answer to "why
            // can I not see the haze" was that there was none in the world.
            //
            // Clean air is not a vacuum either. These numbers are a real atmosphere rather than a
            // taste: Koschmieder's law says extinction = 3.912 / visibility, and the World
            // Meteorological Organization's own bands put HAZE at two to five kilometres of
            // visibility and a clear day at over ten. Eight kilometres is the top of haze and the
            // bottom of clear -- a good day with air in it, which is what most days are and what no
            // photograph of a landscape is ever without.
            //
            // Over a two hundred metre courtyard that is about five per cent, which is a tint you
            // would not name but would notice the absence of. Over the kilometres of a horizon view
            // it is the difference between distance and a flat backdrop.
            //
            // g of 0.80 because aerosol is strongly forward-scattering; a scale height of 400 m
            // because haze sits in the boundary layer and thins out above it.
            f64 authored[5] = {3.912 / 8000.0, 0.90, 0.80, 400.0, 0.0};
            if (!options_.fog.empty()) parse_reals(options_.fog, authored, 5);
            const f64 per = static_cast<f64>(script.settings.voxels_per_metre);
            const f64 extinct = std::max(authored[0], 0.0) / per;
            const f64 albedo = std::clamp(authored[1], 0.0, 1.0);
            fog_[0] = fog_[1] = fog_[2] = static_cast<f32>(extinct * albedo);
            fog_[3] = static_cast<f32>(extinct);
            fog_shape_[0] = static_cast<f32>(authored[2]);
            fog_shape_[1] = static_cast<f32>(authored[3] * per);
            fog_shape_[2] = static_cast<f32>(authored[4] * per);
            WS_LOG_INFO("clip", "fog {:.4f}/voxel, albedo {:.2f}, g {:.2f}, scale {:.1f} voxels",
                        extinct, albedo, fog_shape_[0], fog_shape_[1]);
        }

        // Building one named part on its own, so a fragment can be looked at without the rest of
        // the building standing around it.
        if (!options_.clip_part.empty()) {
            u32 piece = 0;
            if (script.part(options_.clip_part, piece)) {
                // The paint is left alone. Rules keyed on other parts simply do not match here,
                // and the part comes out wearing the materials it will wear in the building ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
                // which is the point of looking at it.
                script.solid = piece;
                script.has_solid = true;
            } else {
                WS_LOG_ERROR("clip", "no part called '{}' ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â check the `let` name",
                             options_.clip_part);
            }
        }
        const u64 parsed_at = now_ns();

        // A world already built from exactly this text, at exactly this resolution, is worth more
        // than the ability to build it again. Two hundred million field evaluations do not fit in
        // a second and never will; a third of a gigabyte off a disk does.
        //
        // The resolution comes from the parsed script rather than from an assumption about the
        // default, because a clip can name its own and a key that ignored that would hand back a
        // world sampled at the wrong size.
        const std::string cache_path = cache_file_for(path, ".world");
        // Keyed on the source WITHOUT its author tag. Who made a file is not part of what the file
        // builds, and counting it means a world put on the shelf — which is stamped with its
        // author as it is copied (D447) — no longer matches the world already built beside it. The
        // first open of every library world was a rebuild from cold because of one comment line.
        const u64 key =
            world_cache_key(ui::without_author(source) + "|part=" + options_.clip_part,
                            script.settings.voxels_per_metre, build_stamp());
        if (!source.empty() && !options_.no_clip_cache) {
            WorldCache cache;
            cache.tags = &tags_;
            cache.properties = &properties_;
            cache.types = &types_;
            cache.world = &world_;
            cache.ledger = &ledger_;
            // The cache read is left under Reading rather than given a stage of its own. On a hit
            // the whole thing is half a second, so how the bar apportions it is invisible; on a
            // miss it cost nothing to have tried. Splitting it would mean deciding which stage it
            // belonged to BEFORE knowing whether it succeeded, and putting the bar back afterwards
            // is how a bar starts going backwards.
            // A cached world that does not match this key is dead: the clip changed, or the code
            // that builds it did. Deleting it here rather than waiting for the rebuild to overwrite
            // it matters because these files are HUGE -- the facility is six hundred megabytes -- and
            // a build that is interrupted, or a clip that is renamed or removed, leaves the old one
            // on disk for ever with a key nothing will ever ask for again.
            //
            // The key already covers everything it should. It is hashed from the fully spliced
            // source, so editing any INCLUDED file counts, and from the newest modification time
            // across src/forge and src/world, so changing the compiler counts too.
            std::error_code stale;
            if (!std::filesystem::exists(cache_path, stale)) {
                // nothing to clear
            } else if (!world_cache_matches(cache_path, key)) {
                std::filesystem::remove(cache_path, stale);
                if (!stale) {
                    WS_LOG_INFO("world", "'{}' changed since its cache was built; discarded it",
                                path);
                }
            }

            if (read_world_cache(cache_path, key, cache, &jobs)) {
                progress_.enter(LoadStage::Uploading);
                // The palette comes from the SCRIPT, not from the cache.
                //
                // The clip is what declares the materials -- twenty-five of them in the facility --
                // and it has been parsed by this point regardless, because the cache key is hashed
                // from it. The cache carries a copy so that a world can come back complete without
                // one, and that copy is only as good as the build that wrote the file: the one on
                // disk here was written before the field existed and reads back empty, which fell
                // through to a palette of ONE. Q and E then cycled a list with nowhere to go, and
                // the report was "I cannot change materials" -- a key that looks broken because the
                // thing it steps through has one entry.
                //
                // Preferring the script fixes every stale cache in existence rather than the next
                // one written, and it cannot go stale itself.
                materials_ = script.material_types.empty() ? cache.materials
                                                           : script.material_types;
                const char* palette_from =
                    script.material_types.empty() ? "the cache" : "the clip";
                if (materials_.empty()) {
                    materials_.push_back(1);
                    palette_from = "nowhere -- neither the clip nor the cache had one";
                }
                material_index_ = options_.material % materials_.size();
                chisel_.set_material(materials_[material_index_]);
                // And where the lamps are, which comes back with the world rather than being read
                // out of it again. R9g. An OLD file carries none, and that has to mean "nobody
                // wrote any" rather than "there are none" -- so the map is simply left empty and
                // every chunk is scanned on the first rebuild, exactly as before. Trap 7, and here
                // the wrong answer is a building with its lights off.
                for (const CachedEmitters& chunk : cache.emitters) {
                    emitter_cache_.emplace(ChunkCoord{chunk.chunk_x, chunk.chunk_y, chunk.chunk_z},
                                           chunk.cells);
                }
                if (!cache.emitters.empty()) {
                    WS_LOG_INFO("light", "the lamps came back with the world: {} chunks of cells",
                                cache.emitters.size());
                }
                // Logged because a palette of one is indistinguishable from a key that does not
                // work, and the two have different fixes. That is how this was found.
                WS_LOG_INFO("tool", "palette: {} materials from {}", materials_.size(),
                            palette_from);
                // What came off the disk may be a world that stopped short ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â see
                // resume_refinement. The script has to be handed over here rather than
                // rebuilt later, because it owns the field the background sampler reads and
                // parsing is the only place it comes from.
                if (coarse > 1) {
                    resume_refinement(std::move(script), cache, cache_path, key, coarse);
                }
                const WorldStats cached_stats = world_.stats();
                WS_LOG_INFO("world", "'{}' loaded from cache in {:.0f} ms [t+{:.0f} ms]: {} chunks, {} solid "
                                     "voxels", path, ns_to_ms(now_ns() - start),
                            ns_to_ms(now_ns() - load_began_ns_),
                            cached_stats.chunks, cached_stats.solid_voxels);
                return;
            }
        }

        if (script.ok()) {
            progress_.enter(LoadStage::Sampling);
            forge::SampleResult built = forge::sample(
                script.field, script.solid, script.paint, script.settings, &jobs,
                [this](f64 fraction, u64 done, u64 expected) {
                    progress_.within(fraction);
                    progress_.count(done, expected);
                });
            // D610. Before variation, which is the only place it can go: variation mints a record
            // per voxel and after it every voxel is alone in its material by construction.
            if (options_.despeckle) {
                const forge::DespeckleReport dots = forge::despeckle(built.clip);
                if (dots.repainted > 0) {
                    WS_LOG_INFO("clip",
                                "despeckled {} lone voxels of the wrong material ({} left as a "
                                "deliberate stipple)",
                                dots.repainted, dots.left);
                }
            }
            const u64 sampled_at = now_ns();
            progress_.enter(LoadStage::Varying);
            // Every voxel gets its own version of its material before it goes in, so the world
            // holds the varied clip rather than the flat one ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but only at the detail that keeps
            // it. On a coarse build the ladder will replace this world several times over, and
            // interning a million materials per rung overruns the table; see pump_refinement.
            const forge::VariationReport variety =
                (coarse > 1) ? forge::VariationReport{}
                             : forge::apply_variation(built.clip, types_, script.field,
                                                      script.variation, script.settings, built,
                                                      &jobs);
            const u64 varied_at = now_ns();
            if (variety.voxels > 0) {
                WS_LOG_INFO("clip",
                            "variation: {} records over {} voxels, largest group {} "
                            "(perturb {:.0f} ms, intern {:.0f} ms, resolve {:.0f} ms)",
                            variety.distinct_types, variety.voxels, variety.largest_group,
                            variety.perturb_ms, variety.intern_ms, variety.resolve_ms);
            }
            progress_.enter(LoadStage::Stamping);
            const PasteStats stamped = paste_clip(
                world_, ledger_, built.clip,
                built.origin_voxel[0] * static_cast<i64>(coarse) + options_.clip_at[0],
                built.origin_voxel[1] * static_cast<i64>(coarse) + options_.clip_at[1],
                built.origin_voxel[2] * static_cast<i64>(coarse) + options_.clip_at[2],
                PasteMode::SolidOnly, MatterReason::PlayerPlace, 1, &jobs, types_.type_count(),
                coarse);
            const u64 pasted_at = now_ns();
            if (stamped.chunks_left_empty) world_.compact();

            // Hold on to everything the ladder needs. The script owns the field, which the
            // background sampler reads and nothing else touches once parsing is done.
            if (coarse > 1) {
                refine_cache_path_ = options_.no_clip_cache ? std::string() : cache_path;
                refine_cache_key_ = key;
                refine_authored_ = (options_.clip_metre > 0) ? options_.clip_metre
                                                             : script.settings.voxels_per_metre *
                                                                   static_cast<i32>(coarse);
                refine_scale_ = coarse;
                refine_at_[0] = options_.clip_at[0];
                refine_at_[1] = options_.clip_at[1];
                refine_at_[2] = options_.clip_at[2];
                refine_script_ = std::make_unique<forge::Script>(std::move(script));
                plan_refine_regions(*refine_script_);
                WS_LOG_INFO("clip", "{} regions to sharpen, biggest on screen first",
                            refine_regions_.size());
            }
            // The two ns figures are summed across worker threads, so they exceed the wall clock
            // on a parallel build. What they are for is the RATIO: how much of the sampling is
            // actually inside the field, and how much is everything around it.
            WS_LOG_INFO("clip", "parse {:.0f} ms, sample {:.0f} ms ({} shape + {} paint, "
                                "{} voxels asked, {} settled in bulk; {:.0f} ms shape + {:.0f} ms "
                                "paint across all threads), "
                                "variation {:.0f} ms, paste {:.0f} ms, compact {:.0f} ms",
                        ns_to_ms(parsed_at - start), ns_to_ms(sampled_at - parsed_at),
                        built.shape_evaluations, built.paint_evaluations, built.voxels_asked,
                        built.voxels_settled, ns_to_ms(built.shape_ns), ns_to_ms(built.paint_ns),
                        ns_to_ms(varied_at - sampled_at),
                        ns_to_ms(pasted_at - varied_at), ns_to_ms(now_ns() - pasted_at));
            WS_LOG_INFO("clip", "slack {:.4f} m worst, {:.4f} m to settle a box, {:.4f} m for the "
                                "easiest of {} parts",
                        built.slack, built.prune_slack, built.best_part_slack, built.parts);
            // The palette, taken from whichever copy of the script still HAS one.
            //
            // **This is what "changing material with Q and E no longer works" was.** Twenty lines
            // above, a build that runs the sharpening ladder does
            // `refine_script_ = make_unique<Script>(std::move(script))` — and this line then read
            // `material_types` out of the moved-from object, which is empty. So `materials_` fell
            // through to the one-entry fallback below and Q and E cycled a list with nowhere to go.
            // A palette of ONE and a key that does not work are the same report from the other side
            // of the screen, which is the sentence the cached path's own comment already carries
            // (see `palette_from` above) — this is that fault arriving through the second door,
            // because the cached path never moves the script and the built path always does.
            //
            // It bites only on a build that runs the ladder, which is why a cached load has been
            // fine throughout and nothing in the automated suite caught it: every headless run in
            // this repository loads a settled world from the cache.
            const forge::Script& palette_script = refine_script_ ? *refine_script_ : script;
            materials_ = palette_script.material_types;
            if (materials_.empty()) materials_.push_back(1);
            material_index_ = options_.material % materials_.size();
            chisel_.set_material(materials_[material_index_]);
            WS_LOG_INFO("tool", "palette: {} materials from the clip", materials_.size());
            const WorldStats clip_stats = world_.stats();
            WS_LOG_INFO("world", "'{}' built in {:.0f} ms: {} chunks, {} solid voxels", path,
                        ns_to_ms(now_ns() - start), clip_stats.chunks, clip_stats.solid_voxels);

            // Kept, so the next run does not do any of that again ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but NOT while it is still
            // arriving. A coarse build is a stage on the way to the real world, and writing it here
            // would cache the blocky one with nothing recording that it is blocky. On the ladder
            // the write is save_refined_world's, which happens at the fixed point and carries the
            // list of which boxes are sharp; here there is no ladder and the world is already what
            // the clip asked for.
            if (!options_.no_clip_cache && coarse <= 1) {
                progress_.enter(LoadStage::Caching);
                WorldCache cache;
                cache.tags = &tags_;
                cache.properties = &properties_;
                cache.types = &types_;
                cache.world = &world_;
                cache.ledger = &ledger_;
                cache.materials = materials_;
                write_world_cache(cache_path, key, cache);
            }
            return;
        }
        // Nothing to fall back to, and that is deliberate. An empty world says plainly that the
        // clip did not load; a stand-in scene would say the clip loaded and looked like that.
        WS_LOG_ERROR("clip", "'{}' did not build ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the world is empty", path);
        // And it says so ON THE SCREEN, which it did not.
        //
        // "the world is empty" was a line in a log file, so what a player got was a sky with
        // nothing in it and no way to tell that from the renderer having broken -- which is
        // exactly what was reported, twice, and both times the answer was in a log nobody had
        // been given a reason to open. The FIRST error is the one that is said: the rest of any
        // list of them is what the first one caused.
        world_trouble_ = script.errors.empty()
                             ? std::string("this world built to nothing")
                             : ("did not build: " + script.errors.front().message);
        materials_.push_back(1);
        material_index_ = 0;
    }


    const WorldStats stats = world_.stats();
    WS_LOG_INFO("world",
                "test scene built in {:.0f} ms: {} chunks, {} bricks, {} solid voxels, "
                "{} MB ({:.3f} bytes/voxel)",
                ns_to_ms(now_ns() - start), stats.chunks, stats.bricks, stats.solid_voxels,
                stats.bytes / (1024 * 1024), stats.bytes_per_voxel());
}

// The chisel, and undo. Everything a player does to the world enters here and leaves as an
// op, which is the invariant the whole architecture rests on (documentation/02).
void Application::update_tools(const InputState& input, bool chisel_has_wheel,
                               bool clipboard_has_wheel, f64 dt) {
    constexpr u32 kLocalPlayer = 1;

    // A scripted edit, applied part way through the run rather than while the world is
    // being built. That is deliberate: an edit made before streaming exists proves nothing,
    // and the interesting question is whether an edit to an already-resident chunk reaches
    // the GPU. Applying it at a known frame makes that testable from a screenshot.
    // Late by choice, when asked. An edit at frame 100 lands while the shadow entries around
    // it are still filling, so it cannot show whether an edit reaches surfaces that have
    // already converged ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is the case a player is actually in, and the one where new
    // shadows were reported missing.
    const u64 kScriptedEditFrame = (options_.edit_frame > 0) ? options_.edit_frame : 100;
    if (!options_.edit.empty() && frame_counter_ == kScriptedEditFrame) {
        // (the scripted clip below shares this frame number)
        i64 values[7]{0, 0, 0, 0, 0, 0, 0};
        parse_numbers(options_.edit, values, 7);
        const VoxelTypeId type =
            (values[6] <= 0) ? kAir
                             : materials_[static_cast<usize>(values[6] - 1) % materials_.size()];
        const Op op = Op::fill_box(tick_++, kLocalPlayer, values[0], values[1], values[2],
                                   values[3], values[4], values[5], type,
                                   (type == kAir) ? MatterReason::PlayerBreak
                                                  : MatterReason::PlayerPlace);
        const u64 started = now_ns();
        // Through the same hollowing the interactive path uses, so --hollow tests the thing
        // the player gets rather than a parallel implementation of it.
        std::vector<Op> scripted;
        if (hollow_ > 0) {
            hollow_box(op, static_cast<i64>(hollow_), tick_, scripted);
        } else {
            scripted.push_back(op);
        }
        const OpResult result = history_.apply_group(world_, ledger_, op_log_, scripted);
        WS_LOG_INFO("chisel",
                    "scripted edit: {} voxels changed of {} visited in {:.3f} ms "
                    "(apply {:.3f}, undo capture {:.3f} into {} ops)",
                    result.voxels_changed, result.voxels_visited, ns_to_ms(now_ns() - started),
                    history_.last_apply_ms(), history_.last_capture_ms(),
                    history_.last_inverse_ops());
        if (result.voxels_changed > 0) {
            refresh_world_bounds();
            // The same invalidation the interactive path does. Without it the summary tree
            // never hears that these chunks exist, so nothing past the streaming range is
            // ever drawn ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which made a scripted edit behave differently from the identical
            // edit made by hand, and hid the difference behind "it works when I play it".
            invalidate_edited_chunks({op});
        }
    }

    // ---- the player who is chiselling while they fly ------------------------------------------
    //
    // The worst case the renderer has, and until now there was no way to photograph it. `--fly`
    // measures a camera revealing new faces; `--edit` measures one change to a settled picture.
    // Held together they are what a player actually does, and the two costs are not independent:
    // an edit reopens every face within `kEditShadowReach` of it, and a moving camera is claiming
    // new faces in the same region on the same frames.
    //
    // Deliberately in front of the CAMERA rather than at a fixed box. An edit behind you reopens
    // faces nothing can see, which is a measurement of the wrong thing now that light stops at
    // what a pixel read; an edit in front of you reopens exactly the faces the frame is made of,
    // which is the case that costs and the case a player is in.
    if (options_.chisel_every > 0 && frame_counter_ > 0 &&
        (frame_counter_ % options_.chisel_every) == 0) {
        // AT THE SURFACE THE CAMERA IS LOOKING AT, found by the same `raycast` the Chisel tool
        // uses, and not at a fixed distance in front of the eye.
        //
        // The first version put the box three metres along the forward vector, which is what a
        // hand-held tool looks like and is not what it does. On this flight path the camera is
        // outside the building for most of the run, so three metres ahead is open air: every
        // "carve" changed nothing and every "fill" hung a one-metre cube in the sky and took it
        // down again eight frames later. The counters said 1,437,480 voxels changed and the
        // screen showed an untouched facade, which is the handover's trap 1 in a new place -- a
        // number responding to the edits while the picture responds to nothing.
        //
        // Aimed at the surface, the edits land on the wall the frame is made of, which is also the
        // only version of this that measures the right thing: what an edit costs is every face
        // within kEditShadowReach of it, and those faces are only expensive when they are the ones
        // on screen.
        f32 forward[3];
        camera_.forward_vector(forward);
        // `Camera::position_*` is already in VOXELS, not metres -- see the note at the top of
        // game/camera.hpp about why the position is kept in voxel doubles. Scaling it again put
        // the ray origin thirty-two times too far out, so it hit nothing, and the fixed-distance
        // version this replaced was filling one-metre cubes into empty space a kilometre away:
        // 1,437,480 voxels changed, ninety-four chunks created, and an untouched facade on screen.
        const f64 eye[3] = {camera_.position_x(), camera_.position_y(), camera_.position_z()};
        const RayHit look = raycast(world_, eye[0], eye[1], eye[2], forward[0], forward[1],
                                    forward[2], chisel_.reach());
        if (look.hit) {
            const i64 r = options_.chisel_radius;
            // Carve, then fill the same box back on the next firing: the two halves of what a
            // player does, and the fill returns matter so a long run does not dissolve the
            // building and end up measuring an empty room. The box is centred on the voxel that
            // was hit, so a carve always removes something and the fill always puts something
            // back in the same place -- which is what makes the two visible as one action.
            const bool carve = ((frame_counter_ / options_.chisel_every) & 1ull) == 0ull;
            const VoxelTypeId type = carve ? kAir : materials_[0];
            const Op op =
                Op::fill_box(tick_++, kLocalPlayer, look.x - r, look.y - r, look.z - r,
                             look.x + r, look.y + r, look.z + r, type,
                             carve ? MatterReason::PlayerBreak : MatterReason::PlayerPlace);
            std::vector<Op> scripted{op};
            // Split three ways, because an edit's cost is three unrelated things and the frame
            // time cannot tell them apart: the op and its undo capture, the coarse grids, and
            // everything downstream re-deriving itself. The handover says the third is most of it
            // for a large chisel and nothing had ever measured it for a small one.
            const u64 t_apply = now_ns();
            const OpResult result = history_.apply_group(world_, ledger_, op_log_, scripted);
            const u64 t_bounds = now_ns();
            if (result.voxels_changed > 0) {
                refresh_world_bounds();
                const u64 t_invalidate = now_ns();
                invalidate_edited_chunks({op});
                chisel_bounds_ns_ += t_invalidate - t_bounds;
                chisel_invalidate_ns_ += now_ns() - t_invalidate;
            }
            chisel_apply_ns_ += t_bounds - t_apply;
            ++chisels_fired_;
            chisel_voxels_ += result.voxels_changed;
        } else {
            ++chisels_missed_;
        }
    }

    if (!options_.clip.empty() && frame_counter_ == kScriptedEditFrame) {
        i64 values[12]{0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
        parse_numbers(options_.clip, values, 12);
        // Whichever slot the clipboard lives on, and cycle to it if that slot holds more
        // than one tool. The belt's layout is a default, not a fact this code can assume.
        for (u32 slot = 0; slot < kToolSlots && toolbelt_.active() != ToolKind::Clipboard;
             ++slot) {
            if (!toolbelt_.select_slot(slot)) continue;
            for (u32 i = 0; i < toolbelt_.slot_size(slot); ++i) {
                if (toolbelt_.active() == ToolKind::Clipboard) break;
                toolbelt_.cycle(1);
            }
        }
        const u64 started = now_ns();
        Clip clip = capture_clip(world_, values[0], values[1], values[2], values[3], values[4],
                                 values[5]);
        const u64 solid = clip.solid_count();
        clipboard_.hold(std::move(clip), values[0], values[1], values[2]);
        // Signed: negative places the copies past the ghost instead of between.
        clipboard_.set_copies(static_cast<i32>(values[9]));
        if (values[10] != 0) clipboard_.turn(1, static_cast<f64>(values[10]));
        // Scale as a percentage, so 250 is two and a half times.
        if (values[11] != 0) clipboard_.resize(static_cast<f64>(values[11]) / 100.0);
        clipboard_.set_offset(values[6], values[7], values[8]);
        WS_LOG_INFO("clip",
                    "scripted clip: {}x{}x{} cells, {} solid, scale {:.3f}, {} copies, "
                    "set up in {:.3f} ms (bake {:.3f} ms, {} cells baked{})",
                    clipboard_.clip().size[0], clipboard_.clip().size[1],
                    clipboard_.clip().size[2], solid, clipboard_.scale()[0],
                    clipboard_.copies(), ns_to_ms(now_ns() - started),
                    clipboard_.last_bake_ms(), clipboard_.baked_cells(),
                    clipboard_.baking_truncated() ? ", TRUNCATED" : "");
    }

    if ((input.was_pressed(Key::Q) || input.was_pressed(Key::E)) && !materials_.empty()) {
        const usize step = input.was_pressed(Key::E) ? usize{1} : materials_.size() - 1;
        material_index_ = (material_index_ + step) % materials_.size();
        chisel_.set_material(materials_[material_index_]);
        // Said out loud, because "nothing happened" and "it happened and nothing shows it" are the
        // same report from the other side of the screen, and the palette is not on screen unless
        // the developer panel is open. One line per keypress costs nothing.
        WS_LOG_INFO("tool", "material {} of {} (type {})", material_index_ + 1, materials_.size(),
                    chisel_.material());
    }

    // Undo on Z or Ctrl+Z, redo on Y or Ctrl+Y, and both repeat while held.
    //
    // Undoing thirty steps should be one long press, not thirty presses. The repeat is the same
    // time-based one the clipboard's counters use: a pause before it starts, so a single tap is
    // still a single step, then steadily.
    //
    // X is NOT redo, and used to be. X drops a constraint point (chisel.hpp), so every point
    // dropped also redid a step of history -- and a redo that has something to redo puts back an
    // edit the player had deliberately undone. One key with two meanings, where the meaning nobody
    // asked for is silent until the history is non-empty, which is exactly when it does harm.
    // Scripted, on exactly the same path the key takes -- not a second implementation beside it,
    // which is how the two would drift and how this fault would come back invisible. See
    // Options::undo_frame.
    const bool scripted_undo = options_.undo_frame > 0 && frame_counter_ == options_.undo_frame;
    const bool scripted_redo = options_.redo_frame > 0 && frame_counter_ == options_.redo_frame;
    const bool undo_down = input.is_down(Key::Z) || scripted_undo;
    const bool redo_down = input.is_down(Key::Y) || scripted_redo;
    //
    // Both take the SAME two steps a chisel stroke does: refresh the coarse grids, then tell the
    // renderer which region changed. Only the first was here, so an undo put the world back and
    // left the picture alone -- the node pool is what the game marches and nothing had told it,
    // which is the seam D225 describes reached through the one edit path not carrying its ops.
    std::vector<Op> stepped;
    if (repeat_undo_.poll(undo_down, dt) > 0) {
        const bool did = history_.undo(world_, ledger_, op_log_, tick_++, kLocalPlayer, stepped);
        WS_LOG_INFO("tool", "undo: {}", did ? "one step back" : "nothing to undo");
        if (did) {
            refresh_world_bounds();
            invalidate_edited_chunks(stepped);
        }
    }
    if (repeat_redo_.poll(redo_down, dt) > 0) {
        const bool did = history_.redo(world_, ledger_, op_log_, tick_++, kLocalPlayer, stepped);
        WS_LOG_INFO("tool", "redo: {}", did ? "one step forward" : "nothing to redo");
        if (did) {
            refresh_world_bounds();
            invalidate_edited_chunks(stepped);
        }
    }

    // The buttons only reach a tool once the mouse belongs to the world.
    const bool left = mouse_look_ && !swallow_click_ && input.mouse_left;
    const bool right = mouse_look_ && !swallow_click_ && input.mouse_right;
    const bool middle = mouse_look_ && !swallow_click_ && input.mouse_middle;

    const f64 origin[3] = {camera_.position_x(), camera_.position_y(), camera_.position_z()};
    f32 forward[3];
    camera_.forward_vector(forward);
    const f64 direction[3] = {forward[0], forward[1], forward[2]};

    std::vector<Op> ops;
    if (toolbelt_.active() == ToolKind::Clipboard) {
        clipboard_.set_hollow(hollow_);
        ClipboardInput tool{};
        tool.left = left;
        tool.right = right;
        tool.middle = middle;
        tool.add_point = repeat_add_point_.poll(input.is_down(Key::X), dt) > 0;
        tool.wheel = clipboard_has_wheel ? input.wheel : 0.0f;
        tool.big_step = input.is_down(Key::Shift);
        tool.adjust_distance = chisel_has_wheel;
        if (chisel_has_wheel) tool.wheel = input.wheel;
        tool.toggle_paste_mode = input.was_pressed(Key::P);
        tool.toggle_snap = input.was_pressed(Key::O);
        tool.cycle_adjust = input.was_pressed(Key::Slash);
        tool.clear_points = input.was_pressed(Key::R);
        tool.cancel = input.was_pressed(Key::Backspace);
        tool.increase = repeat_more_.poll(input.is_down(Key::Period), dt);
        tool.decrease = repeat_fewer_.poll(input.is_down(Key::Comma), dt);
        tool.turn_left = repeat_turn_[0].poll(input.is_down(Key::Left), dt);
        tool.turn_right = repeat_turn_[1].poll(input.is_down(Key::Right), dt);
        tool.turn_up = repeat_turn_[2].poll(input.is_down(Key::Up), dt);
        tool.turn_down = repeat_turn_[3].poll(input.is_down(Key::Down), dt);
        if (!clipboard_.update(world_, tool, origin, direction, dt, tick_, kLocalPlayer, ops)) {
            return;
        }
    } else {
        ChiselInput tool{};
        tool.left = left;
        tool.right = right;
        tool.add_point = repeat_add_point_.poll(input.is_down(Key::X), dt) > 0;
        tool.wheel = chisel_has_wheel ? input.wheel : 0.0f;
        tool.adjust_distance = chisel_has_wheel;
        tool.clear_points = input.was_pressed(Key::R);
        tool.cancel = input.was_pressed(Key::Backspace);
        tool.toggle_overwrite = input.was_pressed(Key::P);
        tool.toggle_anchor = input.was_pressed(Key::O);

        Op op;
        if (!chisel_.update(world_, tool, origin, direction, tick_, kLocalPlayer, op)) return;

        // A hollow box is six slabs with the middle left alone ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â untouched rather than
        // emptied, so placing a hollow shape inside a hill builds walls in it instead of
        // scooping the hill out.
        if (hollow_ > 0) {
            u64 id = op.tick;
            hollow_box(op, static_cast<i64>(hollow_), id, ops);
        } else {
            ops.push_back(op);
        }
    }

    const u64 started = now_ns();
    const OpResult result = history_.apply_group(world_, ledger_, op_log_, ops);
    ++tick_;
    last_edit_ms_ = ns_to_ms(now_ns() - started);
    last_edit_voxels_ = result.voxels_changed;

    // An edit can create chunks where the world had none, or empty the last brick out of
    // one, so how far the world reaches has to be worked out again -- it is what a ray is
    // clipped to. Telling the node pool what changed is the line below it.
    f64 bounds_ms = 0.0;
    f64 announce_ms = 0.0;
    if (result.voxels_changed > 0) {
        const u64 bounds_began = now_ns();
        refresh_world_bounds();
        bounds_ms = ns_to_ms(now_ns() - bounds_began);
        const u64 announce_began = now_ns();
        invalidate_edited_chunks(ops);
        announce_ms = ns_to_ms(now_ns() - announce_began);
    }

    // Split for the same reason the region paste's was (D511, trap 17), and reported at a
    // threshold rather than every stroke: an ordinary chisel is a fraction of a millisecond and a
    // line per click would bury the one that is not. What §5 records for a large delete is a frame
    // of 1,209 ms of which the op is 68 and the undo capture 240 -- so most of it is somewhere
    // below this line, and nothing has ever said where.
    if (last_edit_ms_ + bounds_ms + announce_ms > 50.0) {
        WS_LOG_INFO("edit",
                    "large edit: {} voxels in {:.0f} ms (apply {:.0f} + world bounds {:.0f} + "
                    "announce {:.0f}), {} ops",
                    last_edit_voxels_, last_edit_ms_ + bounds_ms + announce_ms, last_edit_ms_,
                    bounds_ms, announce_ms, ops.size());
    }
}

// Tells streaming which chunks an edit changed.
//
// This has to be pushed, because it cannot be pulled. The renderer's feedback reports chunks
// it wanted and *could not find* ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â that is the whole mechanism. A chunk that is resident but
// out of date is found, so it is never reported, so it is never refreshed, and it goes on
// showing what it used to hold. Carve a room inside a hill you have already looked at and
// the hill stays solid until something unrelated evicts that chunk.
//
// The edit knows exactly which chunks it touched, so it says so.
void Application::invalidate_edited_chunks(const std::vector<Op>& ops) {
    bool first = true;
    i64 lo[3]{};
    i64 hi[3]{};
    for (const Op& raw : ops) {
        Op op = raw;
        op.normalise();
        const i64 op_lo[3] = {op.x0, op.y0, op.z0};
        const i64 op_hi[3] = {op.x1, op.y1, op.z1};
        for (u32 axis = 0; axis < 3; ++axis) {
            lo[axis] = first ? op_lo[axis] : std::min(lo[axis], op_lo[axis]);
            hi[axis] = first ? op_hi[axis] : std::max(hi[axis], op_hi[axis]);
        }
        first = false;
    }
    if (first) return;   // no ops, so nothing changed
    announce_world_change(lo, hi);
}

// One box of the world is not what the renderer is holding a copy of, and everything that holds
// one is told here.
//
// Split out of `invalidate_edited_chunks` because an EDIT is not the only thing that writes to the
// world. The clip ladder sharpens the building region by region in the background and pastes each
// box straight into the world's bricks (`pump_refinement`), and for as long as this was reachable
// only from the edit path, nothing downstream ever heard about any of it. Measured, on the same
// world at the same camera with the same content hash, one run watching it sharpen and one loading
// the finished article: the node pool held **7,497 of its 17,344 leaves** in a shape the world no
// longer had, and the ambient term -- which since R10d converges and then stops casting rays
// altogether -- differed by a mean of **19.3 of 255 over 53% of the frame**, permanently. See D397.
//
// It takes a BOX rather than a list of ops because the paste is not made of ops. The edit path
// unions its group and hands the union over, which is what the light window already used.
void Application::announce_world_change(const i64 lo[3], const i64 hi[3]) {
    // And for a couple of seconds afterwards, every surface re-measures its shadow.
    //
    // A converged face stops tracing shadow rays and is only refreshed by a two per cent
    // trickle. Close to the camera a face is covered by hundreds of pixels, so two per cent of
    // them is a steady stream and a new shadow arrives at once; at distance a face is covered
    // by one pixel or less, two per cent of that is nothing, and the shadow of something just
    // placed never appears. Worse, a face below kShadowSeed samples leans on a parent node
    // sixty-four voxels across, which is dominated by surface that is still lit ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so the new
    // shadow is not merely late, it is actively averaged away.
    //
    // That is exactly the report: new voxels cast no shadow until the camera comes close, and
    // then it fades in and stays. Coming close is what finally supplies the samples.
    //
    // So an edit says "look again" to everything, briefly. Not a wipe of the cache ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â that was
    // tried and it is the smearing, every voxel placed relighting the whole scene at once.
    // This keeps every measured value and simply re-measures faster for a moment.
    shadow_refresh_frames_ = kShadowRefreshFrames;
    edit_window_opened_ = true;   // see the member: the faces in the box drop their history once
    lights_dirty_ = true;   // a placed lamp is a light nothing can aim at until this is rebuilt
    // ...and WHICH chunks have to be looked at again, which is the whole of R9g's first half.
    //
    // Finding the emitters was a walk of every brick of every chunk, run from scratch on every one
    // of these announcements -- every chisel stroke and, since D397, every region the clip ladder
    // pastes. Measured on the facility before this: **14.15 ms on average and 14.99 at worst, to
    // rediscover the same twenty-one fittings**, against the edit that provoked it costing 0.19 ms
    // to apply and undo. That is `rebuild_coarse_grids` exactly (D522, O(world) for a change one
    // metre across) four times over, and no line anywhere printed it.
    //
    // A cluster cell is four voxels and a chunk is 256, so no cell straddles a chunk and a chunk's
    // cells can simply be kept and concatenated. Only the chunks the edited box touches are dropped
    // from the cache; the merge below still sees every cell in the world, so a fitting that
    // straddles a boundary is unaffected and the list is identical either way.
    for (i64 cz = floor_div_i64(lo[2], kChunkEdge); cz <= floor_div_i64(hi[2], kChunkEdge); ++cz) {
        for (i64 cy = floor_div_i64(lo[1], kChunkEdge); cy <= floor_div_i64(hi[1], kChunkEdge);
             ++cy) {
            for (i64 cx = floor_div_i64(lo[0], kChunkEdge); cx <= floor_div_i64(hi[0], kChunkEdge);
                 ++cx) {
                emitter_cache_.erase(ChunkCoord{cx, cy, cz});
            }
        }
    }

    // And it says it to the region rather than to the world. Everything the change could have
    // altered the light of is inside its own bounds grown by the reach of a shadow; nothing
    // outside that can have changed at all, so nothing outside is disturbed.
    for (u32 axis = 0; axis < 3; ++axis) {
        edit_lo_[axis] = lo[axis] - kEditShadowReach;
        edit_hi_[axis] = hi[axis] + kEditShadowReach;
    }

    // The chunk half of this announcement -- invalidate every chunk in the box, then request it
    // back so a shadow ray would find it -- went with `world/residency.*` (R1e). It was talking to
    // a streaming system no shader read any more. What replaces it is the line below, which says
    // the same thing to the tree the renderer actually walks.


    // And the node pool, which was never told at all.
    //
    // Every structure above is a chunk structure, and the marcher stopped reading those when
    // the node pool became the default. So carving or placing changed the world, refreshed
    // four things nothing was drawing from, and left the tree the renderer actually walks
    // holding the world as it was before the edit -- which on screen is an edit that does
    // nothing whatsoever. Reported by the player, and it is the plainest possible symptom of
    // the seam R1e exists to remove.
    //
    // Per brick rather than per chunk, because that is the pool's leaf and `invalidate` drops
    // the node and every ancestor folded from it. The box arrives the right way round.
    //
    // NOT accompanied by a request, and that is measured rather than assumed. An invalidated
    // node is a node the pool does not have, and D302 makes a cell the pool does not have an
    // OCCLUDER to a shadow ray -- so a freshly carved hole is opaque to the sun until the pool
    // rebuilds it, and rebuilding is driven by feedback from PRIMARY rays. Asking for the
    // bricks back here looks like the fix, and the chunk half of this function is written
    // around exactly that argument. It was tried: it moved no number on the carved-skylight
    // case in three hundred frames, while costing a request per brick per edit, and the pool's
    // own counters showed nothing deferred and nothing starved. So it is not here.
    //
    // The case is real and is written up as open work -- see the shadow-latency section of
    // documentation/13-decision-log.md.
    // The BOX, not every brick in it (D515). Naming the bricks made the caller enumerate a volume
    // and the pool deduplicate it, and neither of them knows anything useful about a brick that
    // was never built -- which on a large delete is nearly all of them: 1,573,269 announced,
    // 13,325 nodes actually refreshed, 714 ms to find that out. The pool holds the tree, so the
    // pool prunes.
    node_pool_.invalidate_box(lo, hi);
}

// How far the world reaches, which is how far a ray may usefully travel: `bounds_min` and
// `bounds_max` in the parameter block clip every ray to this box.
//
// This was `rebuild_coarse_grids`, and the five coarse occupancy grids it rebuilt were **by a
// wide margin the largest thing an edit cost** -- 4.10 ms of CPU for a change one metre across,
// because those grids are O(the world) and a wrapped chunk grid cannot be told about a box. They
// were a chunk structure read by a marcher that no longer exists, so R1e deletes them rather
// than optimising them. What is left is the sweep for the world's extent, which is O(chunks)
// rather than O(bricks).
//
// Level 0 of those grids ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the one that answers "the world has a chunk here and you do not
// have it" ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â is only recorded within a window around this point, because that is what stops
// two distant regions colliding in the wrapped grid and inventing chunks that do not exist.
// So the grids have to follow the camera, not only world edits.
void Application::refresh_world_bounds() {

    // Deliberately *not* clearing the face cache here any more.
    //
    // Wiping the table on every edit meant each voxel placed relit the whole scene at once,
    // which is what the smearing while building actually was. Entries now average over a
    // sliding window instead, so a face notices what was built beside it within a few frames
    // on its own, and nothing further away flinches. See kFaceWindow in pathtrace.comp.

    // And the one place to note how far the world reaches, which is how far a ray can
    // usefully travel now that thumbnails draw well past what is resident.
    world_bounds_valid_ = false;
    world_.for_each_chunk([this](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        if (!world_bounds_valid_) {
            world_min_ = coord;
            world_max_ = coord;
            world_bounds_valid_ = true;
            return;
        }
        world_min_.x = std::min(world_min_.x, coord.x);
        world_min_.y = std::min(world_min_.y, coord.y);
        world_min_.z = std::min(world_min_.z, coord.z);
        world_max_.x = std::max(world_max_.x, coord.x);
        world_max_.y = std::max(world_max_.y, coord.y);
        world_max_.z = std::max(world_max_.z, coord.z);
    });
}

void Application::stream(f64 seconds) {
    (void)seconds;


    // What the renderer asked for, two frames ago. This is the rule that makes residency follow
    // the *view* rather than the camera position -- geometry 300 m away that covers a hundred
    // pixels gets streamed; geometry 10 m away behind a wall does not.
    const std::vector<FeedbackEntry>& wanted = feedback_.read(swapchain_.frame_index());
    last_feedback_ = feedback_.last_reported();
    last_feedback_truncated_ = feedback_.last_truncated();


    // The reports, in one pass.
    u32 faces_seen = 0;
    u32 faces_read_reported = 0;
    // Miss reports for a place the world has nothing at all.
    //
    // A few are normal: a ray inside an occupied block still asks about cells that turn out to be
    // empty. A *large* number is D133 -- a ray reports only its nearest miss, so a phantom hides
    // the real geometry behind it for ever and the same nothing is asked for every frame.
    // `index_world` is what makes that structurally impossible in the node pool, since a root
    // exists wherever the world does. Counted anyway, because it costs one hash on a miss report
    // and the failure it catches is silent.
    u32 phantom = 0;
    {
        for (const FeedbackEntry& entry : wanted) {
            // A ray that READ this node, rather than one that could not find it.
            //
            // Residency had only ever heard about misses, so the moment the tree was complete it
            // heard nothing at all and evicted the scene on a timer (D247). A hit costs one entry
            // per visible root per frame and is what makes "wanted" mean wanted.
            // A face the eye can see: the node the ray stopped on and the direction it was hit
            // from. Claimed rather than requested -- there is nothing to build, only somewhere
            // for the light pass to put an answer.
            if ((entry.level & kFeedbackFace) != 0) {
                const u32 level = static_cast<u32>(entry.level & 0xFF);
                const u32 face = static_cast<u32>((entry.level >> 8) & 0xFF);
                if (level > kMaxNodeLevel || face >= kFaceCount) continue;
                // A face a LIGHT ray landed on, rather than one a pixel landed on. R9a.
                //
                // The claim is the same claim -- there is nothing to build and nothing to stream, so
                // this cannot move a single node into the pool and R9h's rule is intact. What the
                // class changes is what happens when there is no room: a secondary claim is DECLINED
                // against its own cap, and the on-screen set never sees it.
                const bool secondary = (entry.level & kFeedbackSecondary) != 0;
                bool first_time = false;
                const u32 slot =
                    face_store_.claim(FaceKey{entry.x, entry.y, entry.z, level, face},
                                      frame_counter_, &first_time, secondary);
                if (secondary) {
                    ++faces_secondary_offered_;
                    if (slot != kNoFace && first_time) ++faces_secondary_claimed_;
                    // No stand-in for a secondary face, and that is not an omission. A stand-in
                    // exists so the COMPOSITE has something to read while a fine face is being
                    // found (R9d), and no pixel is reading this one; claiming one would be a second
                    // face, at a coarse level shared by five hundred and twelve others, for a
                    // gathering ray that already has its own answer for the coarse case.
                    continue;
                }
                ++faces_seen;

                // And the coarse face standing over it, which the marcher reads while this one is
                // still being found. Derived here rather than reported, because an ancestor key is
                // a shift of its descendant's and a coordinate computable from another coordinate
                // is not information: sending it would double the face traffic through a buffer
                // that is already the binding constraint, and buy nothing.
                //
                // What it buys instead is the wait. A face is claimed only when a primary ray lands
                // on it, at one pixel in sixty-four, so a surface that was hidden behind something
                // and is now visible has no light of its own for about a second ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and the composite
                // falls back to full sun on it, which indoors is the most wrong answer available.
                // Five hundred and twelve fine faces share one stand-in, so the stand-in is claimed
                // the frame the region appears and settled a few frames later, and what a player
                // sees is a blocky shadow sharpening rather than no shadow arriving. R9d.
                //
                // Only when the face under it is NEW, which is the whole reason `was_new` exists.
                // Doing it on every report is 16,000 extra probes a frame that change nothing ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
                // measured at 0.24 ms of CPU while turning ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and the case they would serve cannot
                // occur: a stand-in is wanted for geometry the store has not seen, and geometry the
                // store has not seen has no repeat reports to hang the claim off. What the repeats
                // would buy is keeping a stand-in warm past its cold window while its children stay
                // live, and a stand-in whose children are all live is a stand-in nothing reads.
                //
                // ...and it is claimed as a STAND-IN rather than as an ordinary face, which is the
                // one fact the store cannot derive for itself and the whole of what lets it keep
                // the coarse pyramid after the fine faces are gone (R9f). The paragraph above ends
                // by dismissing "keeping a stand-in warm past its cold window while its children
                // stay live" as buying nothing, and that was right about the mechanism and wrong
                // about the case: a stand-in whose children are all live is indeed read by nobody,
                // and the moment the camera leaves, the children go and the stand-in is what the
                // room has to be rebuilt from. See FaceStore::claim_stand_in.
                const u32 coarse_level = level + kFaceAncestorStep;
                if (first_time && coarse_level <= kMaxNodeLevel) {
                    face_store_.claim_stand_in(
                        FaceKey{entry.x >> kFaceAncestorStep, entry.y >> kFaceAncestorStep,
                                entry.z >> kFaceAncestorStep, coarse_level, face},
                        frame_counter_);
                }
                continue;
            }

            // A face a PIXEL read, named by slot. Not a claim: the face is already here, and this
            // says only that it is on the screen right now.
            //
            // It is the store's residency clock, and until D508 there was not one. `claim` stamped
            // `last_read_`, so "somebody is looking at this" was really "the request lattice got
            // round to asking about this" -- one pixel in stride^2, which a face covering less than
            // a pixel goes many periods without being picked by. That is why the only safe cold
            // window was ten seconds, why the store kept everything the camera had walked past, and
            // why it filled and started refusing faces. D502.
            if ((entry.level & kFeedbackFaceRead) != 0) {
                face_store_.touch(static_cast<u32>(entry.x), frame_counter_);
                // ...and it belongs to the on-screen set now, whatever put it here. This is the
                // exact signal for that -- the visibility pass sends it for every face a pixel
                // resolves to -- and without it a face claimed by a bounce ray and then walked up to
                // would count against the off-screen cap for the rest of its life. R9b's cap has to
                // hold down light nobody is looking at, never light somebody is.
                face_store_.promote(static_cast<u32>(entry.x));
                ++faces_read_reported;
                continue;
            }

            // A node read by a ray, named by slot. Per node rather than per root, because
            // eviction at the root can only keep the scene whole or drop it whole (D260).
            if ((entry.level & kFeedbackRead) != 0) {
                node_pool_.touch_slot(static_cast<u32>(entry.x));
                continue;
            }
            if ((entry.level & kFeedbackUsed) != 0) {
                const u32 level = static_cast<u32>(entry.level & ~kFeedbackUsed);
                if (level > kMaxNodeLevel) continue;
                node_pool_.touch(NodeKey{entry.x, entry.y, entry.z, level});
                continue;
            }

            // An EXACT report names the one cell that stopped a shadow ray, so it is requested and
            // nothing around it is. The dilation below exists because a miss report is a guess and
            // its neighbours are probably wanted too; this is not a guess, and at about fifty
            // thousand of them a frame the six spare requests each are where the node pool's CPU
            // was going (D351).
            const bool exact = (entry.level & kFeedbackExact) != 0;
            const u32 level = static_cast<u32>(entry.level & ~kFeedbackExact);
            if (level < kLeafLevel || level > kMaxNodeLevel) continue;
            // The chunk a missing node falls in is a superset test for "the world has nothing
            // here", and a cheap one.
            if (!world_.has_chunk(chunk_coord_of(static_cast<i64>(entry.x) << level,
                                                 static_cast<i64>(entry.y) << level,
                                                 static_cast<i64>(entry.z) << level))) {
                ++phantom;
            }
            node_pool_.request(NodeKey{entry.x, entry.y, entry.z, level},
                               exact ? kRequestOcclusion : kRequestRay);
            if (exact) continue;

            // And the six face neighbours, for the same reason the chunk path dilates: a ray
            // reports one node, so only the nodes some ray happened to land on would ever be
            // built, which left visible notches along the edges of what had streamed. A
            // neighbour that is already there costs one hash and a hit.
            const NodeKey around[6] = {
                {entry.x - 1, entry.y, entry.z, level}, {entry.x + 1, entry.y, entry.z, level},
                {entry.x, entry.y - 1, entry.z, level}, {entry.x, entry.y + 1, entry.z, level},
                {entry.x, entry.y, entry.z - 1, level}, {entry.x, entry.y, entry.z + 1, level},
            };
            // Not `near`: windows.h still defines it as an empty macro, so a loop variable
            // by that name is a syntax error with no mention of macros anywhere in it.
            for (const NodeKey& adjacent : around) {
                node_pool_.request(adjacent, kRequestDilated);
            }
        }
    }

    last_faces_seen_ = faces_seen;
    last_faces_read_reported_ = faces_read_reported;
    last_feedback_phantom_ = phantom;


    // And give up the faces nobody asked for, which nothing did until now.
    //
    // `FaceStore::evict_cold` was written with the store, tested with the store, and never called.
    // A store that only grows reaches its cap and then refuses every face after it, and the shape
    // of that failure is not the one you would guess: shadows do not stop being DRAWN. The faces
    // that already have light keep it, so the picture holds whatever set of shadowed surfaces it
    // had when the table filled, and everything discovered after that is lit by the composite's
    // fallback. Reported as "after some time the shadowed voxel faces stop being produced and it
    // stays as whatever was produced before", which is the mechanism described exactly.
    //
    // It is new in practice rather than in theory: at brick granularity one camera claimed about
    // nineteen thousand faces against a budget of a million, so a session would rarely reach it.
    // Per voxel the same camera claims four hundred and seventy-seven thousand, and two or three
    // positions fill the table.
    //
    // Runs every frame regardless of which marcher is drawing, because the store is claimed from
    // whichever one ran and a store that stops being swept while the chunk marcher is up would
    // come back full.
    // What the lattice's period IS this frame, before deciding what "cold" means.
    //
    // `visibility.comp` doubles the face-request stride until pixels/stride^2 is under sixty
    // thousand, so the period a face waits to be claimed again doubles with the resolution. The
    // store's eviction floor is derived from it, and the same arithmetic is written twice on
    // purpose rather than pushed through the parameter block: the shader's copy decides which
    // pixels report and this one decides what the host may throw away, and if they ever drift the
    // store gives up faces the lattice has not got round to. Keep them together.
    {
        const u32 pixels = std::max(1u, render_target_.extent.width * render_target_.extent.height);
        u32 stride = 4;
        while ((pixels / (stride * stride)) > 60000u) stride <<= 1;
        // With D508 in, the slowest a face ON SCREEN refreshes its stamp is `face_read_period`, and
        // that is the number the floor is about -- the lattice is then only a second opinion. With
        // the reports off it is the lattice again, and at 4K that is 256 frames rather than 64.
        face_store_.set_claim_period(options_.face_read_period > 0 ? options_.face_read_period
                                                                   : stride * stride);
    }
    face_store_.evict_cold(frame_counter_);

    // ...and SAY SO, in the log, while somebody is playing.
    //
    // Every number about this store was printed at a screenshot and nowhere else, so the one state
    // that produces the reported picture — a full table refusing faces — was invisible in exactly
    // the situation it was reported from. A player cannot take a screenshot with `--settle` and read
    // a pass table; they can play until it looks wrong, quit, and hand over a log. That log has to
    // have the answer in it already, because the alternative is another round of guessing at their
    // session from a repro of mine. Trap 14, one level further out: look at what THEIR run says.
    //
    // Rate limited to once a second while it is happening, and it says both halves — how full the
    // table is and how many faces it turned away — because "nearly full" and "turning faces away"
    // have different answers and the second is the harm.
    {
        const FaceStoreStats live = face_store_.stats();
        const bool refusing = live.refusals > last_face_refusals_;
        if (refusing && frame_counter_ - last_face_warn_frame_ >= 60) {
            WS_LOG_WARN("faces",
                        "the face store turned away {} faces in the last {} frames: {} of {} slots "
                        "live, cold window {} frames. Surfaces past this point have no light of "
                        "their own and fall back to a coarse stand-in that re-measures every frame "
                        "-- which is what blocky flickering light IS. See D502",
                        live.refusals - last_face_refusals_,
                        frame_counter_ - last_face_warn_frame_, live.faces, face_budget_max_,
                        live.cold_window);
            last_face_warn_frame_ = frame_counter_;
        }
        last_face_refusals_ = live.refusals;

        // And a heartbeat, whether or not anything is wrong, because "the log says nothing" and
        // "the log says it was fine" are different answers and only the second one is evidence.
        // Every ten seconds is nothing in a log a session writes anyway, and it is what makes a
        // player's own run readable afterwards without asking them to reproduce under a flag.
        if (frame_counter_ > 0 && frame_counter_ % 600 == 0) {
            WS_LOG_INFO("faces",
                        "store at frame {}: {} of {} slots live, {} evicted, {} turned away, "
                        "cold window {} frames (floor {})",
                        frame_counter_, live.faces, face_budget_max_, live.evictions,
                        live.refusals, live.cold_window, face_store_.min_cold());
        }
    }

    // The two-chunk radius that used to be requested around the camera here is `NodePool::refine`
    // now -- twenty metres at brick detail, asked of the world rather than of a volume, resumable
    // and bounded (R2c, D270-D272). It is a better version of the same rule and it was running
    // beside this one.
}

// Where the quality decision is remembered between runs. Beside the logs and the crash
// reports, under %LOCALAPPDATA%, because an installed copy may sit somewhere unwritable.
namespace {
std::string settings_path() {
    const std::string& dir = crash_log_dir();
    if (dir.empty()) return {};
    return dir + "settings.txt";
}
}  // namespace

// One frame of interface, over the world.
//
// It runs BEFORE the frame is recorded, because what it decides changes the frame: a render scale
// typed into a slider has to be applied before the targets are sized, and a world left has to stop
// the loop before it draws one more picture of a world that is going away.
void Application::run_shell(f64 seconds) {
    shell_drawn_ = false;
    if (!shell_pass_.valid()) return;
    if (!shell_pass_.ensure(window_.width(), window_.height())) return;

    shell_.set_stage(ui::Stage::World);
    // Once, on the first frame of interface this world gets. Long enough to read and to act on:
    // a world that says why it is empty is a world a player can go and look at the file of.
    if (!world_trouble_.empty()) {
        shell_.say(world_trouble_, 12.0);
        world_trouble_.clear();
    }
    // The numbers the overlay says, handed over before the frame that draws them. The shell owns
    // where it goes and what it looks like; this owns what is true.
    ui::Overlay& overlay = shell_.overlay();
    overlay.on = hud_.overlay();
    overlay.fps = (stats_.last_ms() > 0.0) ? 1000.0 / stats_.last_ms() : 0.0;
    overlay.frame_ms = stats_.last_ms();
    overlay.worst_ms = stats_.percentile_ms(0.99);
    overlay.gpu_ms = profiler_.total_gpu_ms();
    overlay.width = swapchain_.extent().width;
    overlay.height = swapchain_.extent().height;
    const ui::Verdict verdict =
        shell_.frame(window_.input(), window_.width(), window_.height(), seconds);
    // Composition is on only while something is being typed into. Leaving it on puts an
    // input-method window over the game, for the whole session, on a machine set up for a language
    // that needs one.
    window_.set_text_input(shell_.ui().wants_text_input());
    // Kept up to date rather than written on the way out, at most once a second (D496).
    shell_.save_if_changed();
    if (verdict.leave_world) wants_title_ = true;
    // A world chosen while you are standing in another one.
    //
    // The library is the same window here as it is on the title, and it lists the same worlds, so
    // opening one from inside a world was something a player would obviously try and the only
    // thing that happened was nothing. Going out to the title to come back in is not a step
    // anybody wants; it is a step the code needed because the only thing that could open a world
    // was the loop the title runs in. So this one ends the same way leaving does — the world is
    // torn down, every pool with it (`02-architecture-overview.md`) — and hands the next one out
    // instead of handing out nothing.
    if (verdict.open_world && !verdict.world.empty()) {
        wants_world_ = verdict.world.string();
        wants_title_ = true;
    }
    apply_knobs();
    shell_drawn_ = !shell_.ui().draw().empty();
}

// What the settings window SHOWS, taken from what the game is actually doing.
//
// Without this the panel opens on the defaults in `Knobs` — which are a guess — so a player reads
// numbers that are not this machine's, and the first slider they touch snaps the game to whatever the
// row happened to say. A settings window has to be a picture of the state before it can be a way to
// change it.
void Application::seed_knobs() {
    ui::Knobs& knobs = shell_.knobs();
    knobs.auto_quality = quality_.enabled();
    knobs.target_fps = (options_.target_fps > 0.0f) ? static_cast<f64>(quality_.target_fps()) : 0.0;
    knobs.quality_level = static_cast<f64>(quality_.level());
    knobs.render_scale = static_cast<f64>(render_scale_);
    knobs.field_of_view = static_cast<f64>(camera_.fov_degrees());
    knobs.vsync = swapchain_.vsync();
    knobs.motion_blur = motion_blur_;
    knobs.overlay = hud_.overlay();
    knobs.changed = false;   // this is a read of the state, not a change to it
}

// What the settings window changed, put where the renderer will see it.
//
// Only on the frames it actually changed something: `changed` is set by the shell and cleared
// here. A settings panel that reapplied every knob every frame would fight the quality controller
// for the level, which is a picture that breathes.
void Application::apply_knobs() {
    ui::Knobs& knobs = shell_.knobs();
    // While the controller owns the level, the row that shows it follows what the controller decided
    // rather than what it last said — otherwise the one number on this panel that moves by itself is
    // the one number on it that is always stale.
    if (knobs.auto_quality) knobs.quality_level = static_cast<f64>(quality_.level());
    if (!knobs.changed) return;
    knobs.changed = false;

    quality_.set_enabled(knobs.auto_quality);
    if (knobs.target_fps > 0.0) {
        quality_.set_target_fps(static_cast<f32>(knobs.target_fps));
    } else {
        quality_.set_target_fps(window_.refresh_hz());
    }
    if (!knobs.auto_quality) {
        quality_.set_level(static_cast<u32>(std::clamp(knobs.quality_level, 0.0,
                                                       static_cast<f64>(kQualityLevels - 1))));
        applied_quality_level_ = 0xFFFFFFFFu;   // force the knobs through on the next update
    }
    // A render scale is the one setting that resizes every target, so it goes through the same
    // path a quality step does rather than being poked in.
    const f32 wanted = static_cast<f32>(std::clamp(knobs.render_scale, 0.05, 4.0));
    if (std::abs(wanted - render_scale_) > 0.001f) {
        render_scale_ = wanted;
        handle_resize();
    }
    motion_blur_ = knobs.motion_blur;
    camera_.set_fov_degrees(static_cast<f32>(knobs.field_of_view));
    if (knobs.vsync != swapchain_.vsync()) swapchain_.set_vsync(knobs.vsync);
    hud_.set_overlay(knobs.overlay);
}

// Deliberately the plainest format that works: one `key value` per line. A player who wants
// to force a quality level or a target should be able to open it in Notepad and see what the
// game decided about their machine, and an unreadable or half-written file should cost a
// benchmark rather than a crash.
void Application::load_settings() {
    const std::string path = settings_path();
    if (path.empty()) return;
    std::ifstream file(path);
    if (!file) return;

    std::string key;
    f64 value = 0.0;
    while (file >> key >> value) {
        if (key == "target_fps") quality_.set_target_fps(static_cast<f32>(value));
        else if (key == "quality_level") { quality_.set_level(static_cast<u32>(value)); benchmark_pending_ = false; }
        else if (key == "auto_quality") quality_.set_enabled(value != 0.0);
    }
}

void Application::save_settings() {
    const std::string path = settings_path();
    if (path.empty()) return;
    std::ofstream file(path, std::ios::trunc);
    if (!file) return;
    file << "target_fps " << quality_.target_fps() << "\n"
         << "quality_level " << quality_.level() << "\n"
         << "auto_quality " << (quality_.enabled() ? 1 : 0) << "\n";
}

// Rebuild the list of emitters when the world has changed, and not otherwise.
//
// Sorted nearest-first around the camera, so a capped list keeps the lamps that light what is
// on screen. Rebuilt on an edit rather than every frame: the scan is cheap because a brick
// carrying no emitter is rejected by its palette, but it is not free, and lamps do not move.
void Application::update_lights() {
    if (!lights_dirty_) return;
    lights_dirty_ = false;

    // Nothing in this world can emit, so there is nothing to scan for.
    //
    // The list is rebuilt on every announced change to the world, and since D397 that includes
    // every region the clip ladder pastes as the building sharpens -- hundreds of them on a cold
    // load. The scan itself is cheap per brick, because a brick whose palette holds no emitter is
    // rejected in a handful of comparisons, but "cheap per brick" over every brick of every chunk
    // several hundred times is not free. A world whose material table contains no emissive record
    // at all cannot contain an emissive voxel, and that is one pass over a few hundred records.
    bool can_emit = false;
    for (const VisualRecord& visual : types_.visuals()) {
        if (visual.emissive != 0) {
            can_emit = true;
            break;
        }
    }

    // Timed, because this walk is O(WORLD) and it is run on every announced change to it -- which
    // since D397 includes every region the clip ladder pastes. That is the shape of the two largest
    // costs this rewrite has already deleted: `rebuild_coarse_grids` was O(world) for a change one
    // metre across at 3.86 ms an edit (D522), and `announce_world_change` named every brick in the
    // edited box at 718 ms in a single frame (D515). Neither was suspected until somebody printed
    // its cost beside the thing it was supposed to be part of. R9g's whole mechanism -- emitters
    // persisted per region rather than rediscovered by scanning -- is what would remove this, so the
    // number belongs here before the change rather than after it.
    const u64 build_began = now_ns();
    std::vector<LightSource> lights;
    last_emitter_scans_ = 0;
    last_emitter_reused_ = 0;
    // R9g's control arm: every rebuild rediscovers every chunk, which is what this did before and
    // is the state every figure taken before it was measured in. It is a cleared cache rather than
    // a second path, so the two arms run the same code and differ by what is in a map.
    if (!options_.emitter_cache) emitter_cache_.clear();
    if (can_emit) {
        // Scan only the chunks whose cells are not already known, and keep the rest. What was
        // dropped from the cache is exactly what `announce_world_change` said had changed.
        //
        // The MERGE still sees every cell in the world, which is what keeps the answer identical:
        // a fitting that straddles a chunk boundary is joined here exactly as it was when every
        // chunk was rescanned, and the ranking and the cap see the same set in the same order.
        std::vector<EmissiveCell> cells;
        world_.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
            auto found = emitter_cache_.find(coord);
            if (found == emitter_cache_.end()) {
                ++last_emitter_scans_;
                found = emitter_cache_
                            .emplace(coord, scan_chunk_emitters(
                                                chunk, coord.x * static_cast<i64>(kChunkEdge),
                                                coord.y * static_cast<i64>(kChunkEdge),
                                                coord.z * static_cast<i64>(kChunkEdge), types_))
                            .first;
            } else {
                ++last_emitter_reused_;
            }
            cells.insert(cells.end(), found->second.begin(), found->second.end());
        });
        lights = merge_light_list(
            cells, camera_.chunk_x() * kChunkEdge + static_cast<i64>(camera_.local_x()),
            camera_.chunk_y() * kChunkEdge + static_cast<i64>(camera_.local_y()),
            camera_.chunk_z() * kChunkEdge + static_cast<i64>(camera_.local_z()));
    }
    const u64 build_ns = now_ns() - build_began;
    light_build_ns_ += build_ns;
    light_build_worst_ns_ = std::max(light_build_worst_ns_, build_ns);
    ++light_builds_;

    // Did anything the renderer can see actually change?
    //
    // This question is worth asking rather than assuming, and the reason is the cost on the other
    // side: a changed list makes every face in the store throw its lamp confidence away and measure
    // again, which is right when a lamp really has moved and is a second of wasted rays when it has
    // not. An edit is announced for the whole box a shadow can reach, so most rebuilds return a
    // list identical to the one already on the card.
    const u64 hash = light_list_hash(lights);
    const bool changed = hash != light_hash_;
    light_hash_ = hash;

    light_count_ = static_cast<u32>(lights.size());
    if (light_count_ > 0 && light_buffer_.mapped != nullptr) {
        std::memcpy(light_buffer_.mapped, lights.data(), lights.size() * sizeof(LightSource));
    }

    if (!changed) return;
    // Sixteen bits is what a face record has room for beside its sample count, so the counter is
    // kept inside them here rather than being masked at three different reading sites. Skipping
    // nought keeps "a zeroed record" distinct from "a record measured under the first list".
    light_version_ = (light_version_ % 0xFFFFu) + 1u;
    light_changed_ = true;
    WS_LOG_INFO("light", "{} emitters, list version {}", light_count_, light_version_);
}

// Measure this machine, then hold the frame rate on it.
//
// The first run benchmarks rather than guesses. A quality level chosen on the machine the
// game was written on means nothing anywhere else, and asking a player to find the settings
// before the game looks right is asking them to do the work the game should have done. So the
// first few seconds are spent at full detail, timed, and the answer is written down; every
// run after that starts from it and the controller takes over.
void Application::update_quality() {
    if (benchmark_pending_) {
        // Skip the opening frames: shaders are still compiling, chunks are still arriving, and
        // the pipeline is cold. Timing those measures the loading screen, not the machine.
        if (frame_counter_ > kBenchmarkWarmupFrames) {
            benchmark_total_ms_ += stats_.last_ms();
            benchmark_worst_ms_ = std::max(benchmark_worst_ms_, stats_.last_ms());
            ++benchmark_frames_;
        }
        if (frame_counter_ >= benchmark_until_ && benchmark_frames_ > 0) {
            const f64 average_ms = benchmark_total_ms_ / static_cast<f64>(benchmark_frames_);
            const u32 level = level_for_frame_time(average_ms, quality_.target_fps());
            quality_.set_level(level);
            benchmark_pending_ = false;
            // The DISTRIBUTION, not only the mean, because the mean is the one number that
            // cannot show the fault people actually report.
            //
            // "It says eighty frames a second and it does not feel like sixty" is a statement
            // about the worst frames, not the average one: at 12 ms average with a 99th of 25 and
            // a worst of 50, one frame in a hundred is four times as long as its neighbours, and
            // the eye reads a single long frame as a stutter no matter how many short ones
            // surround it. Averaging is what hides that, and this measurement existed only as an
            // average until somebody said the game felt wrong while the counter said it was fine.
            WS_LOG_INFO("quality",
                        "benchmark: {:.2f} ms a frame at full detail (50th {:.2f}, 95th {:.2f}, "
                        "99th {:.2f}, worst {:.2f}), target {:.0f} fps -> starting at level {} of {}",
                        average_ms, stats_.percentile_ms(0.50), stats_.percentile_ms(0.95),
                        stats_.percentile_ms(0.99), benchmark_worst_ms_,
                        quality_.target_fps(), level, kQualityLevels - 1);
            save_settings();
        }
        return;   // nothing is adjusted while the measurement is being taken
    }

    quality_.observe(stats_.last_ms());
    if (quality_.level() != applied_quality_level_) {
        const u32 from = applied_quality_level_;
        applied_quality_level_ = quality_.level();
        apply_quality();
        if (from != 0xFFFFFFFFu) {
            WS_LOG_INFO("quality", "level {} -> {} ({:.1f} ms a frame, target {:.1f})", from,
                        applied_quality_level_, quality_.smoothed_ms(),
                        1000.0f / quality_.target_fps());
        }
    }
}

// Push the current level's knobs into the things that read them.
//
// Most of these are read fresh every frame ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â from the parameter block or the push constants ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
// so changing a level costs nothing and cannot fail.
//
// Resolution is the exception, and it is the reason this function can be slow. Rendering
// smaller than the window means new images, new descriptors and a new dispatch size, so the
// device has to be idle before the old ones go away. That is a stall of a millisecond or two,
// taken at most once every twenty frames (kFramesToDrop in game/quality.cpp) and only on the
// three rungs where the scale actually changes ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 3 to 2, 2 to 1, 1 to 0. Everything above
// level 3 renders at the window size and never pays it. Paying it here is what makes the
// dispatch and the accumulation image genuinely smaller; scaling on presentation alone would
// have traced the same pixels and saved nothing.
//
// This must not be called with a frame already recording ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the images it frees are bound to
// the descriptor sets that frame is using. The one caller is update_quality(), which runs
// before the swapchain frame is begun.
void Application::apply_quality() {
    const QualityKnobs& knobs = quality_.knobs();
    detail_bias_ = knobs.detail_bias;

    if (knobs.resolution_scale != render_scale_) {
        render_scale_ = knobs.resolution_scale;
        const VkExtent2D render = scaled_extent();
        if (render_target_.valid() &&
            (render.width != render_target_.extent.width ||
             render.height != render_target_.extent.height)) {
            device_.wait_idle();
            destroy_render_target();
            create_render_target(render.width, render.height);
            WS_LOG_INFO("quality", "rendering at {}x{} of a {}x{} window ({:.0f}%)",
                        render.width, render.height, swapchain_.extent().width,
                        swapchain_.extent().height, render_scale_ * 100.0f);
        }
    }
}

// What the tree-walking passes are told. The sun is the same vector make_trace_push hands the
// tracer, for the reason D204 records: one constant in one place is what stopped
// 19-auto-quality.md holding two figures no build could reproduce.
Application::NodePush Application::make_node_push(u32 face_count) const {
    NodePush push{};
    push.control[0] = node_buffers_.entry_capacity();
    push.control[1] = 32u;
    push.control[2] = face_count;
    push.control[3] = static_cast<u32>(frame_counter_);
    const f32 sun[3] = {0.4f, 0.85f, 0.3f};
    const f32 length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    push.sun[0] = sun[0] / length;
    push.sun[1] = sun[1] / length;
    push.sun[2] = sun[2] / length;
    // The cosine of the sun's angular radius, which is what makes a shadow edge soft. The same
    // figure make_trace_push hands the tracer, for the same reason the direction is.
    push.sun[3] = std::cos(0.5f * 3.14159265f / 180.0f);
    push.face_capacity = face_buffers_.entry_capacity();
    push.face_probes = 32u;

    // How many frames a face waits between shadow rays, so the pass costs the same whatever is on
    // screen. This is face SELECTION -- R3a's third box -- folded into the shading pass rather
    // than run as its own compaction pass, because what costs is the ray and not the record read:
    // an invocation that decides it is not due this frame reads thirty-two bytes and stops.
    //
    // It exists because faces became voxels. A brick-keyed store held nineteen thousand faces at
    // the close camera; keyed by voxel at the level the pixel resolves, the same view is a few
    // hundred thousand, and shading all of them every frame is several milliseconds against a
    // 4.4 ms budget. The plan says exactly what to do about that: the budget is a cap on
    // CONVERGENCE, never on framerate (Ãƒâ€šÃ‚Â§6).
    //
    // A face that has not settled is never held back ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a new surface reaches its answer in a
    // handful of frames however busy the store is, and only the refresh rate of settled faces
    // degrades. That is the right thing to give up: a settled face is looking at a sun that has
    // not moved.
    //
    // Over the ON-SCREEN set, and that is R9b rather than a detail. This stride is the sun's ray
    // budget divided among the faces that want one, and it was divided among the WATERMARK -- so the
    // moment R9a started claiming faces for light rays, every face a pixel is looking at was refreshed
    // less often, by exactly the ratio the off-screen set had grown to. It measured as the faces pass
    // getting CHEAPER (1.16 ms against 0.96) with 262,144 off-screen faces in the store, which is the
    // most misleading shape a regression can take: the number that is supposed to go up went down,
    // and the cost was hidden in convergence -- 84 sun samples a face against 72.
    //
    // That is precisely what the plan says the per-class budget is for: one shared budget lets the
    // off-screen set starve the on-screen one, and the on-screen one is what the player is looking
    // at. A class that overruns must degrade its own refresh rate and nothing else's -- so the
    // off-screen class has a budget of its OWN now (`secondary_light_stride`, R9b), taken out of a
    // separate figure and never out of this one. Subtracting it here is what makes that true rather
    // than intended: the two classes are two divisions of two budgets, and the day they share a
    // denominator is the day D527 happens for the fourth time.
    // The LIVE on-screen faces, and neither word in that is spare.
    //
    // It was the watermark, which counts every slot ever used — so a store that has evicted a
    // quarter of a million faces divides the sun's budget by a quarter of a million faces that do
    // not exist. Two arms of one build differed by exactly that: 125,078 evictions against 32,848,
    // same live count, and a sun stride of 3 against 2. That is a third fewer sun rays on every
    // visible face in one arm, and it reads on the pass table as the change having made the light
    // CHEAPER. D527 is the same fault by a different route, and the general form of it is that a
    // budget divided by the wrong population is a silent quality setting.
    //
    // ...and the COARSE PYRAMID comes out of it too, for the same reason and after making the same
    // mistake a third time (R9f). A stand-in kept past its cold window is a face no pixel is
    // reading, so `may_cast` is false and it casts nothing at all -- and 21,799 of them on the close
    // camera took the stride from 5 to 6. What that cost is invisible in every timing: the faces
    // pass read 1.553 ms against 1.564, and the convergence beside it read **107,582 of 497,656
    // faces finished against 475,632 of 476,230**, because the far ray needs kBounceMin samples at
    // one per stride frames and 512x6 is past where the run was measured. A budget divided by the
    // wrong population is a silent quality setting, and this is the third door into that sentence.
    //
    // A stand-in a pixel IS reading does cast, and it is not subtracted back: that is the transient
    // state where its fine children are missing, and a face standing in for five hundred and twelve
    // others while they are found should be refreshed often rather than rationed. The error is at
    // most this class's share, it is in the direction of more rays rather than fewer, and it is
    // bounded by the pyramid being a few per cent of the store.
    const FaceStoreStats set = face_store_.stats();
    const u32 quiet = set.secondary + set.stand_ins;
    const u32 live = std::max(set.faces > quiet ? set.faces - quiet : set.faces, 1u);
    push.face_stride = std::max(1u, (live + kFacesPerFrame - 1) / kFacesPerFrame);

    // Where the card may claim faces of its own, and R3e is the whole of why it may.
    push.provisional_base = face_buffers_.provisional_base();
    push.face_first = 0;

    // The lamps. `light_reset` is the one-frame announcement, and it is the whole of what makes a
    // placed or deleted lamp arrive instantly: a face whose lamp term has converged stops reading
    // the word that would tell it, so nothing short of the host saying so can reopen it. Consumed
    // by the frame that renders it, exactly as `edit_min.w == 2` is (D373).
    push.light_count = light_count_;
    push.light_version = light_version_;
    push.light_reset = light_changed_ ? 1u : 0u;
    // The two levers this stage added, both switchable at run time so the arms of an A/B are one
    // build. D407 is the standing reason: this pass is a function of a convergence state that is
    // not reproducible between batches, so two builds cannot be compared and two flags can.
    push.from_worklist = 0;   // set by the dispatch that uses the list
    push.seen_window = options_.face_gate;
    push.prolong = options_.face_prolong ? 1u : 0u;
    push.report_crossings = options_.node_crossings ? 1u : 0u;
    push.light_read_period = options_.light_read_period;
    push.edit_seed = options_.face_edit_seed;
    push.lamp_edit_scope = options_.lamp_edit_scope ? 1u : 0u;
    push.face_read_period = options_.face_read_period;
    push.secondary_period = options_.secondary_period;
    push.bounce = options_.bounce ? 1u : 0u;
    push.bounce_min = options_.bounce_min;
    push.bounce_memory = options_.bounce_memory;
    // The same three numbers make_trace_push hands the composite, and named here rather than
    // copied there so the two cannot drift: a gathering ray that escapes reads the sky, and the
    // picture is lit by that same sky, so a difference between them would be indirect light with a
    // colour the frame does not have. See kSunColour.
    push.sun_colour[0] = kSunColour[0];
    push.sun_colour[1] = kSunColour[1];
    push.sun_colour[2] = kSunColour[2];
    push.sun_colour[3] = 0.0f;
    return push;
}

// The off-screen set's own ray budget, as a stride in frames. R9b, and it is the half of that
// sub-step that had never been spent.
//
// The shape is deliberately the same as the sun stride above -- a population divided by a budget --
// because the two are the same kind of number and the failure they share is the one D527, D557 and
// the comment above all describe: a budget divided by the wrong population is a silent quality
// setting. Keeping them apart in two functions over two populations is what makes "a class that
// overruns degrades its own refresh rate and nothing else's" a property of the code.
//
// The budget is a SHARE of the screen's rather than a constant of its own, so the class scales with
// what the machine is already spending rather than with a number somebody picked once. Nought is the
// control arm and means the class casts nothing at all, which is the state everything measured before
// this was measured in; the shader reads a stride of nought as "off" for the same reason
// `secondary_period` does.
//
// What it does NOT do is bound the transient. A face that has just entered this class bursts its
// ambient term exactly as any other face does, because D394 measured every attempt at metering that
// burst as making the transient worse -- what an unconverged face spends is mostly the face and not
// the ray. So this bounds how many such faces exist per frame and lets each of them get on with it.
u32 Application::secondary_light_stride() const {
    if (options_.secondary_light_share == 0) return 0;
    const FaceStoreStats set = face_store_.stats();
    if (set.secondary == 0 && set.stand_ins == 0) return 0;
    // The stand-ins are in this budget too. They are faces no pixel reads by construction -- R9f
    // keeps them precisely so they outlive the fine faces under them -- and a gathering ray falling
    // back to one is reading it just as surely as it reads a face it landed on. They were also, until
    // now, the emptiest records in the store: `the coarse pyramid on the card` reported nought of
    // 21,790 with a finished ambient term.
    const u32 population = set.secondary + set.stand_ins;
    const u32 budget = std::max(1u, kFacesPerFrame / options_.secondary_light_share);
    return std::max(1u, (population + budget - 1) / budget);
}

TracePush Application::make_trace_push() {
    TracePush trace{};
    // Up and to one side, matching the light the real-time shading already assumes, so
    // the two are comparable rather than merely both plausible.
    const f32 sun[3] = {0.4f, 0.85f, 0.3f};
    const f32 length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    trace.sun[0] = sun[0] / length;
    trace.sun[1] = sun[1] / length;
    trace.sun[2] = sun[2] / length;
    // The real sun is about half a degree across. A point light casts a shadow with no
    // penumbra at all, which is the single most obvious way a render looks synthetic.
    trace.sun[3] = std::cos(0.5f * 3.14159265f / 180.0f);
    // Chosen so a 0.5-albedo surface facing the sun lands near mid-grey. At 12 every
    // material blew to white, which hides exactly the differences this mode exists to
    // show. Real exposure control is Stage 9's job.
    trace.sun_colour[0] = kSunColour[0];
    trace.sun_colour[1] = kSunColour[1];
    trace.sun_colour[2] = kSunColour[2];
    trace.control[0] = 0;   // was the accumulator's sample count, which went with R3d
    // Which of the two histories the cloud pass writes this frame; it reads the other.
    cloud_parity_ ^= 1u;
    trace.control[1] = cloud_parity_;
    trace.control[2] = static_cast<u32>(frame_counter_);   // for cache eviction
    trace.quality[0] = quality_.knobs().refine_stride;
    trace.quality[1] = quality_.knobs().shadow_target;
    trace.quality[2] = (shadow_refresh_frames_ > 0) ? 1u : 0u;
    trace.quality[3] = light_count_;
    std::memcpy(trace.fog, fog_, sizeof(trace.fog));
    std::memcpy(trace.fog_shape, fog_shape_, sizeof(trace.fog_shape));
    if (shadow_refresh_frames_ > 0) --shadow_refresh_frames_;
    return trace;
}

void Application::dispatch_clouds(VkCommandBuffer cmd, TracePush& trace,
                                  VkExtent2D render_extent, u32 trace_offset) {
    // The cloud volume first, at a quarter of the resolution in each axis, into the buffer the
    // tracer reads. One march per sixteen pixels: see the head of shaders/clouds.comp for why
    // cloud can afford that and nothing else in the picture can.
    if (clouds_.pipeline() != VK_NULL_HANDLE) {
        profiler_.begin_pass(cmd, "cloud", 0.55);
        const VkImageLayout was =
            cloud_ready_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        image_barrier(cmd, cloud_image_.image, was, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        image_barrier(cmd, cloud_image_prev_.image, was, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        image_barrier(cmd, cloud_marched_.image, was, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        cloud_ready_ = true;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clouds_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clouds_.layout(), 0, 1,
                                &cloud_set_, 1, &trace_offset);
        vkCmdPushConstants(cmd, clouds_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(TracePush), &trace);
        // The packed march: one invocation per four-by-four block, all of them marching.
        const u32 blocks_w = (render_extent.width + kCloudScale - 1) / kCloudScale;
        const u32 blocks_h = (render_extent.height + kCloudScale - 1) / kCloudScale;
        vkCmdDispatch(cmd, (blocks_w + 7) / 8, (blocks_h + 7) / 8, 1);

        VkMemoryBarrier2 marched{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        marched.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        marched.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        marched.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        marched.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo marched_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        marched_dependency.memoryBarrierCount = 1;
        marched_dependency.pMemoryBarriers = &marched;
        vkCmdPipelineBarrier2(cmd, &marched_dependency);

        // And the resolve, over every pixel: this frame's marches where they landed, and last
        // frame's answer reprojected everywhere else. Bit 1 of the parity word picks the half.
        trace.control[1] |= 2u;
        vkCmdPushConstants(cmd, clouds_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(TracePush), &trace);
        vkCmdDispatch(cmd, (render_extent.width + 7) / 8, (render_extent.height + 7) / 8, 1);
        trace.control[1] &= ~2u;
        profiler_.end_pass(cmd);

        // Written by one dispatch and read by the next, so they have to be told apart.
        VkMemoryBarrier2 wrote_cloud{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        wrote_cloud.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        wrote_cloud.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        wrote_cloud.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        wrote_cloud.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo cloud_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        cloud_dependency.memoryBarrierCount = 1;
        cloud_dependency.pMemoryBarriers = &wrote_cloud;
        vkCmdPipelineBarrier2(cmd, &cloud_dependency);

    }
}

void Application::record_frame(f32 time_seconds) {
    const VkCommandBuffer cmd = swapchain_.cmd();
    const VkExtent2D extent = swapchain_.extent();
    // The world is rendered at this size and the window is filled from it by the present
    // blit. The two are the same at every quality level above 2; below that they are not, and
    // anything measured in pixels of the *picture* ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the dispatch, the parameter block's
    // resolution, the bandwidth counter ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â has to use this one rather than the window's.
    const VkExtent2D render_extent = render_target_.extent;

    profiler_.begin_frame(cmd, swapchain_.frame_index());
    feedback_.begin_frame(cmd);

    // ---- streaming ------------------------------------------------------------------
    // The node pool, updated and copied before anything reads it. Its own pass, so the cost is
    // visible beside streaming rather than folded into it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the two are alternatives and the
    // whole point of R1 is which of them is cheaper.
    {
        profiler_.begin_pass(cmd, "nodes", 0.8);
        const f64 camera_voxel[3] = {
            static_cast<f64>(camera_.chunk_x()) * 256.0 + camera_.local_x(),
            static_cast<f64>(camera_.chunk_y()) * 256.0 + camera_.local_y(),
            static_cast<f64>(camera_.chunk_z()) * 256.0 + camera_.local_z(),
        };
        // What the camera can see, for the eviction instrument and for nothing else. Built from
        // the camera itself rather than from `params`, which is filled further down the frame --
        // but from the same four accessors that fill it, so the frustum described here is the one
        // the rays sweep. D426.
        NodeView node_view;
        node_view.origin[0] = camera_voxel[0];
        node_view.origin[1] = camera_voxel[1];
        node_view.origin[2] = camera_voxel[2];
        camera_.forward_vector(node_view.forward);
        camera_.right_vector(node_view.right);
        camera_.up_vector(node_view.up);
        node_view.tan_half_fov = camera_.tan_half_fov();
        node_view.aspect = (render_extent.height > 0)
                               ? static_cast<f32>(render_extent.width) /
                                     static_cast<f32>(render_extent.height)
                               : 1.0f;
        node_view.valid = true;

        const u64 node_start = now_ns();
        const NodeUploadBatch& node_batch =
            node_pool_.update(world_, camera_voxel, frame_counter_, &node_view);
        node_ms_ = ns_to_ms(now_ns() - node_start);
        if (node_ms_ > worst_node_ms_) {
            worst_node_ms_ = node_ms_;
            worst_node_frame_ = frame_counter_;
        }
        last_node_built_ = node_batch.built;
        last_node_evicted_ = node_batch.evicted;
        last_node_evicted_nodes_ = node_batch.evicted_nodes;
        last_node_evicted_on_screen_ = node_batch.evicted_on_screen;
        last_node_churned_ = node_batch.churned;
        last_node_deferred_ = node_batch.deferred;
        last_node_out_of_memory_ = node_batch.out_of_memory;
        node_buffers_.upload(cmd, node_pool_, swapchain_.frame_index());
        profiler_.add_bytes(node_buffers_.stats().uploaded_this_frame);
        profiler_.end_pass(cmd);
    }

    profiler_.begin_pass(cmd, "streaming", 0.8);
    const u64 stream_started = now_ns();
    stream(static_cast<f64>(time_seconds));
    const u64 stream_ended = now_ns();

    // The face store's mirror, AFTER the claims rather than before them.
    //
    // It used to be uploaded beside the node pool, which is recorded a few lines above `stream()` ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
    // so every face claimed this frame missed this frame's copy and reached the card on the next
    // one. A whole frame of latency on the newest faces, spent on nothing, and invisible because
    // the picture it produces is the one that arrives anyway a frame later. `shade_faces` is
    // dispatched far below this point, so the only thing that ever needed the earlier position was
    // the audit, which reads the CPU's copy and not the card's.
    face_buffers_.upload(cmd, face_store_, swapchain_.frame_index());
    profiler_.add_bytes(face_buffers_.stats().uploaded_this_frame);
    // Chunk residency and its ten device buffers stood here (R1e). They were maintained every
    // frame -- about twelve milliseconds of CPU and 650 MB -- for a marcher that has not existed
    // since `world.glsl` was deleted. What is left of that whole layer is the two interned tables
    // a voxel is turned into a colour with, which the node pool does not hold.
    type_tables_.upload_tables(cmd, types_, swapchain_.frame_index());
    profiler_.add_bytes(type_tables_.staged_bytes());

    profiler_.end_pass(cmd);
    const u64 uploads_ended = now_ns();

    {
        // The CHEAP half. `stats()` walks every node and popcounts every leaf, and this runs
        // every frame -- see the note on live_stats().
        const NodePoolStats pool = node_pool_.live_stats();
        StreamingReport report;
        report.world_chunks = world_.chunk_count();
        report.nodes = pool.nodes;
        report.leaves = pool.leaves;
        report.payload_in_use = pool.payload_in_use;
        report.payload_capacity = pool.payload_capacity;
        report.resident_bytes = pool.total_bytes;
        report.screen_bytes = pool.screen_bytes;
        report.staged_bytes = node_buffers_.stats().uploaded_this_frame;
        report.builds = pool.builds;
        report.evictions = pool.evictions;
        report.churn = pool.churn;
        report.deferred = last_node_deferred_;
        report.hit_rate = pool.hit_rate();
        report.out_of_memory = last_node_out_of_memory_;
        report.update_ms = node_ms_;
        report.worst_update_ms = worst_node_ms_;
        report.feedback_reports = last_feedback_;
        report.feedback_dropped = last_feedback_truncated_;
        report.feedback_phantom = last_feedback_phantom_;
        hud_.set_streaming(report);
        // Split, because one figure covering three things is what hid the paste's real cause for
        // two sessions (trap 17). These are the host's per-frame renderer costs, and the frame's
        // own CPU time is printed beside them.
        stream_ms_ = ns_to_ms(stream_ended - stream_started);
        uploads_ms_ = ns_to_ms(uploads_ended - stream_ended);
        report_ms_ = ns_to_ms(now_ns() - uploads_ended);

        const ChiselPreview& preview = chisel_.preview();
        const ClipboardPreview& ghost = clipboard_.preview();
        const bool clipboard_active = toolbelt_.active() == ToolKind::Clipboard;

        ToolReport tool;
        tool.active_tool = tool_name(toolbelt_.active());
        tool.active_slot = toolbelt_.active_slot();
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            tool.slot_count[slot] = toolbelt_.slot_size(slot);
            tool.slot_position[slot] = toolbelt_.slot_position(slot);
        }

        if (clipboard_active) {
            tool.active = ghost.selecting || ghost.holding;
            tool.dragging = ghost.selecting;
            tool.holding = ghost.holding;
            tool.too_large = ghost.too_large;
            tool.copies = clipboard_.copies();
            tool.paste_mode = paste_mode_name(clipboard_.paste_mode());
            tool.adjust_mode = adjust_mode_name(clipboard_.adjust_mode());
            {
                const f64* s = clipboard_.scale();
                const bool uniform = std::abs(s[0] - s[1]) < 1e-6 && std::abs(s[1] - s[2]) < 1e-6;
                if (uniform) {
                    std::snprintf(tool.scale_text, sizeof(tool.scale_text), "%.3gx", s[0]);
                } else {
                    std::snprintf(tool.scale_text, sizeof(tool.scale_text), "%.2g/%.2g/%.2g",
                                  s[0], s[1], s[2]);
                }
            }
            tool.grid_snap = clipboard_.grid_snap();
            tool.turn_step = clipboard_.turn_step_degrees();
            for (int axis = 0; axis < 3; ++axis) tool.angles[axis] = clipboard_.angles_degrees()[axis];
            if (ghost.holding && ghost.instances > 0) {
                for (int axis = 0; axis < 3; ++axis) {
                    tool.min[axis] = ghost.min[0][axis];
                    tool.max[axis] = ghost.max[0][axis];
                }
                tool.volume = clipboard_.clip().cell_count();
            } else {
                for (int axis = 0; axis < 3; ++axis) {
                    tool.min[axis] = ghost.select_min[axis];
                    tool.max[axis] = ghost.select_max[axis];
                }
            }
        } else {
            tool.active = preview.active;
            tool.dragging = preview.dragging;
            tool.carving = preview.mode == ChiselMode::Carve;
            tool.overwrite = chisel_.overwrites();
            tool.against_face = chisel_.places_against_face();
            for (int axis = 0; axis < 3; ++axis) {
                tool.min[axis] = preview.min[axis];
                tool.max[axis] = preview.max[axis];
            }
            tool.volume = preview.volume;
        }
        tool.snapping = chisel_.snapping();
        tool.distance_metres = chisel_.distance() / kVoxelsPerMetre;
        tool.constraints = static_cast<u32>(chisel_.constraints().size());
        tool.undo_depth = history_.undo_depth(1);
        tool.redo_depth = history_.redo_depth(1);
        tool.history_bytes = history_.bytes();
        tool.last_edit_voxels = last_edit_voxels_;
        tool.last_edit_ms = last_edit_ms_;
        tool.ops_logged = op_log_.size();
        hud_.set_tool(tool);

        UpdateReport update;
        switch (updater_.state()) {
            case UpdateState::Available:
                update.show = true;
                update.offering = true;
                update.headline = "WorldShaper " + updater_.info().tag + " is available";
                break;
            case UpdateState::Downloading:
                update.show = true;
                update.downloading = true;
                update.progress = updater_.progress();
                update.headline = "downloading " + updater_.info().tag;
                break;
            case UpdateState::Installed:
                update.show = true;
                update.headline = updater_.message();
                break;
            default:
                break;
        }
        hud_.set_update(update);
    }
    ++frame_counter_;

    // ---- the clipboard's held clip, if it has changed --------------------------------
    //
    // Uploaded only when its *contents* move ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a new selection or a rotation. Sliding the
    // ghost or fanning out copies changes nothing here, because those are instance origins
    // in the parameter block, not voxels.
    if (clipboard_.revision() != clip_uploaded_revision_) {
        clip_uploaded_revision_ = clipboard_.revision();
        clip_slots_.clear();
        const std::vector<Clip>& shapes = clipboard_.shapes();

        u32* out = static_cast<u32*>(clip_staging_.mapped);
        u64 cursor = 0;
        for (const Clip& shape : shapes) {
            const u64 cells = shape.cell_count();
            const u64 blocks = shape.coarse_count();
            if (shape.empty() || cursor + cells + blocks > kMaxClipPoolCells) break;
            for (u64 i = 0; i < cells; ++i) {
                // Type plus one, so that zero can mean "not part of the clip" without
                // stealing a type id or costing a second array.
                out[cursor + i] = (shape.inside[i] != 0) ? (shape.voxels[i] + 1u) : 0u;
            }
            for (u64 i = 0; i < blocks; ++i) {
                out[cursor + cells + i] = shape.coarse[i];
            }
            ClipSlot slot;
            slot.first_cell = static_cast<u32>(cursor);
            slot.first_block = static_cast<u32>(cursor + cells);
            for (u32 axis = 0; axis < 3; ++axis) {
                slot.size[axis] = static_cast<u32>(shape.size[axis]);
                slot.blocks[axis] = static_cast<u32>(shape.coarse_size[axis]);
            }
            clip_slots_.push_back(slot);
            cursor += cells + blocks;
        }

        if (cursor > 0) {
            clip_upload_pending_ = true;
            WS_LOG_INFO("clip", "uploaded {} shape(s), {} cells ({} KB) for the ghost",
                        clip_slots_.size(), cursor, (cursor * sizeof(u32)) / 1024);
            VkBufferCopy copy{};
            copy.size = cursor * sizeof(u32);
            vkCmdCopyBuffer(cmd, clip_staging_.buffer, clip_buffer_.buffer, 1, &copy);
            profiler_.add_bytes(copy.size);
        }
    }
    if (clip_upload_pending_) {
        clip_upload_pending_ = false;
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    for (const GpuImage* image : {&visibility_image_, &render_target_, &depth_target_}) {
        image_barrier(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    // The face slots and the behind-glass layer, which are not in that loop, because only one of
    // the two marchers writes them. Discard-and-rewrite is right for an image every frame
    // overwrites in full; these two are overwritten in full only while the node pool is marching.
    // Toggle to the chunk grid, or open the path tracer, and the composite would be reading a face
    // index the driver was free to invent. So: transitioned once, and then CLEARED on the frames
    // nothing fills them, which says "no face here" and "nothing behind here" in the two values the
    // composite already knows how to ignore.
    //
    // The two travel together because the same shader writes both in the same invocation: what is
    // true of one's lifetime is true of the other's.
    constexpr bool node_writes_faces = true;
    if (!face_ready_) {
        for (const GpuImage* image : {&face_image_, &behind_image_}) {
            image_barrier(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        }
        face_ready_ = true;
        face_cleared_ = false;
    }
    if (!node_writes_faces && !face_cleared_) {
        // Once per spell of not writing it, not once a frame: the contents are stable while
        // nothing touches them, and a full-screen clear every frame to store the same value
        // again is bandwidth spent to change nothing.
        VkClearColorValue clear{};
        clear.uint32[0] = 0xFFFFFFFFu;   // kNoFace, as shaders/node.glsl defines it
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, face_image_.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        // And all-zero for the behind layer, which is how "no second surface" is spelled.
        VkClearColorValue behind_clear{};
        vkCmdClearColorImage(cmd, behind_image_.image, VK_IMAGE_LAYOUT_GENERAL, &behind_clear, 1,
                             &range);
        VkMemoryBarrier2 face_clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        face_clear.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        face_clear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        face_clear.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        face_clear.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo face_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        face_dependency.memoryBarrierCount = 1;
        face_dependency.pMemoryBarriers = &face_clear;
        vkCmdPipelineBarrier2(cmd, &face_dependency);
        face_cleared_ = true;
    } else if (node_writes_faces) {
        face_cleared_ = false;
    }

    // The accumulation image is the one that must *not* be transitioned from UNDEFINED every
    // frame. That transition is free precisely because it permits the driver to throw the
    // contents away, which is exactly right for an image rewritten from scratch and exactly
    // wrong for one whose entire purpose is to remember. It happens to survive on this driver,
    // which is luck rather than a guarantee.
    //
    // So: once from UNDEFINED after it is created, and never again. It stays in GENERAL and
    // the read-after-write between frames is covered by an ordinary memory barrier.
    // ---- frame parameters -----------------------------------------------------------
    (void)time_seconds;
    RenderParams params{};

    // Anything that changes what the image *should* be invalidates every sample taken so far,
    // so the average has to start again. Averaging the old view into the new one does not
    // produce a slightly stale picture, it produces a smear that never clears ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the samples
    // have equal weight however wrong they are.
    //
    // Compared against the camera as it will be used this frame, not against a "did the player
    // press a key" flag: the camera also moves from momentum and from being placed by script.
    {
        const f32 here[6] = {camera_.local_x(),           camera_.local_y(),
                             camera_.local_z(),           static_cast<f32>(camera_.chunk_x()),
                             static_cast<f32>(camera_.chunk_y()),
                             static_cast<f32>(camera_.chunk_z())};
        for (u32 i = 0; i < 6; ++i) trace_camera_[i] = here[i];
    }

    params.origin[0] = camera_.local_x();
    params.origin[1] = camera_.local_y();
    params.origin[2] = camera_.local_z();
    camera_.forward_vector(params.forward);
    camera_.right_vector(params.right);

    // ---- R9c: how far past the screen to claim, from how fast the camera is turning -----------
    //
    // Measured before it was built (D585): the leading edge of a pan carries about 112 ambient
    // samples where the identical pixels arrived at from the other side carry 707. Nothing is
    // MISSING there -- the full-sun fallback is nought at every band, panning or still -- so this
    // is about a face having measured something by the time it arrives rather than about it
    // existing.
    //
    // From the angle between this frame's forward and the last one's, which is exact for a mouse
    // and for `--fly` alike and needs neither to say what it is doing. Standing still it is nought,
    // the margin is nought, the dispatch is exactly the screen, and this stage costs nothing at all
    // in the state the settled grid measures -- which is also why the grid cannot see it.
    {
        f64 turn = 0.0;
        if (halo_forward_valid_) {
            f64 d = 0.0;
            for (u32 axis = 0; axis < 3; ++axis) {
                d += static_cast<f64>(params.forward[axis]) * halo_prev_forward_[axis];
            }
            turn = std::acos(std::clamp(d, -1.0, 1.0));   // radians this frame
        }
        for (u32 axis = 0; axis < 3; ++axis) halo_prev_forward_[axis] = params.forward[axis];
        halo_forward_valid_ = true;

        // One pixel's angle, the same figure the marcher widens its cone by, so "how many pixels
        // has the view moved" is a division rather than a second idea of what a pixel subtends.
        const f64 pixel_angle =
            2.0 * camera_.tan_half_fov() / std::max(1.0, static_cast<f64>(render_extent.height));
        const f64 moved = (pixel_angle > 0.0) ? turn / pixel_angle : 0.0;
        // Capped at a quarter of the smaller axis. The lead is what decides how much of the deficit
        // this closes -- the ambient burst is kSkyBurst a frame, so N frames of lead is 16N samples
        // -- and the cap is what stops a fast spin asking for a margin larger than the screen.
        const f64 wanted = moved * static_cast<f64>(options_.halo_lead);
        const u32 cap = std::min(render_extent.width, render_extent.height) / 4;
        halo_margin_ = options_.halo ? static_cast<u32>(std::clamp(wanted, 0.0, f64(cap))) : 0;
        // Sparse: a halo is a claim and not a picture, and these reports share the feedback buffer
        // with the node reports and the on-screen face requests. Aimed at about twenty thousand a
        // frame, which the buffer holds beside the sixty thousand the screen already asks for.
        halo_stride_ = 8;
        if (halo_margin_ > 0) {
            const f64 ring =
                static_cast<f64>(render_extent.width + 2 * halo_margin_) *
                    static_cast<f64>(render_extent.height + 2 * halo_margin_) -
                static_cast<f64>(render_extent.width) * render_extent.height;
            while (ring / static_cast<f64>(halo_stride_ * halo_stride_) > 20000.0) {
                halo_stride_ <<= 1;
            }
        }
    }
    camera_.up_vector(params.up);
    params.camera_chunk[0] = static_cast<i32>(camera_.chunk_x());
    params.camera_chunk[1] = static_cast<i32>(camera_.chunk_y());
    params.camera_chunk[2] = static_cast<i32>(camera_.chunk_z());

    // Where the camera was last frame, for the motion blur. Carried in the same space as `origin`,
    // which is relative to the camera's own chunk ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so when the player crosses a chunk boundary
    // that space shifts under the stored value and the previous position is suddenly wrong by a
    // chunk. Corrected by the same offset the rest of this function uses, so a step across a
    // boundary does not smear the whole screen for one frame.
    {
        const i64 chunk_now[3] = {camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()};
        for (u32 axis = 0; axis < 3; ++axis) {
            const f64 shift =
                static_cast<f64>(prev_camera_chunk_[axis] - chunk_now[axis]) * kChunkEdge;
            params.prev_origin[axis] = static_cast<f32>(prev_origin_[axis] + shift);
            params.prev_forward[axis] = prev_forward_[axis];
            params.prev_right[axis] = prev_right_[axis];
            params.prev_up[axis] = prev_up_[axis];
        }

        // Only ever a fraction of a frame's travel, so a frame that took a hundred milliseconds
        // does not paint a hundred milliseconds of streak. The shutter is what it is; a hitch is
        // not a longer exposure, it is the same exposure arriving late.
        params.motion[0] = motion_blur_ ? kShutterFraction : 0.0f;
        params.motion[1] = kLongestStreak;
        // And which cloud history holds this frame's answer, for the tracer to read.
        params.motion[2] = static_cast<f32>(cloud_parity_);
        params.motion[3] = 0.0f;   // was the accumulator's weight clamp; R3d took the accumulator

        // The weather. Coverage is what kind of day it is; the time is what moves the decks, and
        // moving the decks is what moves their shadows across the ground.
        params.sky_cloud[0] = cloud_coverage_;
        // In GAME seconds, not real ones. A second at the keyboard is a minute in the world, so
        // the weather has to move sixty times as fast or a cloud takes an in-game hour to cross a
        // field it should cross in a minute ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which reads as a painted sky that happens to drift.
        params.sky_cloud[1] = time_seconds * kGameSecondsPerSecond;
        params.sky_wind[0] = cloud_wind_[0];
        params.sky_wind[1] = cloud_wind_[1];
        // How much GAME time passed since the last frame. The reprojection needs it to follow a
        // cloud rather than the pixel it used to be under; how far that is in metres depends on the
        // noise scale, which is the shader's business and not this one's.
        f32 game_now = time_seconds * kGameSecondsPerSecond;
        f32 game_step = std::clamp(game_now - prev_cloud_time_, 0.0f, kGameSecondsPerSecond);
        prev_cloud_time_ = game_now;
        params.sky_wind[2] = game_step;
        params.sky_wind[3] = 0.0f;

        // The ceiling the light meter may not expose past, which is what decides whether a dark
        // room is allowed to be dark. Nought hands the shader its own default; see
        // kExposureMaxDefault in shaders/resolve.comp for the number and what it was measured
        // against.
        params.tone[0] = options_.exposure_max;
        // ...and how hard R5a's filter weighs a neighbour against what a face already holds.
        // Negative hands the shader its own figure. See kDenoiseEdgeSharp in shade_faces.comp.
        params.tone[1] = options_.denoise_edge;
        // ...and how much a face's lobe has to be worth before it asks for a block of outgoing
        // bins. Negative hands the shader its own figure, and NOUGHT is a real setting there that
        // means every face with a material asks -- which is why this one cannot use nought as the
        // sentinel the way tone[0] does. See kLobeWorthFloor in shaders/face_terms.glsl.
        params.tone[2] = options_.lobe_floor;
        params.tone[3] = 0.0f;

        // And remember this frame's camera for the next one. After the fill, so a frame always
        // blurs against the frame before it rather than against itself.
        prev_origin_[0] = camera_.local_x();
        prev_origin_[1] = camera_.local_y();
        prev_origin_[2] = camera_.local_z();
        for (u32 axis = 0; axis < 3; ++axis) {
            prev_forward_[axis] = params.forward[axis];
            prev_right_[axis] = params.right[axis];
            prev_up_[axis] = params.up[axis];
            prev_camera_chunk_[axis] = chunk_now[axis];
        }
    }

    // The edited region, moved into the camera's own space. Done here in 64-bit and handed over
    // as a small offset, so the shader never has to know how far from the origin the world has
    // wandered.
    if (shadow_refresh_frames_ > 0) {
        const i64 chunk[3] = {camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()};
        for (u32 axis = 0; axis < 3; ++axis) {
            const i64 base = chunk[axis] * kChunkEdge;
            params.edit_min[axis] = static_cast<i32>(
                std::clamp(edit_lo_[axis] - base, i64{-1} << 30, i64{1} << 30));
            params.edit_max[axis] = static_cast<i32>(
                std::clamp(edit_hi_[axis] - base, i64{-1} << 30, i64{1} << 30));
        }
        // 1 means "the window is open, keep re-measuring"; 2 adds "and this is the frame it opened
        // on, so every face in the box throws its history away". Consumed here rather than on a
        // timer, so it is delivered exactly once however many frames the upload takes.
        params.edit_min[3] = edit_window_opened_ ? 2 : 1;
        edit_window_opened_ = false;
    }
    // Clip rays to what is resident, plus a margin.
    //
    // The margin is not slack, it is the mechanism: feedback can only report chunks a ray
    // actually reached, so clipping tightly to the resident set means rays can never
    // discover anything outside it and streaming deadlocks ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the first version of this
    // rendered nothing at all for exactly that reason. With a margin the frontier
    // advances by that many chunks per frame until it covers whatever is visible.
    constexpr i64 kExploreMargin = 24;   // chunks, so ~190 m of frontier per frame
    ChunkCoord bounds_lo{};
    ChunkCoord bounds_hi{};
    // The WORLD's bounds, not what chunk residency happens to hold.
    //
    // This was `residency_.resident_bounds`, and it was the last thing the marcher still read
    // out of the chunk system -- which made deleting that system impossible while the ray clip
    // depended on it. The two differ in the safe direction: resident bounds are what has been
    // streamed and world bounds are what EXISTS, so the box can only ever grow, and a larger clip
    // box clips less rather than hiding geometry. `world_min_`/`world_max_` are maintained a few
    // lines above by the same sweep that used to feed the coarse grids. R1e.
    ChunkCoord world_lo = world_min_;
    ChunkCoord world_hi = world_max_;
    (void)bounds_lo;
    (void)bounds_hi;
    if (world_bounds_valid_) {
        bounds_lo = world_lo;
        bounds_hi = world_hi;
        params.bounds_min[0] = static_cast<i32>(bounds_lo.x - camera_.chunk_x() - kExploreMargin);
        params.bounds_min[1] = static_cast<i32>(bounds_lo.y - camera_.chunk_y() - kExploreMargin);
        params.bounds_min[2] = static_cast<i32>(bounds_lo.z - camera_.chunk_z() - kExploreMargin);
        params.bounds_max[0] = static_cast<i32>(bounds_hi.x - camera_.chunk_x() + kExploreMargin);
        params.bounds_max[1] = static_cast<i32>(bounds_hi.y - camera_.chunk_y() + kExploreMargin);
        params.bounds_max[2] = static_cast<i32>(bounds_hi.z - camera_.chunk_z() + kExploreMargin);
    } else {
        // Nothing resident yet: no box to clip to, so let the rays look around.
        for (u32 axis = 0; axis < 3; ++axis) {
            params.bounds_min[axis] = -256;
            params.bounds_max[axis] = 256;
        }
    }

    // Then widen it to the world's own extent, because thumbnails draw well past what is
    // resident. The box was written when a chunk had to be fully resident to appear at all,
    // so clipping to the resident set plus a margin lost nothing; it does now. A thumbnail a
    // kilometre away is real geometry, and a ray has to be allowed to reach it.
    //
    // The world's extent, and not simply a large number: a large number measured 23.7 ms
    // against 0.78 on the test scene. The clip is not slack ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it is what stops a ray that
    // will never hit anything from stepping until its budget runs out. Bounded by the world,
    // a ray leaving it stops at the edge, which is both exact and free.
    if (world_bounds_valid_) {
        const i64 lo[3] = {world_min_.x - camera_.chunk_x(), world_min_.y - camera_.chunk_y(),
                           world_min_.z - camera_.chunk_z()};
        const i64 hi[3] = {world_max_.x - camera_.chunk_x(), world_max_.y - camera_.chunk_y(),
                           world_max_.z - camera_.chunk_z()};
        for (u32 axis = 0; axis < 3; ++axis) {
            params.bounds_min[axis] =
                std::min<i32>(params.bounds_min[axis], static_cast<i32>(lo[axis]));
            params.bounds_max[axis] =
                std::max<i32>(params.bounds_max[axis], static_cast<i32>(hi[axis]));
        }
    }

    params.resolution[0] = render_extent.width;
    params.resolution[1] = render_extent.height;
    params.resolution[2] = debug_mode_;
    params.resolution[3] = kFeedbackCapacity;
    params.lens[0] = camera_.tan_half_fov();
    // Continuous detail plus the resident-bounds clip mean distance costs almost nothing,
    // so this is set past anything a world will contain rather than being a quality knob.
    params.lens[1] = 4000000.0f;   // voxels: 125 km
    params.lens[2] = detail_bias_;

    // The chisel's preview, moved into the camera-relative space the shader works in. The
    // camera chunk corner is the origin of that space, so the same subtraction that the
    // ray origin gets applies here.
    {
        const i64 base[3] = {camera_.chunk_x() * kChunkEdge, camera_.chunk_y() * kChunkEdge,
                             camera_.chunk_z() * kChunkEdge};
        auto set_tint = [](f32 out[4], const VisualRecord& record, bool invert) {
            const f32 r = static_cast<f32>(record.red) / 255.0f;
            const f32 g = static_cast<f32>(record.green) / 255.0f;
            const f32 b = static_cast<f32>(record.blue) / 255.0f;
            out[0] = invert ? 1.0f - r : r;
            out[1] = invert ? 1.0f - g : g;
            out[2] = invert ? 1.0f - b : b;
            out[3] = 1.0f;
        };

        // The material in hand, untouched. The two tints below carry a decision about it; the
        // cursor marker and the constraint marks want the colour itself. See RenderParams.
        if (!materials_.empty()) {
            const VisualRecord& held = types_.visual_of(chisel_.material());
            params.tool_colour[0] = static_cast<f32>(held.red) / 255.0f;
            params.tool_colour[1] = static_cast<f32>(held.green) / 255.0f;
            params.tool_colour[2] = static_cast<f32>(held.blue) / 255.0f;
            params.tool_colour[3] = 1.0f;
        }

        // How strongly a box tints its faces, as a fraction. A box previously showed only its
        // outline plus a faint wash over the whole volume, which says where an edit is and not
        // which way it is turned — a wireframe seen straight on is ambiguous about that.
        //
        // A quarter for the clipboard's selection, which is a region being measured out, and
        // rather less for the chisel, whose box is usually inside rock and is looked *through*.
        constexpr f32 kSelectionFaceFill = 0.25f;
        constexpr f32 kChiselFaceFill = 0.14f;

        u32 box = 0;
        const auto add_box = [&](const i64 lo[3], const i64 hi[3], i32 state, bool outline = true,
                                 f32 face_fill = 0.0f) {
            if (box >= kMaxPreviewBoxes) return;
            for (int axis = 0; axis < 3; ++axis) {
                params.box_min[box][axis] = static_cast<i32>(lo[axis] - base[axis]);
                params.box_max[box][axis] = static_cast<i32>(hi[axis] - base[axis]);
            }
            params.box_min[box][3] = state;
            // Low byte is the outline flag, next byte is the shell thickness, so the
            // preview can draw the void a hollow placement will leave. Third byte is how
            // hard to fill the faces, which is per box because a selection and the ghost it
            // becomes want different answers and can share a frame.
            const i32 fill =
                std::clamp(static_cast<i32>(face_fill * 255.0f + 0.5f), 0, 255);
            params.box_max[box][3] =
                (outline ? 1 : 0) | (static_cast<i32>(hollow_ & 0xFFu) << 8) | (fill << 16);
            ++box;
        };

        // The cursor marker, added by every branch below. State 6, drawn as a ring rather than a
        // cube: it is on screen every frame of the game, so it has to say "here" and nothing else,
        // and it must not be mistakable for the box that says what is about to happen.
        const auto add_cursor = [&](const i64 at[3]) {
            const i64 same[3] = {at[0], at[1], at[2]};
            add_box(same, same, 6, false, 0.0f);
        };

        if (toolbelt_.active() == ToolKind::Clipboard) {
            const ClipboardPreview& ghost = clipboard_.preview();
            if (ghost.selecting) {
                // Faces filled while the region is being measured out — a selection is a volume
                // and the whole question is how much of the building is inside it. Once it is
                // captured the ghost shows its own voxels and the box goes back to an outline,
                // because a coloured pane over the thing you are lining up is the one thing a
                // paste preview must not do.
                add_box(ghost.select_min, ghost.select_max, ghost.too_large ? 3 : 1, true,
                        kSelectionFaceFill);
            }
            // State 5: march the clip inside this box rather than outlining it, so the
            // ghost shows the voxels that are about to land instead of the space they will
            // land in. Falls back to an outline when the clip is too large to upload.
            for (u32 n = 0; n < ghost.instances && n < kMaxPreviewInstances; ++n) {
                const u32 shape = ghost.shape[n];
                const bool voxel_ghost = shape < clip_slots_.size();
                // Only the last copy ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the one the wheel is steering ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â gets an outline.
                // Sixteen wireframes is both noise to look at and six plane tests a pixel
                // each; the voxels are what says where the others are.
                const bool outline = !voxel_ghost || (n + 1 == ghost.instances);
                const u32 index = box;
                // No face fill: a held clip is drawn as its own voxels, and its bounding box is
                // only there to say which copy the wheel is steering.
                add_box(ghost.min[n], ghost.max[n], voxel_ghost ? 5 : 2, outline, 0.0f);
                if (voxel_ghost && index < kMaxPreviewBoxes) {
                    const ClipSlot& used = clip_slots_[shape];
                    params.clip_slot[index][0] = used.first_cell;
                    params.clip_coarse[index][0] = used.first_block;
                    for (u32 axis = 0; axis < 3; ++axis) {
                        params.clip_slot[index][axis + 1] = used.size[axis];
                        params.clip_coarse[index][axis + 1] = used.blocks[axis];
                    }
                }
            }
            // A ghost is a copy of real voxels rather than one material, so there is no
            // single colour to outline it in. Both tints stay at zero (see below), which the shader
            // reads as "invert the backdrop" ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the neutral answer, and the one that reads
            // over anything.
            //
            // A selection in progress is different: it is not a copy of anything yet, so it takes
            // the material in hand the same way a placement does — its own colour where it can be
            // seen and the inverse where it is buried, which is what keeps a box running into a
            // wall legible on both sides of the wall.
            if (ghost.selecting && !ghost.too_large) {
                set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
            }
            if (ghost.has_cursor) add_cursor(ghost.cursor);
        } else {
            const ChiselPreview& preview = chisel_.preview();
            // Only while a drag is in progress. Idle, the box and the cursor are the same voxel,
            // and drawing both puts a cube around the ring for no information at all -- the marker
            // below already says where the crosshair is.
            if (preview.active && preview.dragging) {
                add_box(preview.min, preview.max,
                        (preview.mode == ChiselMode::Carve) ? 1 : 2, true, kChiselFaceFill);

                if (preview.mode == ChiselMode::Place) {
                    // The material's own colour in the open, its inverse where it is buried.
                    set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                    set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
                } else if (preview.mode == ChiselMode::Carve) {
                    // The other way round, and deliberately so: a carve is drawn in the colour of
                    // what it is about to remove, so the shape buried in the rock reads as the rock
                    // that is going to leave -- and inverted where it stands in open air. The two
                    // halves of one box are then never the same shade, and a carve and a placement
                    // are never mistaken for one another whichever side of a wall they are on.
                    //
                    // The colour comes from what was under the anchor. Carving in open air has
                    // nothing to remove, so it falls back to the material in hand rather than to
                    // nothing, which would leave the box invisible against the sky.
                    const VoxelTypeId doomed =
                        (preview.removing != kAir) ? preview.removing : chisel_.material();
                    set_tint(params.tint_visible, types_.visual_of(doomed), true);
                    set_tint(params.tint_occluded, types_.visual_of(doomed), false);
                }
            }
            if (preview.has_cursor) add_cursor(preview.cursor);
        }
        if (!options_.preview.empty()) {
            i64 values[7]{0, 0, 0, 0, 0, 0, 2};
            parse_numbers(options_.preview, values, 7);
            const i64 lo[3] = {values[0], values[1], values[2]};
            const i64 hi[3] = {values[3], values[4], values[5]};
            box = 0;   // the scripted box replaces whatever the tool wanted to show
            add_box(lo, hi, static_cast<i32>(values[6]), true, kChiselFaceFill);
            // Same colour rules as the live tool, so a scripted screenshot shows what a
            // player would see rather than a stand-in.
            if (values[6] == 2 && !materials_.empty()) {
                set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
            } else if (values[6] == 1 && !materials_.empty()) {
                // Carve, the same way round as the live tool: inverted in the open, the doomed
                // material where it is buried.
                set_tint(params.tint_visible, types_.visual_of(materials_[0]), true);
                set_tint(params.tint_occluded, types_.visual_of(materials_[0]), false);
            }
        }
        // The constraint points. Drawn as crosses in the material's colour rather than as filled
        // cells in the inverse of whatever is behind them -- a filled cell is indistinguishable
        // from a one-voxel preview box, and an inverted backdrop is a different colour on every
        // surface it lands on, so a row of them did not read as a row of anything.
        //
        // Asked of the tool that is actually in hand. The clipboard's points live on its own
        // selector, and showing the chisel's while the clipboard is out would mark places the
        // selection box is not going to reach.
        const std::vector<std::array<i64, 3>>& points =
            (toolbelt_.active() == ToolKind::Clipboard) ? clipboard_.constraints()
                                                        : chisel_.constraints();

        // Gathered camera-relative, then sorted and grouped. No cap: see kMarkGroup.
        mark_points_.clear();
        mark_points_.reserve(points.size() + options_.preview_marks.size());
        const auto add_mark = [&](const i64 point[3]) {
            mark_points_.push_back({static_cast<i32>(point[0] - base[0]),
                                    static_cast<i32>(point[1] - base[1]),
                                    static_cast<i32>(point[2] - base[2])});
        };
        for (const std::array<i64, 3>& point : points) add_mark(point.data());
        for (const std::string& scripted : options_.preview_marks) {
            i64 at[3]{0, 0, 0};
            parse_numbers(scripted, at, 3);
            add_mark(at);
        }
        params.marks_min[3] = 0;
        params.marks_max[3] = 0;

        if (!mark_points_.empty()) {
            // Sorted so a group is a NEIGHBOURHOOD rather than an arbitrary thirty-two.
            //
            // The groups exist to be rejected wholesale, and a group whose members are scattered
            // across the building has a bounding box the size of the building and rejects nothing.
            // Sorting by a coarse cell first puts points that are near each other in the same
            // group, which is the entire difference between the hierarchy working and being an
            // extra test per pixel. Two metres a cell: fine enough to separate distinct clusters,
            // coarse enough that a swept line stays in a handful of them.
            constexpr i32 kSortCell = 64;
            std::sort(mark_points_.begin(), mark_points_.end(),
                      [](const std::array<i32, 3>& l, const std::array<i32, 3>& r) {
                          const i32 lz = l[2] / kSortCell, rz = r[2] / kSortCell;
                          if (lz != rz) return lz < rz;
                          const i32 ly = l[1] / kSortCell, ry = r[1] / kSortCell;
                          if (ly != ry) return ly < ry;
                          const i32 lx = l[0] / kSortCell, rx = r[0] / kSortCell;
                          if (lx != rx) return lx < rx;
                          return l < r;
                      });

            const u32 count = static_cast<u32>(mark_points_.size());
            const u32 groups = (count + kMarkGroup - 1) / kMarkGroup;
            // Eight words a header, three a mark. Packed into the tail of the clip buffer, which
            // clips fill from the other end.
            const u64 words = static_cast<u64>(groups) * 8 + static_cast<u64>(count) * 3;
            if (words <= kMarkReserveCells) {
                const u32 header_at = static_cast<u32>(kMaxClipPoolCells - words);
                const u32 marks_at = header_at + groups * 8;
                u32* out = static_cast<u32*>(clip_staging_.mapped);

                i32 all_lo[3]{}, all_hi[3]{};
                for (u32 g = 0; g < groups; ++g) {
                    const u32 first = g * kMarkGroup;
                    const u32 members = std::min<u32>(kMarkGroup, count - first);
                    i32 lo[3], hi[3];
                    for (u32 axis = 0; axis < 3; ++axis) {
                        lo[axis] = mark_points_[first][axis];
                        hi[axis] = lo[axis] + 1;
                    }
                    for (u32 m = 0; m < members; ++m) {
                        const std::array<i32, 3>& at = mark_points_[first + m];
                        for (u32 axis = 0; axis < 3; ++axis) {
                            lo[axis] = std::min(lo[axis], at[axis]);
                            // A mark covers its whole voxel, so the high corner is one past it.
                            hi[axis] = std::max(hi[axis], at[axis] + 1);
                            out[marks_at + (first + m) * 3 + axis] = static_cast<u32>(at[axis]);
                        }
                    }
                    for (u32 axis = 0; axis < 3; ++axis) {
                        out[header_at + g * 8 + axis] = static_cast<u32>(lo[axis]);
                        out[header_at + g * 8 + 3 + axis] = static_cast<u32>(hi[axis]);
                        all_lo[axis] = (g == 0) ? lo[axis] : std::min(all_lo[axis], lo[axis]);
                        all_hi[axis] = (g == 0) ? hi[axis] : std::max(all_hi[axis], hi[axis]);
                    }
                    out[header_at + g * 8 + 6] = marks_at + first * 3;
                    out[header_at + g * 8 + 7] = members;
                }

                for (u32 axis = 0; axis < 3; ++axis) {
                    params.marks_min[axis] = all_lo[axis];
                    params.marks_max[axis] = all_hi[axis];
                }
                params.marks_min[3] = static_cast<i32>(groups);
                params.marks_max[3] = static_cast<i32>(header_at);
                mark_upload_words_ = words;
                mark_upload_at_ = header_at;
            } else if (count != last_marks_reported_) {
                // The one case that can still refuse, and it says so rather than dropping them
                // silently -- trap 7. It needs about forty thousand points to reach.
                WS_LOG_WARN("tool", "{} constraint points is past what the preview buffer holds",
                            count);
            }
        }
        last_marks_reported_ = static_cast<u32>(mark_points_.size());
    }

    // The marks, into the tail of the clip buffer, every frame.
    //
    // Every frame rather than on a revision, unlike the clip itself: these are camera-RELATIVE, so
    // they move whenever the camera crosses a chunk boundary even if the player has dropped
    // nothing. A few kilobytes of a copy is cheaper than the bookkeeping to know when it is not
    // needed. The region cannot overlap the clip's, which packs up from zero while this packs down
    // from the top, and the barrier below already covers both.
    if (mark_upload_words_ > 0) {
        VkBufferCopy copy{};
        copy.srcOffset = static_cast<VkDeviceSize>(mark_upload_at_) * sizeof(u32);
        copy.dstOffset = copy.srcOffset;
        copy.size = mark_upload_words_ * sizeof(u32);
        vkCmdCopyBuffer(cmd, clip_staging_.buffer, clip_buffer_.buffer, 1, &copy);
        profiler_.add_bytes(copy.size);
        mark_upload_words_ = 0;

        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // The slot stride is the device's alignment, not sizeof ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the dynamic offset passed at
    // bind time uses the aligned stride, so writing at sizeof would put frame 1's data
    // where the shader is not looking.
    std::memcpy(static_cast<u8*>(params_buffer_.mapped) +
                    static_cast<usize>(swapchain_.frame_index()) * params_stride_,
                &params, sizeof(RenderParams));


    // ---- primary visibility ---------------------------------------------------------
    profiler_.begin_pass(cmd, "visibility", 9.5);
    const u32 params_offset =
        static_cast<u32>(swapchain_.frame_index()) * static_cast<u32>(params_stride_);
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_.layout(), 0,
                                1, &node_set_, 1, &params_offset);
        // The entry table's size and how far a probe may run. A push constant rather than a
        // field in the parameter block, because it belongs to this pipeline and nothing else
        // reads it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and because the block is already at the size AMD gives (128 bytes) once.
        const NodePush node_constants = make_node_push(0);
        vkCmdPushConstants(cmd, visibility_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(node_constants), &node_constants);
    }

    // The screen plus R9c's margin on every side. `halo_margin_` is nought whenever the camera is
    // not turning, so this is the dispatch it has always been in the settled case, and the two arms
    // of an A/B differ by a number rather than by a code path.
    vkCmdDispatch(cmd, (render_extent.width + 2 * halo_margin_ + 7) / 8,
                  (render_extent.height + 2 * halo_margin_ + 7) / 8, 1);
    profiler_.add_bytes(static_cast<u64>(render_extent.width) * render_extent.height * 20);
    profiler_.end_pass(cmd);

    // ---- shade the faces -------------------------------------------------------------
    //
    // One invocation per face, and the whole point of the stage: the dispatch is sized by how
    // many FACES are live, not by how many pixels there are. The same building at 4K shades
    // exactly what it shades at 800p.
    //
    // Its own pass, so the cost is visible beside visibility rather than folded into it -- the
    // claim R3 makes is that this number does not move with resolution and that has to be
    // measurable rather than asserted (D201's lesson, in the pass it is being made about).
    {
        // The visibility pass writes faces now, not only reads them: R3e has it claim a stand-in on
        // the card for any surface the store has never heard of. So the shading pass has to be told
        // to wait for those writes, where before the only barrier in the frame was the one before
        // the composite. Without it the claim and the ray that fills it are a race, and the shape
        // of that failure is a face that reads as fully lit for one frame -- which is precisely
        // what this stage exists to remove, so it would look like the change not working.
        VkMemoryBarrier2 claim_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        claim_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        claim_barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        claim_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        claim_barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo claim_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        claim_dependency.memoryBarrierCount = 1;
        claim_dependency.pMemoryBarriers = &claim_barrier;
        vkCmdPipelineBarrier2(cmd, &claim_dependency);

        const u32 face_count = face_store_.watermark();
        const u32 provisional_base = face_buffers_.provisional_base();
        const u32 provisional_count = FaceBuffers::provisional_count();

        // ---- the probe buffer, which is now a lever as well as an instrument ------------------
        //
        // The host's dials in word 0, the off-screen set's stride in the last word, and the card's
        // counters cleared between them. THREE DISJOINT RANGES, and that is the whole reason the
        // stride sits at the far end rather than next to the dials: transfer commands in one command
        // buffer are not ordered against each other, so a host word inside the filled span would be
        // whichever of the two the driver happened to run second.
        //
        // Two commands rather than one fill for the dials was already the ownership rule made
        // structural rather than remembered (D528), and the third is the same rule again.
        //
        // # Why this moved above the work list
        //
        // `face_work_of` reads the stride, and BOTH passes call it -- the worklist to decide which
        // slots are dispatched at all and the shading pass to decide what to do with them. It is one
        // function precisely so the two cannot disagree (D420), and writing the stride between them
        // would have handed the worklist last frame's value and the shading pass this frame's. That
        // divergence is silent in the direction that costs: a worklist that is stricter drops faces
        // out of the dispatch and nothing anywhere says a face stopped being lit.
        {
            const u32 dials = (options_.light_probe ? kProbeOn : 0u) |
                              (options_.coarse_bounce ? kProbeCoarseBounce : 0u) |
                              (options_.face_denoise ? kProbeDenoise : 0u) |
                              (options_.face_materials ? kProbeMaterial : 0u) |
                              (options_.face_fold ? kProbeFold : 0u) |
                              (options_.face_lobe ? kProbeLobe : 0u) |
                              (options_.lobe_ray ? kProbeLobeRay : 0u) |
                              (options_.lobe_coverage ? kProbeLobeCoverage : 0u) |
                              (options_.see_through ? kProbeSeeThrough : 0u);
            vkCmdUpdateBuffer(cmd, light_probe_.buffer(), 0, sizeof(dials), &dials);
            const u32 secondary_stride = secondary_light_stride();
            vkCmdUpdateBuffer(cmd, light_probe_.buffer(), kProbeSecondaryStride * sizeof(u32),
                              sizeof(secondary_stride), &secondary_stride);
            // R9c's two, in the same range past the counters and for the same reason: the host owns
            // these words and the fill below must not touch them. They go as one update of two
            // words, which is one command rather than two and cannot be half applied.
            const u32 halo[2]{halo_margin_, halo_stride_};
            vkCmdUpdateBuffer(cmd, light_probe_.buffer(), kProbeHaloMargin * sizeof(u32),
                              sizeof(halo), halo);
            // Everything between the two host words, and the bounds are named rather than counted:
            // words 1 up to but not including the stride.
            vkCmdFillBuffer(cmd, light_probe_.buffer(), sizeof(u32),
                            (kProbeSecondaryStride - 1) * sizeof(u32), 0u);
            VkMemoryBarrier2 probe_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            probe_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            probe_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            probe_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            probe_barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            VkDependencyInfo probe_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            probe_dependency.memoryBarrierCount = 1;
            probe_dependency.pMemoryBarriers = &probe_barrier;
            vkCmdPipelineBarrier2(cmd, &probe_dependency);
        }

        // ---- which faces owe work, packed ---------------------------------------------------
        //
        // The shading dispatch is sized by this rather than by the store, so every workgroup it
        // launches is full. See shaders/face_worklist.comp for the measurement that made it worth
        // a pass, a barrier and an indirect dispatch.
        //
        // The header is zeroed here rather than by the pass itself: a compaction pass cannot clear
        // the counter it is about to atomicAdd into, because there is no ordering between its own
        // workgroups. Sixteen bytes of fill.
        const bool use_worklist = options_.face_worklist && face_count > 0;
        if (use_worklist) {
            profiler_.begin_pass(cmd, "face list", 0.30);
            // {groups_x, groups_y, groups_z, count}. Y and Z are ONE, not nought: a fill of zeroes
            // over all four words leaves a dispatch of (n, 0, 0), which launches nothing at all and
            // reads as the face pass suddenly costing a tenth of what it did.
            const u32 header[4]{0, 1, 1, 0};
            vkCmdUpdateBuffer(cmd, face_work_.buffer, 0, sizeof(header), header);
            VkMemoryBarrier2 clear_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            clear_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            clear_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            clear_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            clear_barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            VkDependencyInfo clear_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            clear_dependency.memoryBarrierCount = 1;
            clear_dependency.pMemoryBarriers = &clear_barrier;
            vkCmdPipelineBarrier2(cmd, &clear_dependency);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, face_worklist_.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, face_worklist_.layout(),
                                    0, 1, &node_set_, 1, &params_offset);
            const NodePush list_push = make_node_push(face_count);
            vkCmdPushConstants(cmd, face_worklist_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(list_push), &list_push);
            vkCmdDispatch(cmd, (face_count + 63) / 64, 1, 1);
            profiler_.end_pass(cmd);

            // The list is both read as data and consumed as a dispatch command, and those are two
            // different destination stages. Naming only the shader stage leaves the command fetch
            // unordered against the write that produced it, which is a dispatch of whatever the
            // buffer held last frame -- and last frame's count is plausible, so it would have read
            // as a mysterious few thousand faces going unlit rather than as a fault.
            VkMemoryBarrier2 list_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            list_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            list_barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            list_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            list_barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            VkDependencyInfo list_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            list_dependency.memoryBarrierCount = 1;
            list_dependency.pMemoryBarriers = &list_barrier;
            vkCmdPipelineBarrier2(cmd, &list_dependency);
        }

        if (face_count > 0 || provisional_count > 0) {
            profiler_.begin_pass(cmd, "faces", 4.4);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shade_faces_.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shade_faces_.layout(), 0,
                                    1, &node_set_, 1, &params_offset);
            if (face_count > 0) {
                NodePush shade_push = make_node_push(face_count);
                shade_push.from_worklist = use_worklist ? 1u : 0u;
                vkCmdPushConstants(cmd, shade_faces_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(shade_push), &shade_push);
                if (use_worklist) {
                    vkCmdDispatchIndirect(cmd, face_work_.buffer, 0);
                } else {
                    vkCmdDispatch(cmd, (face_count + 63) / 64, 1, 1);
                }
            }

            // And the card's own faces, as a second dispatch rather than a longer first one.
            //
            // They sit in the tail of the same array, above `max_faces`, while the store's
            // watermark is far below it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so one dispatch spanning both would be a million
            // invocations reading thirty-two bytes and stopping. This one is 32,768 invocations,
            // most of which find a mark from an older frame and return.
            NodePush provisional_push = make_node_push(provisional_base + provisional_count);
            provisional_push.face_first = provisional_base;
            vkCmdPushConstants(cmd, shade_faces_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(provisional_push), &provisional_push);
            vkCmdDispatch(cmd, (provisional_count + 63) / 64, 1, 1);

            profiler_.add_bytes(static_cast<u64>(face_count + provisional_count) * sizeof(GpuFace));
            profiler_.end_pass(cmd);
        }
    }

    // ---- resolve --------------------------------------------------------------------
    VkMemoryBarrier2 vis_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    vis_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vis_barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    vis_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vis_barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo vis_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    vis_dependency.memoryBarrierCount = 1;
    vis_dependency.pMemoryBarriers = &vis_barrier;
    vkCmdPipelineBarrier2(cmd, &vis_dependency);

    // The same sky the path tracer draws: the cloud volume into the shared buffer, then the
    // parameters the resolve pass needs to read it and to light the air.
    TracePush trace = make_trace_push();
    const u32 trace_offset =
        static_cast<u32>(swapchain_.frame_index()) * static_cast<u32>(params_stride_);
    dispatch_clouds(cmd, trace, render_extent, trace_offset);

    // ---- the light meter's two slots, rotated (R6a) --------------------------------------------
    //
    // Slot 0 is what the frame about to be drawn will add to; slot 1 is what the frame before it
    // finished as, and is what every invocation reads so that they all compute the same exposure.
    // So: copy 0 into 1, then zero 0. Both are transfers, and they must be ordered against each
    // other and against the composite that reads one and writes the other.
    //
    // With the meter off, BOTH slots are zeroed instead. That leaves `groups` at nought in the
    // slot the shader reads, which is the shader's own "nothing has been measured" path, and it
    // applies `kPreviewExposure` -- exactly the constant this pass used before R6a existed. The
    // control arm is therefore the old picture by construction rather than by a second code path.
    {
        const VkDeviceSize slot = sizeof(FrameStatistics);
        if (options_.auto_exposure) {
            const VkBufferCopy rotate{0, slot, slot};
            vkCmdCopyBuffer(cmd, frame_stats_.buffer, frame_stats_.buffer, 1, &rotate);
            VkMemoryBarrier2 copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            copied.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            copied.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            copied.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            copied.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo after_copy{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            after_copy.memoryBarrierCount = 1;
            after_copy.pMemoryBarriers = &copied;
            vkCmdPipelineBarrier2(cmd, &after_copy);
            vkCmdFillBuffer(cmd, frame_stats_.buffer, 0, slot, 0);
        } else {
            vkCmdFillBuffer(cmd, frame_stats_.buffer, 0, VK_WHOLE_SIZE, 0);
        }
        VkMemoryBarrier2 cleared{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        cleared.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        cleared.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        cleared.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cleared.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo after_clear{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        after_clear.memoryBarrierCount = 1;
        after_clear.pMemoryBarriers = &cleared;
        vkCmdPipelineBarrier2(cmd, &after_clear);
    }

    profiler_.begin_pass(cmd, "resolve", 0.8);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.layout(), 0, 1,
                            &resolve_set_, 1, &params_offset);
    // The sun, the weather and the air. This pass drew a hardcoded gradient for want of them.
    vkCmdPushConstants(cmd, resolve_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TracePush),
                       &trace);
    vkCmdDispatch(cmd, (render_extent.width + 7) / 8, (render_extent.height + 7) / 8, 1);
    profiler_.add_bytes(static_cast<u64>(render_extent.width) * render_extent.height * 20);
    profiler_.end_pass(cmd);

    // Hand this frame's "what I could not find" list back to the CPU. Without this the
    // shader's report is written and then thrown away, and streaming never learns
    // anything ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is exactly what happened until it was noticed.
    // ...and the light meter's answer, so a log line can say what it settled on. Thirty-two bytes
    // after the composite has written them; nothing waits on it and it is read whenever the host
    // next looks.
    {
        VkMemoryBarrier2 metered{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        metered.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        metered.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        metered.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        metered.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo after_meter{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        after_meter.memoryBarrierCount = 1;
        after_meter.pMemoryBarriers = &metered;
        vkCmdPipelineBarrier2(cmd, &after_meter);
        const VkBufferCopy back{0, 0, kFrameStatsSlots * sizeof(FrameStatistics)};
        vkCmdCopyBuffer(cmd, frame_stats_.buffer, frame_stats_readback_.buffer, 1, &back);
    }

    feedback_.end_frame(cmd, swapchain_.frame_index());

    // ---- present ------------------------------------------------------------------
    image_barrier(cmd, render_target_.image, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // The swapchain image is put into TRANSFER_DST by whichever of the two paths below writes it.
    // Doing it here as well would be a second transition out of UNDEFINED on top of the first —
    // legal, and exactly the kind of redundancy that makes a layout trace unreadable.

    // This is also where a scaled render is put back to the size of the window: source is the
    // render target at whatever the quality level chose, destination is the whole swapchain
    // image, and the filter below does the stretching. Nothing else in the frame needs to know.
    //
    // With a window open, the same enlargement happens into the shell's own surface instead, the
    // interface is drawn there at the WINDOW's resolution, and that goes to the swapchain size for
    // size. Putting the interface through the upscale with the world would soften a three-pixel
    // letter for nothing — and when no window is open none of it runs, so the world in the middle
    // pays exactly what it always paid.
    profiler_.begin_pass(cmd, "blit", 0.4);
    if (shell_drawn_ && shell_pass_.valid()) {
        shell_pass_.blit_in(cmd, render_target_.image, render_extent);
        profiler_.end_pass(cmd);
        // Its own pass, with its own budget, because `09-performance-budgets.md` gives the whole
        // interface 0.6 ms on a T0 machine and a number folded into the blit is a number nobody
        // can hold to that.
        profiler_.begin_pass(cmd, "shell", 0.6);
        shell_pass_.upload(shell_.ui().draw(), swapchain_.frame_index());

        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        memory.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(cmd, &dependency);

        const ui::Colour& accent = shell_.ui().accent();
        const f32 accent_rgb[3]{accent.r, accent.g, accent.b};
        shell_pass_.draw(cmd, shell_.ui().draw(), swapchain_.frame_index(), accent_rgb,
                         static_cast<f32>(shell_.ui().seconds()));
        profiler_.end_pass(cmd);
        profiler_.begin_pass(cmd, "blit", 0.4);
        shell_pass_.blit_out(cmd, swapchain_.current_image(), extent);
    } else {
        image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource = region.srcSubresource;
        region.srcOffsets[1] = {static_cast<i32>(render_extent.width),
                                static_cast<i32>(render_extent.height), 1};
        region.dstOffsets[1] = {static_cast<i32>(extent.width), static_cast<i32>(extent.height), 1};

        VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
        blit.srcImage = render_target_.image;
        blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blit.dstImage = swapchain_.current_image();
        blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blit.regionCount = 1;
        blit.pRegions = &region;
        blit.filter = VK_FILTER_LINEAR;
        vkCmdBlitImage2(cmd, &blit);
    }
    profiler_.end_pass(cmd);

    // Leave the render target in GENERAL. A frame that ends with an image in a transfer
    // layout is a trap for anything that reads it afterwards ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is exactly what the
    // screenshot path walked into.
    image_barrier(cmd, render_target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);

    image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    profiler_.begin_pass(cmd, "hud", 0.6);
    hud_.render(cmd, swapchain_.current_view(), extent);
    profiler_.end_pass(cmd);

    image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    profiler_.end_frame(cmd);

    // The lamp announcement is delivered exactly once, and it is delivered HERE rather than where
    // the parameter block is filled, because that fill runs before the passes that read it. Same
    // discipline as `edit_window_opened_`: consumed by the frame that used it, not on a timer, so
    // however long an upload takes the flag is seen by exactly the frames it belongs to.
    light_changed_ = false;
}

int Application::play(const Options& options) {
    options_ = options;
    despeckle_ = options.despeckle;

    // ---- the loading screen ------------------------------------------------------------
    //
    // Created here and nowhere later, because here is the earliest it CAN be: the device and the
    // swapchain are up and nothing else is. Everything expensive in startup happens below this
    // line, so this is the line that decides whether the player watches it or watches a black
    // window and wonders whether the game has died.
    {
        const std::filesystem::path shaders = compiled_shader_dir();
        loading_screen_.create(device_, std::filesystem::path(WS_SHADER_SOURCE_DIR) / "loading.comp",
                               shaders / "loading.comp.spv");
    }

    load_history_ = LoadHistory::read(loading_cache_path());
    progress_.begin(load_history_, LoadProgress::likely_cached(load_history_));
    load_began_ns_ = now_ns();

    // The build runs on its own thread and this one draws. Everything the build reports is atomic
    // and relaxed, so there is no lock between them and a bar one frame stale is a bar nobody can
    // tell is stale.
    {
        std::atomic<bool> built{false};
        std::thread builder([&] {
            build_world();
            built.store(true, std::memory_order_release);
        });
        while (!built.load(std::memory_order_acquire)) {
            draw_loading();
            if (loading_quit_) break;
        }
        builder.join();
    }
    if (loading_quit_) {
        // The window was closed while the world was still building. Nothing above the loading
        // screen has been created yet, so this is the whole of the tear-down — and it has to
        // happen, because the process carries on to the title rather than ending here.
        loading_screen_.destroy();
        return 0;
    }

    // Everything from here to the first frame is the part a progress bar normally leaves out, and
    // leaving it out is exactly why bars sit at ninety-nine per cent: the residency tables, the
    // summary tree, the three pipelines and their shader compiles are all real time, and none of
    // it is "the world building". So the bar keeps running through it, with a frame drawn between
    // each step. The last stage in the list is the first frame that can actually be shown, which
    // is what makes a hundred per cent mean the game is up rather than nearly up.
    progress_.enter(LoadStage::Uploading);
    draw_loading();

    // The VRAM share, the brick-slot arithmetic and the residency budget that stood here were
    // the chunk system's, and they are gone with it (R1e). The node pool sizes itself from
    // `NodePoolBudget` and the face store from `FaceStoreBudget`, both a few lines below.
    //
    // What that budget cost, for the record: it was sized at a tenth of half the card and still
    // zeroed pools the frame never touched -- 1,432 ms of chunk residency and 266 ms of thumbnail
    // tiers on a 2,033 ms start, plus about twelve milliseconds of CPU a frame keeping current a
    // structure no shader had read since `world.glsl` was deleted.

    // How far the world reaches, which is what a ray is clipped to. It used to be a side effect
    // of rebuilding the coarse grids.
    refresh_world_bounds();
    progress_.within(0.30);
    draw_loading();

    const u64 t_tables = now_ns();
    // The two interned tables, and one frame of staging for them. 32 MB because the tables can be
    // two million records each and both halves have to travel together.
    if (!type_tables_.create(device_, 32ull << 20)) {
        return 1;
    }

    if (!feedback_.create(device_)) return 1;
    progress_.within(0.70);
    draw_loading();

    // One slot per frame in flight, aligned to whatever the device demands, so writing
    // next frame's parameters cannot disturb the frame still executing.
    const u64 alignment = device_.caps().min_uniform_offset;
    params_stride_ = ((sizeof(RenderParams) + alignment - 1) / alignment) * alignment;
    params_buffer_ = create_staging_buffer(device_, params_stride_ * kFramesInFlight,
                                           "render params",
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    // The tracer's world-space face cache -- 128 to 256 MB of it, sized from VRAM -- is gone
    // (R1e), and so was the frame-statistics buffer beside it. This is that buffer back, with a
    // writer this time: R6a's light meter, in `shaders/resolve.comp`.
    //
    // Two slots of sixteen bytes. Slot 0 is the frame being drawn, zeroed just before the composite
    // and added to with atomics; slot 1 is the frame before it, complete and written by nobody
    // while it is read. One slot cannot do both jobs -- a shader reading the words it is also
    // adding to sees however much of the frame happened to have run, so the same scene would expose
    // differently across the picture and differently again on another card.
    //
    // TRANSFER_SRC as well as the STORAGE this needs, because the rotation is a copy of this buffer
    // into itself and `create_device_buffer` grants only TRANSFER_DST -- every other device buffer
    // here is written from staging and never read by a copy. `--validation` is the only thing that
    // says so: the copy ran and the picture looked right.
    frame_stats_ = create_device_buffer(
        device_, kFrameStatsSlots * sizeof(FrameStatistics),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "frame statistics");
    // ...and thirty-two bytes the host can read it back through, copied at the end of every frame.
    //
    // An exposure that nobody can read is a constant nobody can argue with: the whole reason this
    // stage exists is that `kPreviewExposure` was 3.2 with no writer and no way to tell from a
    // picture whether that was right. A meter with no printed number would be the same fault with
    // an extra buffer, so `frame:` carries what it settled on and what the frame's log-average
    // luminance was. Two frames stale, which for a log line is exact enough.
    frame_stats_readback_ = create_staging_buffer(
        device_, kFrameStatsSlots * sizeof(FrameStatistics), "frame statistics readback");

    // TEMPORARY PROBE: put back the device memory the chunk buffers used to hold, to find out
    // whether the frame-time difference is placement rather than code.
    ballast_ = create_device_buffer(device_, 950ull << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    "ballast");

    // Where the lamps are, so a shadow ray can be aimed at one. Small enough to be a staging
    // buffer written straight from the CPU: a thousand lights is 28 KB and it only changes
    // when the world does.
    light_buffer_ = create_staging_buffer(device_, kMaxLights * sizeof(LightSource),
                                          "light list", VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    const u64 clip_bytes = kMaxClipPoolCells * sizeof(u32);
    clip_buffer_ = create_device_buffer(device_, clip_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        "clip cells");
    clip_staging_ = create_staging_buffer(device_, clip_bytes, "clip staging");

    // The chunk marcher's set — two output images, eleven world buffers and the parameter block —
    // is gone with R1e. It outlived its pipeline by one stage: `visibility.comp` moved onto
    // `node_layout_` when the node pool became the only marcher, and this set went on being
    // allocated, written every resize and filled with eleven buffers no shader declared. An
    // unread descriptor costs nothing per frame and hides everything behind it, which is why
    // trimming it is what lets the buffers under it go.

    // Resolve set: the visibility image in, the colour image out, plus the two tables it
    // needs to turn a voxel type into a colour.
    // Seven, not six: the last is the cloud history at kCloudBinding, which this pass reads so the
    // sky it draws is the same sky the path tracer draws. Binding numbers need not be contiguous,
    // and using the same number as the tracer means shaders/pt_clouds.glsl and its consumers do not
    // have to care which pass included them.
    // Nine now: 6 is the face store and 7 is the face-slot image, which together are how the
    // picture stops lighting itself per pixel and starts reading light off the surface.
    VkDescriptorSetLayoutBinding resolve_bindings[15]{};
    for (u32 i = 0; i < 6; ++i) {
        resolve_bindings[i].binding = i;
        // 0-1 images, 2-3 the type and visual tables, 4 the parameter block, 5 the
        // clipboard's held clip.
        resolve_bindings[i].descriptorType =
            (i < 2)   ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : (i < 4) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
            : (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        resolve_bindings[i].descriptorCount = 1;
        resolve_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    resolve_bindings[6].binding = 6;   // the face store
    resolve_bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_bindings[6].descriptorCount = 1;
    resolve_bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[7].binding = 7;   // which face each pixel is on
    resolve_bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    resolve_bindings[7].descriptorCount = 1;
    resolve_bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[8].binding = kCloudBinding;
    resolve_bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    resolve_bindings[8].descriptorCount = 2;   // read and write alternate; the shader picks by parity
    resolve_bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[9].binding = 8;   // the card's own light: ambient occlusion, per face (R10a)
    resolve_bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_bindings[9].descriptorCount = 1;
    resolve_bindings[9].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[10].binding = 9;   // the light meter's two slots (R6a)
    resolve_bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_bindings[10].descriptorCount = 1;
    resolve_bindings[10].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[11].binding = 10;   // what each face is made of (R4a)
    resolve_bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_bindings[11].descriptorCount = 1;
    resolve_bindings[11].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[12].binding = 11;   // ...and what it reflects, along sixteen directions (R4c)
    resolve_bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_bindings[12].descriptorCount = 1;
    resolve_bindings[12].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[13].binding = 12;   // ...and what it lets THROUGH (R4d)
    resolve_bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_bindings[13].descriptorCount = 1;
    resolve_bindings[13].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_bindings[14].binding = 14;   // ...and what the ray reached BEHIND it (R4d)
    resolve_bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    resolve_bindings[14].descriptorCount = 1;
    resolve_bindings[14].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo resolve_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    resolve_layout_info.bindingCount = 15;
    resolve_layout_info.pBindings = resolve_bindings;
    WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &resolve_layout_info, nullptr,
                                      &resolve_layout_));

    const VkDescriptorPoolSize pool_sizes[]{
        // Storage images, counted rather than guessed, because the old figure of 8 was already
        // short of what the three sets ask for and had survived only on a driver's willingness to
        // hand out more than the pool promised. Resolve takes six (the visibility image in, the
        // colour out, the face slots, two clouds, and R4d's behind-glass layer), the cloud pass
        // three, the node pool four (visibility, depth, face slots, behind glass) — thirteen.
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 24},
        // The two sets left here bind the type tables, the clip, the face store and its light.
        // Counted generously: running out of pool is a failure at start-up with a message
        // nobody connects to the binding they just added.
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 60},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    WS_VK(vkCreateDescriptorPool(device_.handle(), &pool_info, nullptr, &descriptor_pool_));

    VkDescriptorSetAllocateInfo resolve_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    resolve_alloc.descriptorPool = descriptor_pool_;
    resolve_alloc.descriptorSetCount = 1;
    resolve_alloc.pSetLayouts = &resolve_layout_;
    WS_VK(vkAllocateDescriptorSets(device_.handle(), &resolve_alloc, &resolve_set_));

    // The cloud pass's set, allocated here with the others because create_render_target writes
    // image descriptors into it and cannot write into a set that does not exist yet.
    {
        // Three bindings, and they keep the numbers clouds.comp already names: the parameter
        // block at 13, the cloud history at 20 and this frame's marches at 21.
        //
        // This was the path tracer's whole set -- two images, the eleven world buffers, the type
        // tables, the clip, the face cache, the light list and the frame statistics -- and R3d
        // kept it entire because the cloud pass is passed that layout and the binding numbers were
        // ones gpu/render_params.hpp and the shaders agree about. Nothing had to be renumbered in
        // the end: a Vulkan layout's binding numbers need not be contiguous, so the seventeen
        // descriptors no shader declares are simply not there, and the buffers behind them are
        // free to go with them. That is what unblocked the rest of R1e.
        VkDescriptorSetLayoutBinding cloud_bindings[3]{};
        cloud_bindings[0].binding = 13;
        cloud_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        cloud_bindings[0].descriptorCount = 1;
        cloud_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        cloud_bindings[1].binding = kCloudBinding;
        cloud_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        // Two at the cloud binding: the history is read and written alternately, so both are
        // bound and the shader picks by parity rather than the descriptors being rewritten.
        cloud_bindings[1].descriptorCount = 2;
        cloud_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        cloud_bindings[2].binding = kCloudMarchedBinding;
        cloud_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        cloud_bindings[2].descriptorCount = 1;
        cloud_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo cloud_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        cloud_layout_info.bindingCount = 3;
        cloud_layout_info.pBindings = cloud_bindings;
        WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &cloud_layout_info, nullptr,
                                          &cloud_layout_));

        VkDescriptorSetAllocateInfo cloud_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        cloud_alloc.descriptorPool = descriptor_pool_;
        cloud_alloc.descriptorSetCount = 1;
        cloud_alloc.pSetLayouts = &cloud_layout_;
        WS_VK(vkAllocateDescriptorSets(device_.handle(), &cloud_alloc, &cloud_set_));
    }

    {
        const VkExtent2D render = scaled_extent();
        create_render_target(render.width, render.height);
    }

    // The pipelines. This was the rest of the wait while the reference tracer existed: one large
    // shader whose compile is seconds rather than milliseconds on a cold driver cache, measured at
    // 8,053 ms against 551 for a second run of the same build. R3d deleted it; this is 7 ms now. This is the last stage, so it is also
    // the one that has to be included or the bar reaches its end and the screen stays up anyway.
    progress_.enter(LoadStage::Settling);
    draw_loading();

    // Compiled shaders sit beside the executable, and *beside* means beside the one that is
    // running ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â asked at run time, not baked in at build time. The source tree location
    // still comes from the build, because hot reload only ever runs where the source is.
    const std::filesystem::path shaders = compiled_shader_dir();

    // The node pool, its buffers, its descriptor set and its pipeline. Created unconditionally
    // rather than behind the flag: the point of R1c is that the two marchers can be swapped at
    // run time and diffed on one camera, and a path that is only built when it is asked for is a
    // path nobody notices has stopped compiling.
    {
        NodePoolBudget node_budget;
        WS_LOG_INFO("load", "type tables {:.0f} ms  [t+{:.0f} ms]",
                ns_to_ms(now_ns() - t_tables), ns_to_ms(now_ns() - load_began_ns_));
        const u64 t_pool = now_ns();
        node_pool_.create(node_budget, types_);
        const u64 t_node_buffers = now_ns();
        WS_LOG_INFO("load", "node pool {:.0f} ms", ns_to_ms(t_node_buffers - t_pool));
        if (!node_buffers_.create(device_, node_budget)) {
            WS_LOG_FATAL("app", "could not create the node pool buffers");
            return 1;
        }
        FaceStoreBudget face_budget;
        if (options_.face_budget > 0) face_budget.max_faces = options_.face_budget;
        face_budget.pressure = options_.face_pressure;
        if (options_.face_pressure_from > 0) face_budget.pressure_from = options_.face_pressure_from;
        if (options_.secondary_share > 0) face_budget.secondary_share = options_.secondary_share;
        face_budget.keep_stand_ins = options_.coarse_keep;
        face_budget.class_eviction = options_.class_eviction;
        face_budget_max_ = face_budget.max_faces;
        face_store_.create(face_budget);
        if (!face_buffers_.create(device_, face_budget)) {
            WS_LOG_FATAL("app", "could not create the face store buffers");
            return 1;
        }
        face_buffers_.set_whole_set_retry(options_.whole_set_retry);
        // Every slot the face pass may write: the store's capacity plus the card's provisional
        // tail. Not the watermark, which grows.
        // Nineteen words a slot: the two ambient sample counts packed in one word, the near-field
        // contact sum, its gradient along each of the face's two axes, the far field's own count of
        // rays that reached open sky, three floats of accumulated lamp irradiance, one word holding
        // the lamp sample count with the emitter-list version those samples were taken under, and
        // three floats of accumulated BOUNCE radiance over the far field's own count -- then seven
        // more holding the FILTERED far field, bounce and lamp, which R5a writes from this face's
        // coplanar neighbours and which only the composite reads.
        // ...and one more holding WHICH WAY the lamps are, as an octahedral direction, so a polished
// surface can draw a highlight of a sconce rather than only be lit by it (kFaceLampDir).
// kFaceLightWords in shaders/face_terms.glsl is the same twenty, and the shader bounds its
        // writes against this length because a disagreement here is a write into whatever the
        // allocator placed next (D332).
        if (!face_light_.create(device_, 20 * (face_buffers_.provisional_base() +
                                               FaceBuffers::provisional_count()))) {
            WS_LOG_FATAL("app", "could not create the face light buffer");
            return 1;
        }
        // And one word a slot for when a pixel last read that face, over exactly the same range.
        if (!face_seen_.create(device_,
                               face_buffers_.provisional_base() + FaceBuffers::provisional_count(),
                               "face seen")) {
            WS_LOG_FATAL("app", "could not create the face seen buffer");
            return 1;
        }
        // And one word a slot for when a GATHERING RAY last read that face, over the same range
        // again. It is what tells the shading pass that a face nobody can see is nonetheless being
        // integrated by somebody, which is the only reason to spend a ray on one. R9b.
        if (!face_gathered_.create(device_,
                                   face_buffers_.provisional_base() +
                                       FaceBuffers::provisional_count(),
                                   "face gathered")) {
            WS_LOG_FATAL("app", "could not create the face gathered buffer");
            return 1;
        }
        // And one word a slot for when that face last told the HOST it was being read. Over the same
        // range as `face seen`, so a provisional slot has a word even though it never reports.
        if (!face_read_.create(device_,
                               face_buffers_.provisional_base() + FaceBuffers::provisional_count(),
                               "face read")) {
            WS_LOG_FATAL("app", "could not create the face read buffer");
            return 1;
        }
        // And one word a slot for what that face is MADE of, over the same range again. It is the
        // one thing about a face that comes from neither the store nor the light: the store knows
        // where a face is and the light pass knows what arrives there, and until R4 nothing at all
        // knew whether the surface was stone or gilt. See `face_material` in shaders/node.glsl.
        if (!face_material_.create(device_,
                                   face_buffers_.provisional_base() +
                                       FaceBuffers::provisional_count(),
                                   "face material")) {
            WS_LOG_FATAL("app", "could not create the face material buffer");
            return 1;
        }
        // And R4c's pool of outgoing bins, which is the first thing on a face that is NOT one word
        // a slot. A lobe is thirty-four words; laying one out for all 1,081,344 slots would be
        // 147 MB to hold nought on nine faces in ten, which is the sentence §4 of the plan writes
        // as *a matte stone wall allocates no payload at all*. So it is sixty-five thousand blocks
        // that faces hold and give up, sized against R4a's census -- 35,950 faces of 801,175 on
        // this building carry any metal. kLobeBlocks and kLobeBins in shaders/face_terms.glsl are
        // the same two numbers and the shader bounds every write against this length, because a
        // stride the host and the shader disagree about is a write into whatever the allocator put
        // next (D332).
        if (!face_lobe_.create(device_, kLobePoolWords, "face lobe")) {
            WS_LOG_FATAL("app", "could not create the face lobe pool");
            return 1;
        }
        // And what each face lets THROUGH, over the same range as the material beside it. R4d.
        if (!face_medium_.create(device_,
                                 face_buffers_.provisional_base() +
                                     FaceBuffers::provisional_count(),
                                 "face medium")) {
            WS_LOG_FATAL("app", "could not create the face medium buffer");
            return 1;
        }
        // And the gathering ray's own counters, which are not per slot at all: one word a QUESTION,
        // over the whole dispatch. See kLightProbeWords in shaders/node.glsl for the word map and
        // for why word 0 is the host's and the rest are the card's.
        if (!light_probe_.create(device_, kLightProbeWords, "light probe")) {
            WS_LOG_FATAL("app", "could not create the light probe buffer");
            return 1;
        }
        // And one word a NODE slot, for the same job on the other array: which nodes the light has
        // already said it is using this window. Sized to the pool's capacity rather than its
        // watermark, because a slot the pool has not reached yet is a slot it will reach.
        if (!node_seen_.create(device_, node_budget.max_nodes, "node seen")) {
            WS_LOG_FATAL("app", "could not create the node seen buffer");
            return 1;
        }
        // The work list: a dispatch command, a count, and one slot per face in the store. Only the
        // store's own range is ever compacted -- the card's provisional tail is a separate, small,
        // contiguous dispatch -- so it is sized to the store and not to the store plus the tail.
        const u64 work_bytes =
            (4ull + face_buffers_.provisional_base()) * sizeof(u32);
        face_work_ = create_device_buffer(device_, work_bytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          "face work list");
        if (!face_work_.valid()) {
            WS_LOG_FATAL("app", "could not create the face work list");
            return 1;
        }
        WS_LOG_INFO("load", "node buffers {:.0f} ms  [t+{:.0f} ms]",
                    ns_to_ms(now_ns() - t_node_buffers), ns_to_ms(now_ns() - load_began_ns_));

        // 0-1 out images, 2-6 the pool, 7 feedback, 8 the parameter block, 9-10 the faces,
        // 11 the face-slot image, 12 the card's provisional faces, 13 the face light, 14 the lamps.
        //
        // One set for both the marcher and the face shader. They need the same tree ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the shading
        // pass marches shadow rays through exactly the geometry the primary ray stopped on ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and
        // two sets would be two places for the same buffers to be bound, which is how the node
        // pipeline's output images came to be written in one place and forgotten in the other
        // (Ãƒâ€šÃ‚Â§4 trap 1). A shader need not use every binding in a set.
        // Thirteen: 0-1 the visibility and depth images, 2-7 the pool and feedback, 8 the parameter
        // block, 9-10 the face store, 11 the face-slot image the composite reads, 12 the card's own
        // provisional face table (R3e), which is the store's shape minus the host.
        // Fourteen: 13 is the face light, which the card writes and the card reads and the host
        // never touches at all (R10a).
        // Fifteen: 14 is the emitter list, so a FACE can aim at a lamp (R3c). It is the same buffer
        // the path tracer reads at its own binding 18 -- one list, so the reference renderer and
        // the real-time one cannot disagree about where the lamps are or how bright they are.
        // Sixteen: 15 is the face SEEN stamp, written by the visibility pass and read by the
        // shading pass, which is how the light pass finally stopped lighting what nobody can see.
        // Seventeen: 16 is the compacted work list, so the shading dispatch is sized by how many
        // faces owe work rather than by how many exist.
        // Eighteen: 17 is the node SEEN stamp, which is to the light's reads what 15 is to the
        // pixel's -- a card-owned word a slot that turns one entry per ray into one per node per
        // window. D430.
        // Nineteen: 18 is the face READ stamp, the same thing again for the store (D508).
        // Twenty: 19 is the light probe -- the gathering ray's own counters, and the dials that
        // could not fit in a push block that is exactly 128 bytes full. R9f.
        // Twenty-one: 20 is the face GATHERED stamp, which is to a gathering ray's reads what 15 is
        // to a pixel's. It is what makes the off-screen set worth having: without it every face in
        // that class held nought samples for its whole life. R9b.
        // Twenty-four: 21 is what each face is MADE of, and 22 and 23 are the two interned tables it
        // is worked out from -- the same two buffers the composite has always had at its own
        // bindings 2 and 3. Until R4 no pass on this set had any reason to read them, which is why
        // the light pass reads the marcher's folded average colour for an albedo and has never been
        // able to ask about a roughness at all.
        // Twenty-five: 26 is the third image this set writes -- what the primary ray reached once
        // transmissive matter let it past. R4d.
        VkDescriptorSetLayoutBinding node_bindings[27]{};
        for (u32 i = 0; i < 27; ++i) {
            node_bindings[i].binding = i;
            node_bindings[i].descriptorType =
                (i < 2 || i == 11 || i == 26) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : (i == 8)                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            node_bindings[i].descriptorCount = 1;
            node_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo node_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        node_layout_info.bindingCount = 27;
        node_layout_info.pBindings = node_bindings;
        WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &node_layout_info, nullptr,
                                          &node_layout_));

        VkDescriptorSetAllocateInfo node_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        node_alloc.descriptorPool = descriptor_pool_;
        node_alloc.descriptorSetCount = 1;
        node_alloc.pSetLayouts = &node_layout_;
        WS_VK(vkAllocateDescriptorSets(device_.handle(), &node_alloc, &node_set_));

        const VkBuffer node_pool_buffers[22]{
            node_buffers_.entries(), node_buffers_.nodes(),     node_buffers_.leaves(),
            node_buffers_.occupancy(), node_buffers_.payload(), feedback_.buffer(),
            face_buffers_.faces(), face_buffers_.entries(),    face_buffers_.provisional(),
            face_light_.buffer(),      light_buffer_.buffer,    face_seen_.buffer(),
            face_work_.buffer,         node_seen_.buffer(),     face_read_.buffer(),
            light_probe_.buffer(),     face_gathered_.buffer(), face_material_.buffer(),
            type_tables_.types(),      type_tables_.visuals(),  face_lobe_.buffer(),
            face_medium_.buffer(),
        };
        // Spelled out rather than derived. The mapping had grown a chain of conditionals with two
        // holes in it -- 8 is the parameter block and 11 is an image -- and a third hole would have
        // made it unreadable in the one place where being wrong is silent.
        const u32 node_bindings_for[22]{2,  3,  4,  5,  6,  7,  9,  10, 12, 13, 14,
                                        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
        VkDescriptorBufferInfo node_infos[22]{};
        // Twenty-two writes for twenty-one buffers and one uniform block, and the COUNT a few lines
        // below is the thing to change with them. Adding a descriptor here and leaving that literal
        // alone writes every binding but the new one, silently, and `--validation` is the only thing
        // that says so -- which is D518 exactly, in the other direction.
        VkWriteDescriptorSet node_writes[23]{};
        for (u32 i = 0; i < 22; ++i) {
            node_infos[i].buffer = node_pool_buffers[i];
            node_infos[i].offset = 0;
            node_infos[i].range = VK_WHOLE_SIZE;
            node_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            node_writes[i].dstSet = node_set_;
            node_writes[i].dstBinding = node_bindings_for[i];
            node_writes[i].descriptorCount = 1;
            node_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            node_writes[i].pBufferInfo = &node_infos[i];
        }
        VkDescriptorBufferInfo node_params{};
        node_params.buffer = params_buffer_.buffer;
        node_params.offset = 0;
        node_params.range = sizeof(RenderParams);
        node_writes[22].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        node_writes[22].dstSet = node_set_;
        node_writes[22].dstBinding = 8;
        node_writes[22].descriptorCount = 1;
        node_writes[22].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        node_writes[22].pBufferInfo = &node_params;
        vkUpdateDescriptorSets(device_.handle(), 23, node_writes, 0, nullptr);

        // And the output images, which the render target owns. It was created before this
        // set existed, so its own binding pass skipped them.
        VkDescriptorImageInfo node_vis{};
        node_vis.imageView = visibility_image_.view;
        node_vis.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo node_depth{};
        node_depth.imageView = depth_target_.view;
        node_depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet node_images[4]{};
        for (u32 i = 0; i < 2; ++i) {
            node_images[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            node_images[i].dstSet = node_set_;
            node_images[i].dstBinding = i;
            node_images[i].descriptorCount = 1;
            node_images[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorImageInfo node_face{};
        node_face.imageView = face_image_.view;
        node_face.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        node_images[0].pImageInfo = &node_vis;
        node_images[1].pImageInfo = &node_depth;
        node_images[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        node_images[2].dstSet = node_set_;
        node_images[2].dstBinding = 11;
        node_images[2].descriptorCount = 1;
        node_images[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        node_images[2].pImageInfo = &node_face;
        VkDescriptorImageInfo node_behind{};
        node_behind.imageView = behind_image_.view;
        node_behind.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        node_images[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        node_images[3].dstSet = node_set_;
        node_images[3].dstBinding = 26;
        node_images[3].descriptorCount = 1;
        node_images[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        node_images[3].pImageInfo = &node_behind;
        vkUpdateDescriptorSets(device_.handle(), 4, node_images, 0, nullptr);

        const std::filesystem::path node_spirv = shaders / "visibility.comp.spv";
        const std::filesystem::path node_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "visibility.comp";
        // The face shader shares the marcher's set: a shadow ray has to march exactly the
        // geometry the primary ray stopped on, so giving it its own set would be two places to
        // bind the same buffers. Its push constant is larger -- a sun direction and a count --
        // and a pipeline layout takes the size it is given, so they are made separately.
        const std::filesystem::path shade_spirv = shaders / "shade_faces.comp.spv";
        const std::filesystem::path shade_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "shade_faces.comp";
        if (!shade_faces_.create(device_, shade_source, shade_spirv, node_layout_,
                                 sizeof(NodePush))) {
            WS_LOG_FATAL("app", "could not create the face shading pipeline: {}",
                         shade_faces_.last_error());
            return 1;
        }

        // The compaction pass that decides which slots the shading dispatch covers.
        const std::filesystem::path worklist_spirv = shaders / "face_worklist.comp.spv";
        const std::filesystem::path worklist_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "face_worklist.comp";
        if (!face_worklist_.create(device_, worklist_source, worklist_spirv, node_layout_,
                                   sizeof(NodePush))) {
            WS_LOG_FATAL("app", "could not create the face work list pipeline: {}",
                         face_worklist_.last_error());
            return 1;
        }

        // The same push range as the face shader, because they include the same file and a
        // stage may declare only one block: the marcher writes the first two fields and ignores
        // the rest, but its layout has to reserve what the block declares.
        if (!visibility_.create(device_, node_source, node_spirv, node_layout_,
                                     sizeof(NodePush))) {
            WS_LOG_FATAL("app", "could not create the node visibility pipeline: {}",
                         visibility_.last_error());
            return 1;
        }
        }

    const u64 t_pipelines = now_ns();

    progress_.within(0.25);
    draw_loading();

    const std::filesystem::path resolve_spirv = shaders / "resolve.comp.spv";
    const std::filesystem::path resolve_source =
        std::filesystem::path(WS_SHADER_SOURCE_DIR) / "resolve.comp";
    // The same push constant the tracer takes. It carries the sun, the weather and the air, none
    // of which this pass could see before ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is why it drew a hardcoded gradient.
    if (!resolve_.create(device_, resolve_source, resolve_spirv, resolve_layout_,
                         sizeof(TracePush))) {
        WS_LOG_FATAL("app", "could not create the resolve pipeline: {}",
                     resolve_.last_error());
        return 1;
    }

    progress_.within(0.40);
    draw_loading();

    {
        // The cloud volume, on what used to be the tracer's set and push constants. The set is
        // kept whole rather than trimmed to what the cloud pass names: its binding numbers are
        // ones the shaders and gpu/render_params.hpp agree about, and renumbering them is R3d's
        // follow-on rather than part of deleting the pass.
        const std::filesystem::path cloud_spirv = shaders / "clouds.comp.spv";
        const std::filesystem::path cloud_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "clouds.comp";
        if (!clouds_.create(device_, cloud_source, cloud_spirv, cloud_layout_,
                            sizeof(TracePush))) {
            // Not fatal. A sky with no cloud in it is a worse picture and a working one.
            WS_LOG_ERROR("app", "no clouds this run: {}", clouds_.last_error());
        }

        WS_LOG_INFO("load", "pipelines {:.0f} ms", ns_to_ms(now_ns() - t_pipelines));
    }

    // The two tables the composite turns a voxel into a colour with. Everything else the marcher
    // used to be handed -- the wrapped chunk grid, the records, the masks, the popcount prefixes,
    // the headers, the occupancy, the payload, the coarse grids and the two thumbnail buffers --
    // went with the set it was bound to. The node pool's own buffers live on `node_set_`.
    //
    // Four now: the face store joins them, because the composite reads light off a face rather
    // than working it out per pixel.
    // Five now: the face light joins them, so the composite can read how much sky a surface can
    // actually see instead of deciding it from which way the surface points (R10a).
    // Six now: the light meter's two slots join them (R6a).
    // Seven now: what each face is made of, which this pass turns into a diffuse share, a specular
    // colour and a lobe width (R4a).
    // Eight now: the pool of outgoing bins, which is what that lobe is FILLED with -- the composite
    // reads the one bin the eye is looking down and adds it (R4c).
    const VkBuffer resolve_buffers[]{type_tables_.types(), type_tables_.visuals(),
                                     clip_buffer_.buffer, face_buffers_.faces(),
                                     face_light_.buffer(), frame_stats_.buffer,
                                     face_material_.buffer(), face_lobe_.buffer(),
                                     face_medium_.buffer()};

    // The parameter block, bound to the composite and to the cloud pass with a dynamic offset
    // chosen per frame.
    VkDescriptorBufferInfo params_info{};
    params_info.buffer = params_buffer_.buffer;
    params_info.offset = 0;
    params_info.range = sizeof(RenderParams);
    VkWriteDescriptorSet params_writes[2]{};
    for (u32 i = 0; i < 2; ++i) {
        params_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        params_writes[i].dstSet = (i == 0) ? cloud_set_ : resolve_set_;
        params_writes[i].dstBinding = (i == 0) ? 13 : 4;
        params_writes[i].descriptorCount = 1;
        params_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        params_writes[i].pBufferInfo = &params_info;
    }
    vkUpdateDescriptorSets(device_.handle(), 2, params_writes, 0, nullptr);

    // types, visuals, clip cells, faces, face light, the meter, the face material, the lobe pool
    constexpr u32 kResolveBuffers = 9;
    VkDescriptorBufferInfo buffer_infos[kResolveBuffers]{};
    VkWriteDescriptorSet buffer_writes[kResolveBuffers]{};
    // Resolve's storage buffers are bindings 2, 3, 5, 6, 8, 9, 10 and 11 -- 4 is the parameter block
    // and 7 is an image. Spelled out for the same reason the node set's mapping is.
    const u32 resolve_bindings_for[kResolveBuffers]{2, 3, 5, 6, 8, 9, 10, 11, 12};
    for (u32 i = 0; i < kResolveBuffers; ++i) {
        buffer_infos[i].buffer = resolve_buffers[i];
        buffer_infos[i].offset = 0;
        buffer_infos[i].range = VK_WHOLE_SIZE;
        buffer_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        buffer_writes[i].dstSet = resolve_set_;
        buffer_writes[i].dstBinding = resolve_bindings_for[i];
        buffer_writes[i].descriptorCount = 1;
        buffer_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        buffer_writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_.handle(), kResolveBuffers, buffer_writes, 0, nullptr);

    // The test scene spans about 64 m. Start at one corner of it, above the ground slab,
    // looking back toward the origin so the towers, arch and lattice are all in frame.
    // On the approach, off the axis, looking up at the portico.
    //
    // Where a building is first seen from is a decision somebody makes, and for a building with a
    // front it is not a corner of the bounding box. The facility faces south ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â down negative z ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â
    // so this stands out on the lawn a little to the west of the centre line, at the height of
    // somebody's eyes, and looks back at the steps and the columns above them. Three quarters
    // rather than square on, because a portico read head-on is a row of verticals and read at an
    // angle is a building.
    // The origin, standing in the middle of the rotunda looking out of the main door.
    //
    // Yaw of minus ninety because forward is (cos yaw, sin pitch, sin yaw) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so minus ninety is
    // straight down negative z, and the main door is the opening at z -7.35 on that face.
    camera_.set_position_metres(0.0, 0.0, 0.0);
    camera_.set_look(-90.0, 0.0);
    if (!options_.camera.empty()) {
        f64 values[5]{-22.0, 5.0, -22.0, 45.0, -8.0};
        const char* cursor = options_.camera.c_str();
        for (u32 i = 0; i < 5 && *cursor != '\0'; ++i) {
            values[i] = std::strtod(cursor, const_cast<char**>(&cursor));
            if (*cursor == ',') ++cursor;
        }
        camera_.set_position_metres(values[0], values[1], values[2]);
        camera_.set_look(values[3], values[4]);
        for (u32 i = 0; i < 5; ++i) fly_state_[i] = values[i];
    }
    if (!options_.orbit.empty()) {
        f64 values[4] = {30.0, 6.0, 25.0, 26.0};
        parse_reals(options_.orbit, values, 4);
        for (u32 i = 0; i < 4; ++i) orbit_[i] = values[i];
        orbiting_ = true;
    }
    for (const std::string& spec : options_.cuts) {
        if (spec.empty()) continue;
        f64 values[6] = {30.0, 0.0, 0.0, 0.0, 90.0, 0.0};
        parse_reals(spec, values, 6);
        Cut cut;
        cut.at = static_cast<u64>(values[0] < 0.0 ? 0.0 : values[0]);
        for (u32 i = 0; i < 5; ++i) cut.pose[i] = values[i + 1];
        // A cut that cannot fire because the one before it is later is a run that measures the
        // wrong thing and says nothing. Two arms of an A/B would both look clean, which is
        // exactly trap 15, so it is reported here rather than discovered from a picture.
        if (!cuts_.empty() && cut.at <= cuts_.back().at) {
            WS_LOG_WARN("frame",
                        "--cut at measured frame {} is not after the cut before it at {}; the "
                        "cuts fire in the order they were given, so this one will fire on the "
                        "same frame and the camera will end up at the LAST of them",
                        cut.at, cuts_.back().at);
        }
        cuts_.push_back(cut);
    }
    if (!options_.fly.empty()) {
        const char* cursor = options_.fly.c_str();
        for (u32 i = 0; i < 4 && *cursor != '\0'; ++i) {
            fly_velocity_[i] = std::strtod(cursor, const_cast<char**>(&cursor));
            if (*cursor == ',') ++cursor;
        }
        flying_ = true;
    }
    debug_mode_ = options_.debug_mode;

    // The monitor decides the target unless someone says otherwise: rendering faster than the
    // display can show is work nobody sees, and that spare time buys samples instead.
    quality_.create(window_.refresh_hz(), kQualityLevels - 1);
    benchmark_pending_ = true;
    benchmark_until_ = kBenchmarkWarmupFrames + kBenchmarkFrames;
    load_settings();   // a remembered level cancels the benchmark
    if (options_.target_fps > 0.0f) quality_.set_target_fps(options_.target_fps);
    if (options_.no_auto_quality) quality_.set_enabled(false);
    if (options_.quality_level >= 0) {
        quality_.set_level(static_cast<u32>(options_.quality_level));
        benchmark_pending_ = false;
    }
    // A scripted run must be repeatable, so it never benchmarks, never drifts, and ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â this is
    // the part that matters ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â ignores whatever level was saved. Otherwise every screenshot in
    // this repository would be taken at whatever quality the last interactive session happened
    // to settle on, and two measurements taken a day apart would not be comparable. Full
    // detail unless a level was named outright.
    if (!options_.screenshot.empty() && !options_.benchmark) {
        benchmark_pending_ = false;
        quality_.set_enabled(false);
        if (options_.quality_level < 0) quality_.set_level(kQualityLevels - 1);
    }
    if (options_.benchmark) {
        benchmark_pending_ = true;
        benchmark_total_ms_ = 0.0;
        benchmark_frames_ = 0;
        benchmark_worst_ms_ = 0.0;
        benchmark_until_ = frame_counter_ + kBenchmarkWarmupFrames + kBenchmarkFrames;
        quality_.set_level(kQualityLevels - 1);   // measured at full detail or not at all
    }
    apply_quality();
    applied_quality_level_ = quality_.level();
    seed_knobs();
    WS_LOG_INFO("quality", "target {:.0f} fps ({}), level {} of {}{}", quality_.target_fps(),
                options_.target_fps > 0.0f ? "asked for" : "the monitor's refresh rate",
                quality_.level(), kQualityLevels - 1,
                benchmark_pending_ ? ", benchmarking this machine first" : "");

    progress_.within(0.85);
    draw_loading();
    if (!hud_.create(device_, window_, swapchain_.format())) return 1;

    // A hundred per cent, and the next thing that happens is a real frame.
    //
    // This is the whole promise of the screen and it is kept here rather than anywhere earlier:
    // there is no work left between this line and the frame loop, so the bar filling and the game
    // appearing are the same instant. A bar that reached its end and then made the player wait
    // would have measured everything except the thing they were waiting for.
    // Deliberately not drawn again. A last frame showing a full bar would cost one present
    // between the bar filling and the game appearing, which is precisely the gap the player
    // reports as "it says a hundred and then hangs" ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so the bar's last drawn state is the high
    // nineties and the next thing on the screen is the world.
    WS_LOG_INFO("load", "everything ready  [t+{:.0f} ms]", ns_to_ms(now_ns() - load_began_ns_));
    progress_.finish();
    const LoadHistory measured_load = progress_.history(load_history_);
    progress_.history(load_history_).write(loading_cache_path());

    // What loading actually spent, per stage.
    //
    // These have been measured since the loading bar existed, to weight the bar, and never once
    // printed -- so "it takes five seconds" has never had a breakdown behind it. A warm start
    // reads the world from cache in 146 ms and still takes 4.85 s, and until this line nothing
    // said where the rest went.
    {
        std::string breakdown;
        for (u32 i = 0; i < static_cast<u32>(LoadStage::Count); ++i) {
            // Two shapes are kept apart -- a cold build and a cache hit spend themselves on
            // entirely different stages -- so report whichever this run actually was.
            const usize shape = LoadProgress::likely_cached(measured_load) ? LoadHistory::kCached
                                                                          : LoadHistory::kBuilt;
            const f64 seconds = measured_load.seconds[shape][i];
            if (seconds < 0.001) continue;
            if (!breakdown.empty()) breakdown += "  ";
            breakdown += std::string(stage_name(static_cast<LoadStage>(i))) + " " +
                         std::to_string(static_cast<int>(seconds * 1000.0)) + "ms";
        }
        WS_LOG_INFO("app", "load stages: {}", breakdown);
    }

    // The screen holds a full-resolution image and startup is the only thing that needs it.
    loading_screen_.destroy();

    // The compile time, every run. A stale binary is otherwise invisible: the build tool
    // once reported "no work to do" over a source file that had changed, and a measurement
    // was taken against code that no longer existed. One line makes that impossible to miss.
    WS_LOG_INFO("app", "WorldShaper {}, compiled {} {}", kVersionTag, __DATE__, __TIME__);

    // Sweep up the previous executable an earlier update left behind, then ask GitHub
    // whether there is a newer release. The check is on its own thread and never blocks
    // starting; nothing is downloaded unless the player says so.
    Updater::clean_up_previous();
    hollow_ = options_.hollow;
    if (!options_.no_update_check) updater_.begin_check();
    WS_LOG_INFO("app", "ready. F1 developer panel, F2 overlay, F5 reload shaders, Esc quit");

    const u64 start_ns = now_ns();
    u64 last_ns = start_ns;

    // Nothing held, nothing pressed, nothing typed and no wheel. What the game is given on the
    // frames the interface has the input, so that "the shell has it" is one branch here rather than
    // a condition every tool has to remember to ask.
    const InputState kNoInput{};

    while (window_.pump()) {
        const u64 frame_start = now_ns();
        stats_.push(ns_to_ms(frame_start - last_ns));
        last_ns = frame_start;
        if (stats_.last_ms() > worst_frame_ms_) {
            worst_frame_ms_ = stats_.last_ms();
            worst_frame_at_ = frame_counter_;
        }
        // A graphics driver resets a GPU that has not answered in about two seconds, and the
        // reset arrives as a lost device with no explanation attached. A frame this slow is
        // already most of the way there, so it is worth a line of its own.
        if (stats_.last_ms() > 200.0) {
            WS_LOG_WARN("frame", "frame {} took {:.0f} ms", frame_counter_, stats_.last_ms());
        }

        const InputState& input = window_.input();
        // Escape, in ONE step, and it is not "quit".
        //
        // It was two: the first press gave the mouse back and the second opened a window — so the
        // key everybody in the world presses to reach the settings had to be pressed twice before
        // anything appeared at all, and the thing that appeared was the library on its own. Both
        // halves of that were wrong for the same reason. Giving the mouse back is not a state a
        // player asked to be in; it is what opening the menu *costs*, so it belongs to the same
        // press. And the two families of window are one state, not two (D443, `Shell::open_windows`).
        //
        // So this is a toggle between two named states and there is no third. **Menu**: both windows
        // up, the pointer free, and the game deaf to everything (see `shell_has_input` below).
        // **Playing**: no windows, the mouse captured, every binding live. Closing puts the mouse
        // back where it was, because a menu you close is a game you are back in — which is also
        // what stops the old second-press-does-nothing frame from existing.
        if (input.was_pressed(Key::Escape) && !shell_.ui().wants_keys()) {
            if (shell_.windows_open()) {
                shell_.close_windows();
                mouse_look_ = true;
                // For the hand that was already holding a button when it reached for Escape: the
                // press it is still holding belongs to the menu it just left, not to the chisel.
                swallow_click_ = true;
                window_.set_relative_mouse(true);
            } else {
                shell_.open_windows();
                mouse_look_ = false;
                window_.set_relative_mouse(false);
            }
        }
        if (input.was_pressed(Key::F1)) hud_.toggle_developer_panel();
        if (input.was_pressed(Key::F2)) hud_.toggle_overlay();
        if (input.was_pressed(Key::F3)) debug_mode_ = (debug_mode_ + 1) % 7;
        if (input.was_pressed(Key::F5)) {
            resolve_.force_reload();
        }
        // Swap marchers where you are standing, without restarting.
        //
        // Both are built and both are fed every frame while R1e is outstanding, so this costs
        // a branch and nothing else ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and it is the only way to compare them on the thing a
        // fixed camera cannot show: what loading and turning round actually feel like. A
        // grid of settled means is blind to that by construction, which is how a marcher that
        // is faster on all seven cameras can still be reported as laggy and both be true.
        //
        // It is also the escape hatch. If the new one is worse in front of you, press F6 and
        // you are back on the old one for the rest of the session.
        if (input.was_pressed(Key::F11)) swapchain_.set_vsync(!swapchain_.vsync());
        // The only thing that starts a download. Nothing else does, and nothing does it
        // automatically.
        if (input.was_pressed(Key::F8) && updater_.state() == UpdateState::Available) {
            updater_.begin_download();
        }

        // Who this frame's input belongs to, decided ONCE and for everything.
        //
        // The shell's windows are a mode rather than an overlay. While they are up the pointer is
        // free and the mouse is not captured — and every key the game binds went on reaching the
        // world anyway, because only the *mouse* was ever asked about. So dragging a slider on the
        // left of the screen also flew the camera with the other hand, the wheel over a value also
        // changed the flight speed, a digit typed into a field also swapped the tool in your hand,
        // and Z was undo while you were reading the settings. Those are one bug, not eight, and the
        // fix is one line: the game is handed an EMPTY input for those frames rather than being
        // asked to remember, binding by binding, who each key was for.
        //
        // `wants_keys` is in it for the frame a field takes focus on: the windows say the mode and
        // the field says the keyboard, and either of them is enough.
        //
        // The F-keys above are deliberately outside this. They are the developer's, they are not
        // bound to anything in the world, and a panel that could not be opened over the menu would
        // be a panel that cannot be opened while looking at the menu.
        const bool shell_has_input = shell_.windows_open() || shell_.ui().wants_keys();
        const InputState& game = shell_has_input ? kNoInput : input;

        // Clicking the world captures the mouse; from then on the buttons belong to the
        // chisel. That first click is swallowed, or capturing the mouse would also start a
        // cut you never asked for.
        //
        // With the windows up, a press in the middle — the part of the screen a window may never
        // cover — is the other way back to playing, and it does exactly what Escape does. Without
        // it the only way out of the menu is a key, which is the one thing an interface a child who
        // cannot read can use is not allowed to require.
        if (shell_has_input) {
            if (input.mouse_left_pressed && shell_.windows_open() &&
                shell_.centre().holds(input.mouse_x, input.mouse_y) && !hud_.wants_mouse()) {
                shell_.close_windows();
                mouse_look_ = true;
                swallow_click_ = true;
                window_.set_relative_mouse(true);
            }
        } else if (!mouse_look_) {
            if ((input.mouse_left || input.mouse_right) && !hud_.wants_mouse() &&
                !shell_.ui().wants_mouse()) {
                mouse_look_ = true;
                swallow_click_ = true;
                window_.set_relative_mouse(true);
            }
        }
        if (mouse_look_ && swallow_click_ && !input.mouse_left && !input.mouse_right) {
            swallow_click_ = false;
        }

        resolve_.reload_if_changed();

        // Who gets the wheel this frame. It is the most contested input in the game, so the
        // rule is written once, here, rather than being discovered by each tool:
        //   a number key held  -> cycle tools within that slot
        //   G held             -> the chisel's working distance
        //   otherwise          -> the active tool, and the free camera if it does not want it
        u32 held_slot = kToolSlots;
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            const Key key = static_cast<Key>(static_cast<u16>(Key::Digit1) + slot);
            if (game.is_down(key)) {
                held_slot = slot;
                break;
            }
        }
        const bool cycling = held_slot < kToolSlots;

        // H takes the wheel and sets how thick a shell a placement leaves. Zero is solid,
        // which is where it starts and what it goes back to.
        //
        // It claims the wheel ahead of everything else, including tool cycling, because it is
        // a modifier you hold deliberately ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the same bargain G already makes for distance.
        const bool hollow_has_wheel = game.is_down(Key::H);
        if (hollow_has_wheel && game.wheel != 0.0f) {
            const i32 step = (game.wheel > 0.0f) ? 1 : -1;
            hollow_ = static_cast<u32>(std::max(0, static_cast<i32>(hollow_) + step));
            WS_LOG_INFO("tool", "hollow {}",
                        (hollow_ == 0) ? std::string("off (solid)")
                                       : std::to_string(hollow_) + " voxel shell");
        }

        const bool chisel_has_wheel = !hollow_has_wheel && game.is_down(Key::G);
        // The clipboard only claims the wheel once it is holding something. With nothing
        // selected it has nothing to slide, so the wheel goes back to flight speed ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which
        // is what you want while flying somewhere to make a selection.
        const bool clipboard_has_wheel = !cycling && !chisel_has_wheel && !hollow_has_wheel &&
                                         toolbelt_.active() == ToolKind::Clipboard &&
                                         clipboard_.holding();
        const bool tool_has_wheel = cycling || chisel_has_wheel || clipboard_has_wheel ||
                                    hollow_has_wheel;

        const ToolKind tool_before = toolbelt_.active();
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            const Key key = static_cast<Key>(static_cast<u16>(Key::Digit1) + slot);
            if (game.was_pressed(key)) toolbelt_.select_slot(slot);
        }
        if (cycling && game.wheel != 0.0f) {
            toolbelt_.select_slot(held_slot);
            toolbelt_.cycle(static_cast<i32>(game.wheel));
        }
        // Putting a tool away puts down what it was holding. A ghost that survived a trip
        // through the chisel and reappeared later would be a surprise, not a convenience.
        if (toolbelt_.active() != tool_before) clipboard_.drop();

        const f64 dt = (stats_.last_ms() > 0.0) ? stats_.last_ms() * 0.001 : 1.0 / 60.0;
        camera_.update(game, (dt > 0.1) ? 0.1 : dt, mouse_look_, !tool_has_wheel);

        // A fixed step rather than the real frame time, so the same frame number is the same
        // place on every machine and a measurement is comparable between builds.
        if (flying_) {
            constexpr f64 kStep = 1.0 / 60.0;
            for (u32 axis = 0; axis < 3; ++axis) {
                fly_state_[axis] += fly_velocity_[axis] * kStep;
            }
            fly_state_[3] += fly_velocity_[3] * kStep;
            camera_.set_position_metres(fly_state_[0], fly_state_[1], fly_state_[2]);
            camera_.set_look(fly_state_[3], fly_state_[4]);
        }

        // The cut, after the smooth motion so it wins, and once.
        //
        // Gated on the measurement window having started, which under --settle means the world has
        // stopped building ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so what the new view is missing is faces and nothing else. Without
        // that gate the cut fires during the load and measures streaming again, which is the
        // confusion this instrument exists to end.
        while (next_cut_ < cuts_.size() && (!options_.settle || settled_seen_) &&
               frame_counter_ - settle_frame_ >= cuts_[next_cut_].at) {
            const Cut& cut = cuts_[next_cut_];
            camera_.set_position_metres(cut.pose[0], cut.pose[1], cut.pose[2]);
            camera_.set_look(cut.pose[3], cut.pose[4]);
            for (u32 i = 0; i < 5; ++i) fly_state_[i] = cut.pose[i];
            ++next_cut_;
            // Which cut this was, not merely that one happened: a two-cut run whose second cut
            // never fired -- because the run ended first, or because its frame was behind the
            // first -- draws the picture of a camera that never came back, and that reads exactly
            // like the fault being measured.
            WS_LOG_INFO("frame", "camera cut {} of {} at measured frame {}, to {:.1f},{:.1f},{:.1f}",
                        next_cut_, cuts_.size(), cut.at, cut.pose[0], cut.pose[1], cut.pose[2]);
        }
        update_tools(game, chisel_has_wheel, clipboard_has_wheel, (dt > 0.1) ? 0.1 : dt);

        if (window_.minimised()) continue;
        if (window_.resized_this_frame() || swapchain_.needs_recreate()) handle_resize();

        update_quality();
        update_lights();

        // Where the camera was standing and what it was doing, refreshed every frame, so a
        // crash report says which part of the world provoked it rather than only which
        // function noticed. Two snprintfs a frame against a fault nobody can reproduce.
        {
            char where[160];
            std::snprintf(where, sizeof(where),
                          "frame %llu  chunk %lld,%lld,%lld  local %.1f,%.1f,%.1f",
                          static_cast<unsigned long long>(frame_counter_),
                          static_cast<long long>(camera_.chunk_x()),
                          static_cast<long long>(camera_.chunk_y()),
                          static_cast<long long>(camera_.chunk_z()), camera_.local_x(),
                          camera_.local_y(), camera_.local_z());
            crash_set_context("camera", where);
            char what[160];
            std::snprintf(what, sizeof(what),
                          "tool %d  debug %u  %u nodes over %zu chunks",
                          static_cast<int>(toolbelt_.active()), debug_mode_,
                          // live_stats(), never stats(): this runs every frame, and the walking
                          // version costs 1.76 ms of it. Measured, because it was written here
                          // as stats() first and the frame went 5.19 ms -> 7.27.
                          node_pool_.live_stats().nodes, world_.chunk_count());
            crash_set_context("state", what);
            char timing[160];
            std::snprintf(timing, sizeof(timing),
                          "last %.1f ms, worst %.1f ms at frame %llu, last GPU %.2f ms",
                          stats_.last_ms(), worst_frame_ms_,
                          static_cast<unsigned long long>(worst_frame_at_),
                          profiler_.total_gpu_ms());
            crash_set_context("timing", timing);
        }

        // A rung of the ladder, if one has finished sampling since the last frame.
        pump_refinement();

        // The interface, before the frame it changes. A world left here stops the loop rather
        // than drawing one more picture of a world that is going away.
        run_shell(ns_to_ms(frame_start - start_ns) * 0.001);
        // Leaving on a count rather than on a click, which is how the tear-down path gets
        // exercised without a hand on the keyboard. See Options::cycle.
        if (options_.cycle > 0 && frame_counter_ >= options_.cycle) {
            WS_LOG_INFO("shell", "leaving the world after {} frames, as asked", frame_counter_);
            wants_title_ = true;
        }
        if (wants_title_) break;

        hud_.begin_frame();
        hud_.draw(stats_, profiler_, device_.caps(), swapchain_);

        if (!swapchain_.begin_frame()) {
            // The swapchain is being rebuilt; close the ImGui frame we already opened so
            // the next NewFrame is balanced.
            ImGui::EndFrame();
            continue;
        }

        record_frame(static_cast<f32>(ns_to_ms(frame_start - start_ns) * 0.001));
        swapchain_.end_frame();

        // Wait for the next slot HERE, rather than inside the next begin_frame.
        //
        // Both are the same wait for the same length of time and neither changes the frame rate.
        // What changes is where the input is read relative to it. Waiting inside begin_frame means
        // the loop reads the mouse, spends up to a whole frame blocked on the card, and only then
        // draws from a camera that is by then a frame old. Waiting here means the block is over
        // before the mouse is read, so the image is drawn from where the player is looking rather
        // than from where they were looking.
        //
        // It is invisible in every measurement in this file ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the same work happens in the same
        // order on the GPU and the frame time is identical ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and it is the difference between a
        // frame rate and how a frame rate feels.
        swapchain_.wait_for_slot();

        // Deliberate fault at the same moment a scripted screenshot would be taken, so the
        // report it produces is a real in-game one: camera, device and all.
        if (options_.crash_test == "frame" && frame_counter_ >= options_.screenshot_frame) {
            volatile int* target = nullptr;
            *target = 1;
        }

        // Where the measurement window starts counting from.
        //
        // Without --settle that is frame nought, which is what every figure in this repository was
        // taken with and is why two runs of one binary could not be compared: the scene sharpens
        // region by region in the background, so frame 300 catches whatever happened to be built by
        // then, and a build that renders faster gets there sooner with LESS world in front of it.
        // Measured on the `close` camera: 52,292 pixels between two runs of the same executable.
        //
        // With it, the window starts when refinement has nothing left it can do from this camera.
        // That is a fixed point rather than a stopwatch reading, so two runs measure one scene.
        // And it has to HOLD, not merely happen once.
        //
        // "Nothing selectable" is transient. pump_refinement marks a box done and calls
        // start_refinement BEFORE pasting it, so the pick that decides whether anything is left is
        // made against the world as it was before the box landed - and a box that lands can uncover
        // regions the occlusion test was rejecting. Latching on the first quiet frame therefore
        // started the window in the middle of the build: two runs measured 82,718 and 95,638 nodes
        // and disagreed on 65,316 pixels, and a longer window made it worse rather than better,
        // which is the signature of a world still changing rather than a picture still converging.
        // The clock, read once and used by everything below that has to be able to give up.
        //
        // Before the settle block rather than after it, because a wait measured in FRAMES is not a
        // wait at all when the frames are what got slow: kSettleGiveUp is thirty thousand of them,
        // which at the one frame a second a bad build runs at is eight hours of a run that was
        // asked to take three minutes.
        const bool out_of_time =
            options_.max_seconds > 0.0 &&
            ns_to_ms(now_ns() - load_began_ns_) > options_.max_seconds * 1000.0;

        if (options_.settle && !settled_seen_) {
            if (refine_settled_ && !refine_running_) {
                ++settle_streak_;
            } else {
                settle_streak_ = 0;
            }
            // And it cannot wait for ever.
            //
            // Settling means "refinement has nothing left it can do from here", and an EDIT can
            // give it something to do again -- carving a wall exposes regions the occlusion test
            // was rejecting. So a run that edits can reset the streak repeatedly and never reach
            // its screenshot frame at all, which is not a slow measurement, it is a measurement
            // that never returns. Two of them ran until they were killed and wrote nothing.
            if (!settled_seen_ && (frame_counter_ > kSettleGiveUp || out_of_time)) {
                settled_seen_ = true;
                settle_frame_ = frame_counter_;
                WS_LOG_WARN("frame",
                            "gave up waiting for the world to settle after {} frames and {:.0f} s; "
                            "measuring from here anyway, and this figure is not comparable with a "
                            "settled one",
                            frame_counter_, ns_to_ms(now_ns() - load_began_ns_) / 1000.0);
            }
            if (settle_streak_ >= kSettleFrames) {
                settled_seen_ = true;
                settle_frame_ = frame_counter_;
                WS_LOG_INFO("frame", "world settled at frame {}; measuring from here",
                            settle_frame_);
            }
        }
        const bool measuring = !options_.settle || settled_seen_;
        const u64 measured = measuring ? frame_counter_ - settle_frame_ : 0;

        // Warm-up thrown away before anything is averaged. Shaders are still compiling and
        // the first nodes are still arriving for the opening frames, and timing those measures
        // the loading screen rather than the renderer ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the same reasoning the first-run
        // benchmark in documentation/19-auto-quality.md already uses.
        if (!options_.screenshot.empty() && measuring && measured == options_.screenshot_frame / 2) {
            profiler_.reset_averages();
        }

        // A deadline in seconds, because a deadline in FRAMES is no deadline at all.
        //
        // Every scripted run ends by counting frames, which works exactly until the change being
        // measured is the one that made the frame slow Ã¢â‚¬â€ and then the run that would have told you
        // so never finishes. This session did that four times, and each time the person whose
        // machine it was closed it by hand before the measurement it was producing arrived. A
        // change that makes the renderer ten times slower has to be *reportable*, and it is the
        // slow ones that most need reporting.
        //
        // The shot is still taken, so a slow build is diagnosed from a picture and a log rather
        // than from nothing at all, and the log says plainly that the frame target was not met.
        // `out_of_time` is read above the settle block, which needs it too.
        if (!options_.screenshot.empty() && measuring && out_of_time &&
            measured < options_.screenshot_frame) {
            WS_LOG_WARN("app",
                        "deadline: {:.0f} s elapsed at frame {} of {} Ã¢â‚¬â€ the build is too slow to "
                        "reach the frame it was asked for, which is itself the result. Raise it "
                        "with --max-seconds N, or --max-seconds 0 for none",
                        options_.max_seconds, measured, options_.screenshot_frame);
        }

        if (!options_.screenshot.empty() && measuring &&
            (measured >= options_.screenshot_frame || out_of_time)) {
            device_.wait_idle();
            // The shell's surface when a window is open, the render target when none is. A
            // screenshot of the world with a panel over it has to be the picture that was
            // presented, or the one screen a scripted run can photograph is the one without the
            // interface on it — which is how an interface stops being checked.
            save_image_png(device_,
                           (shell_drawn_ && shell_pass_.valid()) ? shell_pass_.surface()
                                                                 : render_target_,
                           options_.screenshot);

            // A figure taken while the world is still being built is not comparable to anything,
            // and nothing said so.
            //
            // The scene is sharpened region by region over the opening frames and the result is
            // cached ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but the cache is only written when the LAST region lands, and a scripted
            // measurement exits at its screenshot frame long before that. So every run rebuilt the
            // world from scratch, and how much of it existed at frame 300 depended on how fast the
            // frames ran. That inverts the thing a measurement is for: a build that renders faster
            // reaches frame 300 sooner, has *less* world in front of it, and flatters itself.
            // Measured: two runs of one binary on the `close` camera differed on 52,292 pixels,
            // which is more than the change being tested moved them.
            //
            // Left as a warning rather than an implicit wait, because --settle is the wait and
            // some runs genuinely want the cold case.
            usize unrefined = 0;
            for (const RefineRegion& box : refine_regions_) {
                if (!box.done) ++unrefined;
            }

            // What scene the figure above was taken against, which nothing has ever recorded.
            //
            // Every performance number in this repository is a time without a scene attached, and
            // the scene is not fixed: it sharpens region by region in the background and a run is
            // measured wherever it had got to. Two figures are comparable only if these three
            // numbers match, so they are printed beside the timings rather than left to be
            // inferred from how long somebody waited.
            // The content hash is the scene's identity rather than a description of it. Chunk and
            // voxel counts are a proxy ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â two different worlds can share both ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and the question
            // "are these two runs looking at the same thing" has to be answerable exactly, not
            // plausibly. It skips empty chunks, so a world that has been compacted and one that
            // has not agree, which is the difference between a world built here and the same world
            // read back from the clip cache.
            const WorldStats measured_world = world_.stats();
            // "0 of 0" would be the literal truth for a world with no ladder behind it and would
            // read as "nothing has been sharpened", which is the opposite of what it means.
            const std::string sharpness =
                refine_regions_.empty()
                    ? std::string("no ladder, the world is at the detail the clip asked for")
                    : std::to_string(refine_regions_.size() - unrefined) + " of " +
                          std::to_string(refine_regions_.size()) + " regions sharpened";
            WS_LOG_INFO("frame", "scene: {} chunks, {} solid voxels, {}, content {:016x}",
                        measured_world.chunks, measured_world.solid_voxels, sharpness,
                        world_.content_hash());
            if (unrefined > 0 && !options_.settle) {
                WS_LOG_WARN("frame",
                            "the world was still being sharpened - {} regions left. This figure is "
                            "NOT comparable with another run; take it with --settle",
                            unrefined);
            } else if (unrefined > 0) {
                // Settled, and still short. Expected: a region behind a wall is skipped by the
                // occlusion test in start_refinement and stays coarse while the camera stands
                // here. Said out loud so the number is not read as a failure.
                WS_LOG_INFO("frame",
                            "settled with {} regions this camera cannot see; they stay coarse",
                            unrefined);
            }
            // Averages, not the last frame. One frame's GPU time moves several per cent from
            // clock and scheduling alone, so a figure another build has to beat has to be a
            // mean over a window. `worst` is beside it because a locked frame rate is decided
            // by the worst frame and not by the mean one (documentation/09 Ãƒâ€šÃ‚Â§9).
            const std::vector<PassAverage>& passes = profiler_.averages();
            WS_LOG_INFO("frame", "{:<14} {:>9} {:>9} {:>9}", "pass", "mean ms", "worst", "budget");
            for (const PassAverage& pass : passes) {
                // Indented by nesting, so a pass that contains another reads as containing it
                // rather than as a sibling that happens to cost the same.
                const std::string label = std::string(pass.depth * 2, ' ') + pass.name;
                WS_LOG_INFO("frame", "{:<14} {:9.3f} {:9.3f} {:9.2f}", label, pass.mean_ms,
                            pass.worst_ms, pass.budget_ms);
            }
            WS_LOG_INFO("frame", "total GPU mean {:.3f} ms, worst {:.3f} ms, over {} frames",
                        profiler_.mean_total_gpu_ms(), profiler_.worst_total_gpu_ms(),
                        profiler_.averaged_frames());
            WS_LOG_INFO("frame", "CPU {:.3f} ms", stats_.average_ms());
            WS_LOG_INFO("frame", "CPU node pool {:.3f} ms, worst {:.3f} on frame {}",
                        node_ms_, worst_node_ms_, worst_node_frame_);
            WS_LOG_INFO("frame", "CPU last frame: feedback {:.3f} ms, uploads {:.3f}, report {:.3f}",
                        stream_ms_, uploads_ms_, report_ms_);
            WS_LOG_INFO("frame",
                        "feedback {} reports ({} dropped, {} for places the world is empty at)",
                        last_feedback_, last_feedback_truncated_, last_feedback_phantom_);
            // In bytes as well as in chunks, because the claim the rewrite makes about streaming
            // is about memory following the *screen* ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and a chunk count cannot show that. Two
            // views holding the same number of chunks at different resolutions should differ
            // here, and that difference is the thing being aimed at.
            // What the card actually holds, against what the pool holds. Run at the screenshot
            // rather than per frame because it stalls the device; that is often enough to catch
            // an upload that is dropping or misplacing something, which is the class of fault
            // this exists for.
            {
                node_buffers_.audit(node_pool_);
                // And what the POOL holds against what the world holds, which is the half of that
                // standard nothing was checking. `node_buffers_.audit` asks whether the card agrees
                // with the pool; both can agree perfectly about a brick neither has looked at since
                // the world rewrote it. That is not hypothetical: it is what the clip ladder did on
                // every load, silently, for the life of the ladder -- and the picture it produced
                // was of a building that had sharpened everywhere except in the tree the renderer
                // walks. D397.
                NodeKey first_stale{};
                const u32 stale = node_pool_.stale_leaves(world_, &first_stale);
                if (stale > 0) {
                    WS_LOG_WARN("frame",
                                "the node pool holds {} leaves the world no longer has that shape "
                                "for; the first is the brick at ({}, {}, {})",
                                stale, first_stale.x << kLeafLevel, first_stale.y << kLeafLevel,
                                first_stale.z << kLeafLevel);
                } else {
                    WS_LOG_INFO("frame", "the node pool agrees with the world, leaf for leaf");
                }
                // And the field a leaf audit cannot see. A child mask decides where a ray is
                // allowed to look, so a wrong one is either a phantom request for ever (D133) or
                // geometry nothing will ever ask for -- and it is invisible to every other check
                // here, because the mirror compares the pool against the card and both agree
                // perfectly about a bit that is wrong in each. See D515.
                NodeKey first_mask{};
                const u32 masks = node_pool_.stale_masks(world_, &first_mask);
                if (masks > 0) {
                    WS_LOG_WARN("frame",
                                "the node pool holds {} child masks the world disagrees with; the "
                                "first is the level {} node at ({}, {}, {})",
                                masks, first_mask.level, first_mask.x << first_mask.level,
                                first_mask.y << first_mask.level, first_mask.z << first_mask.level);
                } else {
                    WS_LOG_INFO("frame", "the node pool agrees with the world, mask for mask");
                }
                // The live gate, not the default, so the audit describes the run that was made.
                face_buffers_.audit(face_store_, face_seen_.buffer(),
                                    static_cast<u32>(frame_counter_), options_.face_gate,
                                    light_probe_.buffer(), face_material_.buffer());
                const FaceStoreStats face_stats = face_store_.stats();
                WS_LOG_INFO("frame",
                            "faces: {} live of {}, {} seen this frame, {} claims {} already there, "
                            "{} evicted, {} REFUSED, cold window {} frames (floor {}), "
                            "{} read-reports this frame, sun stride {}, off-screen stride {}, "
                            "{} bytes of faces ({} with the table)",
                            face_stats.faces, face_budget_max_, last_faces_seen_,
                            face_stats.claims, face_stats.hits, face_stats.evictions,
                            face_stats.refusals, face_stats.cold_window, face_store_.min_cold(),
                            last_faces_read_reported_,
                            // What the sun's ray budget is being divided by, which is the one number
                            // the cost of this pass is most directly a function of and the one
                            // nothing printed. Two builds whose face pass differs by a factor of two
                            // with the same store and the same picture differ HERE, and there was no
                            // way to see it. Trap 17: the pass table gives one number to a pass whose
                            // cost is a rate, and a rate is not visible in a total.
                            make_node_push(face_stats.faces).face_stride,
                            // ...and the same number for the other class, beside it, because a
                            // budget that is only visible in one of two classes is the half of the
                            // picture that made D527 readable and D557 not. Nought means the class
                            // casts nothing, which is `--no-secondary-light` and was the only state
                            // this renderer had until R9b's share was spent.
                            secondary_light_stride(),
                            face_stats.face_bytes, face_stats.total_bytes);

                // What SIZE the faces are, which is the size of the smallest shadow the frame can
                // cast. The plan's arithmetic assumes level 0 near the camera -- a voxel covers a
                // whole pixel at 22.5 m at 1440 lines, so everything nearer gains nothing from
                // more pixels (Ãƒâ€šÃ‚Â§6). A store with no level 0 in it is not shading voxel faces
                // whatever the plan says, and the picture shows it as blocky shadows on flat
                // stone. Counted here rather than deduced from a screenshot.
                u32 by_level[16]{};
                for (u32 slot = 0; slot < face_store_.watermark(); ++slot) {
                    const GpuFace& face = face_store_.faces()[slot];
                    if (!face_live(face)) continue;
                    const u32 level = face_level(face);
                    if (level < 16) ++by_level[level];
                }
                std::string face_levels;
                for (u32 level = 0; level < 16; ++level) {
                    if (by_level[level] == 0) continue;
                    if (!face_levels.empty()) face_levels += "  ";
                    face_levels += std::to_string(level) + ":" + std::to_string(by_level[level]);
                }
                WS_LOG_INFO("frame", "faces by level  {}", face_levels);
                // R9a and R9b from the host's side: what the light rays asked for, what the cap
                // allowed, and what it turned away. Offered against claimed is the cost of the rule
                // -- the difference is repeat reports of faces already here, one probe each -- and
                // DECLINED is the cap doing its job rather than a fault. A decline is not a refusal
                // and the two are printed apart on purpose: a refusal is a visible surface with no
                // light of its own (D502), a decline is one gathering ray reading a coarse stand-in.
                //
                // The cap is the table's SPARE room now rather than a fixed quarter, so it moves
                // frame to frame with the size of the on-screen set, and the window this class is
                // given up on is printed beside it for the same reason: the two together are the
                // whole policy, and either alone cannot tell a class being held back from a class
                // being spent.
                WS_LOG_INFO("frame",
                            "the off-screen set: {} of a cap of {} slots (cold at {} frames against "
                            "{} for the rest), {} offered by light rays over the run, {} of those "
                            "new, {} declined by the cap, {} promoted when a pixel read them",
                            face_stats.secondary, face_stats.secondary_cap,
                            face_stats.secondary_window, face_stats.cold_window,
                            faces_secondary_offered_, faces_secondary_claimed_,
                            face_stats.secondary_declined, face_stats.promotions);
                // What finding the lamps costs, which nothing has ever printed. The list is
                // rebuilt by walking every brick of every chunk, and it is rebuilt on every
                // announced change to the world -- every chisel stroke and every region the clip
                // ladder pastes. Printed beside the count so the two are read together: twenty-one
                // emitters found by a scan of the whole building is the shape R9g exists to fix.
                // Read the two halves together. A time alone cannot tell a rebuild that reused
                // everything from one that had nothing to reuse, and the second is what the cost
                // used to be on EVERY announcement -- trap 20, in the one pass where the work and
                // the cost are supposed to have come apart.
                WS_LOG_INFO("light",
                            "the emitter scan: {} rebuilds over the run, {:.2f} ms each on average, "
                            "worst {:.2f} ms; the last one rescanned {} chunks and reused {}",
                            light_builds_,
                            light_builds_ > 0
                                ? ns_to_ms(light_build_ns_) / static_cast<f64>(light_builds_)
                                : 0.0,
                            ns_to_ms(light_build_worst_ns_), last_emitter_scans_,
                            last_emitter_reused_);
                // R9f from the host's side, and the two numbers have to be read as a pair. A live
                // count alone says what the rule costs and nothing about whether it is working; an
                // eviction count alone cannot tell "the rule is holding them" from "there were none
                // to hold". On a settled camera with the rule on, the second is nought and the first
                // is not -- and with `--no-coarse-keep` the second is most of the first.
                WS_LOG_INFO("frame",
                            "the coarse pyramid: {} stand-ins live of {} faces ({:.1f}%), {} given "
                            "up over the run; the store {} keeping them past their cold window",
                            face_stats.stand_ins, face_stats.faces,
                            face_stats.faces > 0
                                ? 100.0 * face_stats.stand_ins / static_cast<f64>(face_stats.faces)
                                : 0.0,
                            face_stats.stand_in_evictions,
                            options_.coarse_keep ? "IS" : "is NOT");
                // R6a's light meter, said out loud. The whole reason this stage exists is that the
                // exposure was a constant of 3.2 with no writer, and nothing in a picture can tell
                // a wrong constant from a right one -- so the number it settled on and the frame's
                // own log-average are printed rather than inferred. Read the two together: the
                // average is what the scene IS and the multiplier is what was done about it.
                if (frame_stats_readback_.mapped != nullptr) {
                    const FrameStatistics& done = static_cast<const FrameStatistics*>(
                        frame_stats_readback_.mapped)[1];
                    if (!options_.auto_exposure) {
                        WS_LOG_INFO("frame",
                                    "the light meter: OFF (--no-auto-exposure), a fixed 3.200x");
                    } else if (done.groups == 0) {
                        WS_LOG_INFO("frame", "the light meter: nothing measured yet");
                    } else {
                        const f64 stops =
                            static_cast<f64>(done.log_luminance) / done.groups /
                                kLogLuminanceUnit - kLogLuminanceBias;
                        WS_LOG_INFO("frame",
                                    "the light meter: {:.3f}x, on a frame whose log-average is "
                                    "{:.2f} stops over {} workgroups",
                                    static_cast<f64>(done.exposure) / kExposureUnit, stops,
                                    done.groups);
                    }
                }
                // And what the scripted chisel did, if it was asked for. A run that changed no
                // voxels is a run that measured the flight and not the edit, and the two figures
                // look identical from the pass table alone.
                if (options_.chisel_every > 0) {
                    WS_LOG_INFO("frame",
                                "chisel: {} edits fired, {} voxels changed, {} missed for want of "
                                "anything to aim at, one every {} frames at radius {}",
                                chisels_fired_, chisel_voxels_, chisels_missed_,
                                options_.chisel_every, options_.chisel_radius);
                    const f64 fired = static_cast<f64>(std::max<u64>(chisels_fired_, 1));
                    WS_LOG_INFO("frame",
                                "chisel CPU per edit: apply and undo {:.2f} ms, world bounds "
                                "{:.2f} ms, invalidation downstream {:.2f} ms",
                                ns_to_ms(chisel_apply_ns_) / fired,
                                ns_to_ms(chisel_bounds_ns_) / fired,
                                ns_to_ms(chisel_invalidate_ns_) / fired);
                }

                // A full table is a fact about the table and never about the world, and it has to
                // be said out loud. Faces became voxels rather than bricks, which multiplied the
                // count by about twenty-five: 639,233 at 4K on the close camera against a budget
                // of 1,048,576. A larger scene reaches the cap, and a cap nobody reports looks
                // exactly like geometry that will not take a shadow.
                if (face_store_.out_of_room()) {
                    WS_LOG_WARN("faces", "face store is FULL at {} faces -- surfaces past this "
                                         "point get no light of their own",
                                face_stats.faces);
                }
            }

            const NodePoolStats node_stats = node_pool_.stats();
            WS_LOG_INFO("frame",
                        "node pool: {} nodes, {} leaves, {} bytes ({} for the screen); "
                        "built {} evicted {}; "
                        "requests {} hits {} deferred {}",
                        node_stats.nodes,
                        node_stats.leaves, node_stats.total_bytes, node_stats.screen_bytes,
                        last_node_built_,
                        last_node_evicted_, node_stats.requests, node_stats.hits,
                        last_node_deferred_);
            // What eviction is actually throwing away.
            //
            // `evicted` alone cannot tell a pool shedding what nobody is looking at -- which is
            // the whole of R2 working correctly -- from a pool dropping the wall in front of you
            // and rebuilding it three frames later, which is the flicker D421 reported and D425
            // left open. Two independent numbers say which: how many of the evicted nodes were
            // inside the frustum when they went, and how many came straight back. The second is
            // the harm and needs no theory about why the pool thought they were cold; the first
            // is also the size of the set a "do not evict what the camera can see" rule would
            // have to hold, so it prices that fix at the same time. D426.
            WS_LOG_INFO("frame",
                        "node eviction: {} lifetime, {} of them inside the view; {} came back "
                        "within {} frames. This frame: {} nodes given up, {} in view, {} back",
                        node_stats.evictions, node_stats.evictions_on_screen, node_stats.churn,
                        kChurnWindow, last_node_evicted_nodes_, last_node_evicted_on_screen_,
                        last_node_churned_);
            if (node_stats.churn > 0) {
                std::string churn_levels;
                for (u32 level = 0; level < 32; ++level) {
                    if (node_stats.churn_per_level[level] == 0) continue;
                    if (!churn_levels.empty()) churn_levels += "  ";
                    churn_levels += std::to_string(level) + ":" +
                                    std::to_string(node_stats.churn_per_level[level]);
                }
                WS_LOG_INFO("frame", "evicted and wanted again, by level  {}", churn_levels);
                // And how full those bricks are, against every brick the pool holds. A brick a
                // ray STOPS on is a wall; a brick it passes THROUGH on the way to one is mostly
                // air, and only the first kind is ever stamped as read.
                WS_LOG_INFO("frame",
                            "brick fill of 512: {:.1f} came back, {:.1f} evicted, {:.1f} resident",
                            node_stats.churn_fill, node_stats.evicted_fill,
                            node_stats.resident_fill);
                // The sharper of the two discriminators. A node that was reported read and then
                // went quiet is a sampling problem and wants a denser or longer window; a node no
                // ray EVER reported is a reporting problem and wants the marcher to say more.
                WS_LOG_INFO("frame",
                            "no ray ever reported reading: {} of {} evicted, {} of {} that came "
                            "back",
                            node_stats.evictions_never_read, node_stats.evictions,
                            node_stats.churn_never_read, node_stats.churn);
                WS_LOG_INFO("frame",
                            "what asked for them back: {} a primary ray's miss, {} a light ray "
                            "stopped by ignorance, {} a dilated neighbour, {} the proximity radius",
                            node_stats.churn_by_source[kRequestRay],
                            node_stats.churn_by_source[kRequestOcclusion],
                            node_stats.churn_by_source[kRequestDilated],
                            node_stats.churn_by_source[kRequestProximity]);
            }

            // Resident nodes by level. A total says memory fell and cannot say which levels
            // failed to move, and R2b's whole rule is that halving the resolution shifts every
            // ray's stopping point exactly one level coarser.
            {
                std::string levels;
                for (u32 level = kLeafLevel; level <= kEntryLevel; ++level) {
                    if (node_stats.per_level[level] == 0) continue;
                    if (!levels.empty()) levels += "  ";
                    levels += std::to_string(level) + ":" +
                              std::to_string(node_stats.per_level[level]);
                }
                WS_LOG_INFO("frame", "nodes by level  {}", levels);
            }
            WS_LOG_INFO("frame", "resident bytes {} payload, {} nodes, {} total, {} the "
                                 "screen pays for",
                        node_stats.payload_in_use, node_stats.node_bytes, node_stats.total_bytes,
                        node_stats.screen_bytes);
            break;
        }
    }

    // Where it settled, so the next run starts there rather than climbing the ladder again
    // from whatever the benchmark guessed. Written on the way out rather than on every change:
    // a controller that touches the disk each time it moves would write during exactly the
    // stutter that made it move.
    if (options_.screenshot.empty()) save_settings();

    device_.wait_idle();
    hud_.destroy();
    type_tables_.destroy();
    feedback_.destroy();
    destroy_buffer(device_, params_buffer_);
    destroy_buffer(device_, ballast_);
    destroy_buffer(device_, clip_buffer_);
    destroy_buffer(device_, frame_stats_);
    destroy_buffer(device_, frame_stats_readback_);
    destroy_buffer(device_, clip_staging_);
    destroy_buffer(device_, light_buffer_);
    visibility_.destroy();
    // The face pass and its store, which were added without being added here.
    //
    // The cost of the omission is exactly what the note below predicts: validation reports three
    // buffers still alive at vkDestroyDevice, and the pipeline's own destructor then runs against
    // a device that no longer exists and takes an access violation inside the driver, with this
    // file nowhere in the stack. It reads as a driver bug and is a missing line.
    shade_faces_.destroy();
    face_buffers_.destroy();
    face_light_.destroy();
    face_seen_.destroy();
    face_gathered_.destroy();
    node_seen_.destroy();
    face_read_.destroy();
    face_material_.destroy();
    face_lobe_.destroy();
    face_medium_.destroy();
    light_probe_.destroy();
    face_worklist_.destroy();
    destroy_buffer(device_, face_work_);
    node_buffers_.destroy();
    if (node_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), node_layout_, nullptr);
        node_layout_ = VK_NULL_HANDLE;
    }
    resolve_.destroy();
    // And the cloud pass. Every pipeline has to be torn down HERE, while the device is still
    // alive: a ComputePipeline left to its own destructor runs after ~Application has taken the
    // device with it, and destroying a pipeline against a dead device is an access violation in
    // the driver with this file nowhere in the stack.
    if (refine_thread_.joinable()) refine_thread_.join();
    clouds_.destroy();
    destroy_render_target();
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_.handle(), descriptor_pool_, nullptr);
    }
    if (resolve_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), resolve_layout_, nullptr);
    }
    if (cloud_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), cloud_layout_, nullptr);
    }
    // The device, the swapchain, the profiler, the window and the interface are NOT torn down
    // here: they outlive a world, which is the whole point of the split. What is torn down is
    // every pool and buffer above — and the rest of this object goes with its destructor, so the
    // next world starts from nothing rather than from whatever this one left behind.
    loading_screen_.destroy();

    // And everything this world left ON something that outlives it. There are three, they are all
    // one line, and every one of them is a thing a title screen would otherwise inherit from a
    // world that no longer exists:
    //
    //   the captured mouse — a title you cannot point at
    //   the crash context — a report naming a camera in a world that has been torn down
    //   composition — an input-method window over a menu nobody is typing into
    //
    // The fourth was the HUD's event hook, and that one was not harmless: see Hud::destroy.
    if (mouse_look_) {
        mouse_look_ = false;
        window_.set_relative_mouse(false);
    }
    window_.set_text_input(false);
    crash_set_context("camera", "no world");
    crash_set_context("state", "at the title");
    return 0;
}

// The process: a window, a card, an interface, and however many worlds get opened in it.
//
// **The game opens on a title, not in a world** (D441). Everything expensive is behind that: the
// build, the pools, the pipelines and the residency belong to an Application, and there is no
// Application until somebody has asked for a world. That is what makes
// `09-performance-budgets.md`'s *cold start to main menu ≤3 s* and *enter a world ≤5 s* two
// numbers about two different events rather than one number said twice.
//
// And **every scripted run walks straight past it** (`23-shell-and-libraries.md` §0). Every
// measurement in this project is taken by one of those flags, and a menu a harness has to click
// through would end measurement here.
int run_windowed(const Options& options) {
    // Where *cold start to main menu* is measured from. `09-performance-budgets.md` §8 asks for
    // three seconds, and that number only means anything now that there is a menu between the
    // launch and the world (D441) — so it is reported, every run, rather than assumed.
    const u64 launched_ns = now_ns();

    Window window;
    Device device;
    Swapchain swapchain;
    GpuProfiler profiler;

    const std::string title = std::string("WorldShaper v") + kVersion;
    if (!window.create(title, options.width, options.height, options.size_explicit)) return 1;
    if (!device.create(&window, options.validation)) return 1;
    {
        // Which card and which driver, in every crash report from here on. A fault that only
        // happens on one machine is answerable; a fault on "a PC" is not.
        const DeviceCapabilities& caps = device.caps();
        crash_set_context(
            "gpu", std::format("{} (vendor 0x{:04X}, driver {}.{}.{}, {} MB)", caps.name,
                               caps.vendor_id, VK_API_VERSION_MAJOR(caps.driver_version),
                               VK_API_VERSION_MINOR(caps.driver_version),
                               VK_API_VERSION_PATCH(caps.driver_version),
                               caps.device_local_bytes >> 20));
    }
    if (!swapchain.create(device, window.width(), window.height(), options.vsync)) return 1;
    if (!profiler.create(device)) return 1;

    const std::filesystem::path spirv = compiled_shader_dir();
    const std::filesystem::path sources(WS_SHADER_SOURCE_DIR);

    ShellPass shell_pass;
    ui::Shell shell;
    Audio audio;
    const bool shell_up = shell_pass.create(device, sources, spirv);
    if (!shell_up) {
        // Not fatal, and deliberately so: a game that will not start because its menu would not
        // compile has traded the thing for the report on the thing. Without it the title cannot be
        // drawn, so the run opens a world directly, exactly as it did before Stage 15.
        WS_LOG_ERROR("shell", "no interface this run; opening a world directly");
    }
    shell.load(ui::default_root(), std::filesystem::path(Window::base_path()));
    // A pinned mark, for a photograph that has to be comparable with the last one. Before the first
    // frame, so the seed the shell would otherwise choose from the clock is never chosen at all.
    if (options.logo_seed != 0) shell.pin_logo(options.logo_seed);
    // The scenes that travel with the executable, on the shelf, once. See Shell::seed_worlds for
    // why a library over a real folder cannot simply start empty.
    shell.seed_worlds(std::filesystem::path(Window::base_path()) / "clips");
    // The synthesis is in ws_ui and the device is in ws_platform, and neither knows the other
    // exists. No audio device costs silence and one line, never a start-up error.
    audio.create(shell.ui().sound());
    shell.ui().sound().configure(audio.valid() ? audio.sample_rate() : 48000u);

    // Whether a script would parse, asked on every keystroke by the editor tab. The tables are
    // this function's rather than the shell's, because ws_ui does not know what a clip is — and a
    // scratch table here cannot contaminate the one a world is using.
    TagRegistry editor_tags;
    shell.set_parser([&](const std::string& text) {
        ui::ParseReport report;
        // A parse that interns materials into a table on every keystroke is a table that grows
        // without bound over an afternoon of typing, so a fresh one is made each time. It is a few
        // microseconds against a keystroke, and it cannot contaminate the table a world is using.
        VoxelTypeTable scratch;
        const forge::Script script = forge::parse_clip_script(text, scratch, editor_tags);
        if (!script.errors.empty()) {
            report.ok = false;
            report.line = script.errors.front().line;
            report.message = script.errors.front().message;
        }
        return report;
    });

    {
        const f64 cold_ms = ns_to_ms(now_ns() - launched_ns);
        WS_LOG_INFO("shell", "title ready in {:.0f} ms{}", cold_ms,
                    (cold_ms > 3000.0) ? "  -- over the 3 s budget in documentation/09 section 8"
                                       : "");
    }

    const u64 began_ns = now_ns();
    int result = 0;

    // `round` is carried across iterations rather than copied fresh, because leaving a world has
    // to clear the world that was opened: a `--world` run that came back to the title and then
    // reopened the same world on the next turn round would never stop.
    Options round = options;
    const bool scripted = options.scripted();
    bool done = false;
    // Set when a scripted `--cycle` run has come back from its world: the title is shown once
    // more, for the frames it was given, and then the run ends.
    bool stop_after_title = false;

    while (!done) {
        // The title runs whenever there is no world already chosen. A scripted run walks past it —
        // except the two that are ABOUT it: one that photographs it, and one that came back to it.
        const bool title_now = shell_up && round.world.empty() &&
                               (!scripted || !round.title_shot.empty() || stop_after_title);
        if (title_now) {
            // ---- the title ---------------------------------------------------------------
            //
            // Its own loop, because there is nothing else to record: no world, no pipelines and
            // no render target. One compute pass draws the room, one draws the marks, and the
            // whole of it is up before anything expensive has been created.
            bool leave = false;
            if (!round.shelf.empty() && !shell.open_shelf(round.shelf)) {
                WS_LOG_WARN("shell", "there is no shelf called '{}'", round.shelf);
            }
            if (!round.title_open.empty()) shell.open_window(round.title_open, true);
            shell.show_icons(round.icon_sheet);
            u64 title_frame = 0;
            for (;;) {
                if (!window.pump()) {
                    leave = true;
                    break;
                }
                if (window.minimised()) continue;
                if (window.resized_this_frame() || swapchain.needs_recreate()) {
                    device.wait_idle();
                    swapchain.recreate(window.width(), window.height());
                }
                shell_pass.reload_if_changed();

                // A scripted title steps its clock by a fixed sixtieth, exactly as `--fly` does for
                // the camera, so `--title-frames N` means N/60 seconds and not "however long that
                // took on this machine". Everything on this screen that moves is a function of this
                // number — the mark's animations, its arrangements, the sort in progress, the room's
                // own light — so on the wall clock a photograph of any of them was a photograph of
                // whatever the frame rate happened to be, and two runs could not be compared.
                const f64 seconds = scripted ? static_cast<f64>(title_frame) / 60.0
                                             : ns_to_ms(now_ns() - began_ns) * 0.001;
                shell.set_stage(ui::Stage::Title);
                shell.set_playing({});
                const ui::Verdict verdict =
                    shell.frame(window.input(), window.width(), window.height(), seconds);
                window.set_text_input(shell.ui().wants_text_input());
                shell.save_if_changed();

                const ui::Colour& accent = shell.ui().accent();
                const f32 accent_rgb[3]{accent.r, accent.g, accent.b};
                shell_pass.present(swapchain, shell.ui().draw(), accent_rgb,
                                   static_cast<f32>(seconds));

                // A scripted title runs for a fixed number of frames and then stops, whether it
                // was asked for a photograph or only for the tear-down path to be walked.
                ++title_frame;
                // The one thing that moves a pinned mark, and the only way the morph between two
                // combinations is ever looked at by anything other than a person watching it.
                if (round.logo_change > 0 && title_frame == round.logo_change) shell.change_logo();
                if ((!round.title_shot.empty() || stop_after_title) &&
                    title_frame >= round.title_frames) {
                    if (!round.title_shot.empty()) {
                        // Taken from the shell's own surface rather than from the swapchain,
                        // because a swapchain image cannot be read back on every driver and this
                        // one always can.
                        device.wait_idle();
                        save_image_png(device, shell_pass.surface(), round.title_shot);
                        WS_LOG_INFO("shell", "title photographed to {} after {} frames",
                                    round.title_shot, title_frame);
                    }
                    leave = true;
                    break;
                }

                if (verdict.quit) {
                    leave = true;
                    break;
                }
                if (verdict.open_world) {
                    round.world = verdict.world.string();
                    break;
                }
            }
            if (leave) break;
            // A title that ended without choosing a world has nothing to hand on, which is what
            // the last title of a `--cycle` run does.
            if (round.world.empty()) break;
        }

        // ---- one world -------------------------------------------------------------------
        //
        // On the heap, and not because of the allocation: the object holds the world, the node
        // pool, the face store and the residency tables, and a few hundred kilobytes of it on the
        // stack is a stack overflow on a thread that has done nothing wrong.
        {
            shell.set_playing(round.world.empty()
                                  ? std::string("the test scene")
                                  : std::filesystem::path(round.world).stem().string());
            // A scripted run can ask for a window to be up in the world too, which is the only
            // way the docked interface over a real backdrop gets photographed by anything other
            // than a person with a hand on the keyboard.
            if (!round.title_open.empty()) shell.open_window(round.title_open, true);
            auto application = std::make_unique<Application>(window, device, swapchain, profiler,
                                                             shell_pass, shell);
            result = application->play(round);
            const bool back_to_title = application->wants_title();
            // A world chosen from inside another one. The tear-down below is the same either way;
            // the only difference is what happens next, and this is copied out before the object
            // that holds it goes.
            const std::string next_world = application->wants_world();
            // The world goes here, at the end of this scope, and every pool in it goes with it —
            // which is `02-architecture-overview.md`'s rule made structural rather than remembered.
            application.reset();

            if (result != 0 || !back_to_title) {
                done = true;
            } else if (!next_world.empty()) {
                // Straight into the next one, past the title. The world just left is gone with
                // every pool it owned, so this is the same fresh start opening one from the title
                // is — it simply does not stop to show a screen nobody asked for.
                round.world = next_world;
                round.title_open.clear();
            } else if (scripted && round.cycle == 0) {
                done = true;
            } else {
                // Back to the title, and the world that was open is forgotten — otherwise the
                // next turn round the loop would reopen the very world that was just left.
                round.world.clear();
                // A cycle run shows the title once more, for the frames it was given, and then
                // stops. It has already done the thing it was for.
                if (round.cycle > 0) {
                    round.cycle = 0;
                    round.title_open.clear();
                    stop_after_title = true;
                }
            }
        }
    }

    shell.save();
    audio.destroy();
    shell_pass.destroy();
    profiler.destroy();
    swapchain.destroy();
    device.destroy();
    window.destroy();
    return result;
}

// Break on purpose, each kind through a different path into the handler, so a report that
// never arrives points at which mechanism is missing rather than at the whole system.
int run_crash_test(const std::string& kind) {
    crash_set_context("crash test", kind);
    WS_LOG_INFO("crash", "deliberate crash: {}", kind);
    if (kind == "report") {
        const std::string path = crash_write_report("requested by --crash-test report");
        WS_LOG_INFO("crash", "report written to {}", path);
        return 0;
    }
    if (kind == "check") {
        WS_CHECK(false, "deliberate check failure from --crash-test");
    } else if (kind == "throw") {
        throw std::runtime_error("deliberate exception from --crash-test");
    } else if (kind == "divzero") {
        // Through a volatile so the optimiser cannot fold it away at compile time.
        volatile int zero = 0;
        return 1 / zero;
    } else if (kind == "write") {
        volatile int* target = nullptr;
        *target = 1;
    } else {
        volatile const int* target = nullptr;
        return *target;
    }
    return 0;
}

}  // namespace

// Build a clip from its file and say what it is, without opening a window.
//
// The other half of authoring. A screenshot says a room looks plausible; this says the doorway is
// 1.00 m and not 0.97, that the two halves match to the voxel, and that the material meant for
// the trim is on 0.4% of the surface rather than none of it because its rule never fired. Both
// halves are needed and this one is the half that can be run in a second.
int run_clip_tool(const Options& options) {
    VoxelTypeTable types;
    TagRegistry tags;
    const u64 begin = now_ns();
    forge::Script script = forge::load_clip_script(options.clip_file, types, tags);
    for (const forge::ScriptError& error : script.errors) {
        if (error.line > 0) {
            WS_LOG_ERROR("clip", "line {}: {}", error.line, error.message);
        } else {
            WS_LOG_ERROR("clip", "{}", error.message);
        }
    }
    if (!script.ok()) return 1;

    if (options.clip_metre > 0) script.settings.voxels_per_metre = options.clip_metre;

    // A slice of the box instead of all of it, at full resolution, so the cost of sampling can
    // be measured in seconds. Six numbers, in metres, the same two opposite corners `bounds`
    // takes.
    if (!options.clip_bounds.empty()) {
        i64 corners[6]{0, 0, 0, 0, 0, 0};
        parse_numbers(options.clip_bounds, corners, 6);
        script.settings.low = {static_cast<f64>(corners[0]), static_cast<f64>(corners[1]),
                               static_cast<f64>(corners[2])};
        script.settings.high = {static_cast<f64>(corners[3]), static_cast<f64>(corners[4]),
                                static_cast<f64>(corners[5])};
    }

    // Measuring one named piece rather than the whole building. What a camera should be looking
    // at is almost never the whole clip ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it is a portico, or a room ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and framing needs that
    // piece's extent, not the extent of the site it stands on.
    if (!options.clip_part.empty()) {
        u32 piece = 0;
        if (!script.part(options.clip_part, piece)) {
            WS_LOG_ERROR("clip", "no part called '{}'", options.clip_part);
            std::printf("parts        ");
            for (const auto& entry : script.parts) std::printf(" %s", entry.first.c_str());
            std::printf("\n");
            return 1;
        }
        script.solid = piece;
    }

    JobSystem jobs;
    const u64 parsed = now_ns();
    // Always counted here. This is the measuring tool; a build it measures should be able to say
    // where it went, and one atomic beside an evaluation is nothing next to the evaluation.
    script.settings.count_rule_cost = true;
    forge::SampleResult built =
        forge::sample(script.field, script.solid, script.paint, script.settings, &jobs);
    const u64 sampled = now_ns();

    // The measuring tool despeckles too, or its numbers are about a world nobody plays. D610.
    // `--no-despeckle` is the control arm and leaves every lone voxel where the sampler put it.
    forge::DespeckleReport cleaned;
    if (options.despeckle) cleaned = forge::despeckle(built.clip);

    forge::SampleResult varied = built;
    const forge::VariationReport variety = forge::apply_variation(
        varied.clip, types, script.field, script.variation, script.settings, built, &jobs);

    const forge::Measurement m =
        forge::measure(varied.clip, script.settings.voxels_per_metre);

    std::printf("%s", forge::report(m, &script.material_names).c_str());
    if (variety.voxels > 0) {
        std::printf("variation     %llu distinct records over %llu voxels (%.4f%%), "
                    "largest identical group %llu\n",
                    static_cast<unsigned long long>(variety.distinct_types),
                    static_cast<unsigned long long>(variety.voxels),
                    variety.uniqueness() * 100.0,
                    static_cast<unsigned long long>(variety.largest_group));
    }
    std::printf("origin        (%lld, %lld, %lld) voxels\n",
                static_cast<long long>(built.origin_voxel[0]),
                static_cast<long long>(built.origin_voxel[1]),
                static_cast<long long>(built.origin_voxel[2]));

    // Where the matter actually is, in metres, in the world. Not the sampled box ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the matter.
    // This is the line a camera is aimed from: everything else describes how much there is, and
    // a camera needs to know where it is and how big.
    if (m.extent.any) {
        const f64 per = static_cast<f64>(script.settings.voxels_per_metre);
        const f64 low[3] = {static_cast<f64>(built.origin_voxel[0] + m.extent.low[0]) / per,
                            static_cast<f64>(built.origin_voxel[1] + m.extent.low[1]) / per,
                            static_cast<f64>(built.origin_voxel[2] + m.extent.low[2]) / per};
        const f64 high[3] = {static_cast<f64>(built.origin_voxel[0] + m.extent.high[0] + 1) / per,
                             static_cast<f64>(built.origin_voxel[1] + m.extent.high[1] + 1) / per,
                             static_cast<f64>(built.origin_voxel[2] + m.extent.high[2] + 1) / per};
        std::printf("worldbox      %.4f %.4f %.4f  %.4f %.4f %.4f  m\n", low[0], low[1], low[2],
                    high[0], high[1], high[2]);
    }
    std::printf("parts        ");
    for (const auto& entry : script.parts) std::printf(" %s", entry.first.c_str());
    std::printf("\n");
    std::printf("field         %zu nodes, %zu parameters, %zu hierarchies over %zu leaves\n",
                script.field.size(), script.field.parameter_count(),
                script.field.accelerator_count(), script.field.accelerated_leaves());
    for (usize i = 0; i < script.field.parameter_count(); ++i) {
        std::printf("  %-16s %.4f\n", script.field.parameter_name(i),
                    script.field.parameter_value(i));
    }
    // What the sampler had to allow for, which is the single number that decides how much of the
    // box it can settle in bulk and how much it has to ask about voxel by voxel. Printed here
    // because this is the tool anybody optimising a clip will be running, and without it the cost
    // is a mystery with no handle on it.
    std::printf("slack         %.4f m worst, %.4f m to settle a box, %.4f m for the worst of "
                "%zu parts\n",
                built.slack, built.prune_slack, built.best_part_slack, built.parts);
    std::printf("asked         %llu voxels individually, %llu settled in bulk (%.1f%% settled)\n",
                static_cast<unsigned long long>(built.voxels_asked),
                static_cast<unsigned long long>(built.voxels_settled),
                100.0 * static_cast<f64>(built.voxels_settled) /
                    static_cast<f64>(std::max<u64>(1, built.voxels_asked + built.voxels_settled)));
    std::printf("evals         %llu shape + %llu paint (%zu paint rules, %zu per-voxel, %zu placed)\n",
                static_cast<unsigned long long>(built.shape_evaluations),
                static_cast<unsigned long long>(built.paint_evaluations), built.rules_total,
                built.rules_per_voxel, built.rules_placed);
    std::printf("cost          parse %.1f ms, sample %.1f ms, %llu field evaluations\n",
                ns_to_ms(parsed - begin), ns_to_ms(sampled - parsed),
                static_cast<unsigned long long>(built.evaluations));
    {
        // Core-milliseconds across every worker, which is why they exceed the wall clock. The
        // ratio is the point: it says which half to work on, and it has twice disagreed with what
        // the evaluation counts implied.
        const f64 shape_ms = ns_to_ms(built.shape_ns);
        const f64 paint_ms = ns_to_ms(built.paint_ns);
        const f64 both = std::max(1.0, shape_ms + paint_ms);
        std::printf("where         shape %.0f core-ms (%.0f%%), paint %.0f core-ms (%.0f%%), "
                    "%.2f us per shape eval\n",
                    shape_ms, 100.0 * shape_ms / both, paint_ms, 100.0 * paint_ms / both,
                    1000.0 * shape_ms / static_cast<f64>(std::max<u64>(1, built.shape_evaluations)));
        std::printf("field         %zu nodes, %zu with no box (%.0f%%), %zu hierarchies over "
                    "%zu leaves, %zu wide unions\n",
                    script.field.size(), script.field.unbounded_nodes(),
                    100.0 * static_cast<f64>(script.field.unbounded_nodes()) /
                        static_cast<f64>(std::max<usize>(1, script.field.size())),
                    script.field.accelerator_count(), script.field.accelerated_leaves(),
                    script.field.unaccelerated_unions());
    }

    // WHICH rules the build spent itself on.
    //
    // Every expensive build this project has had turned out to be a handful of rules out of a
    // hundred and thirty-three, and finding out which took a guess, a change and a rebuild each
    // round. The list below is the answer in one run, and it is sorted because the interesting
    // part is always the top of it.
    if (!built.rule_evaluations.empty()) {
        std::vector<usize> order(built.rule_evaluations.size());
        for (usize i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](usize a, usize b) {
            return built.rule_evaluations[a] > built.rule_evaluations[b];
        });
        const u64 all = std::max<u64>(1, built.paint_evaluations);
        std::printf("paint by rule (of %llu paint evaluations)\n",
                    static_cast<unsigned long long>(built.paint_evaluations));
        for (usize n = 0; n < order.size() && n < 12; ++n) {
            const usize i = order[n];
            const u64 count = built.rule_evaluations[i];
            if (count == 0) break;
            const u32 type = script.paint[i].type;
            const char* name = (type < script.material_names.size() &&
                                !script.material_names[type].empty())
                                   ? script.material_names[type].c_str()
                                   : "?";
            // WHY a rule costs what it does, which is always one of two things: it cannot be
            // settled for a block, or it cannot be placed in space. Printed rather than deduced,
            // because deducing it has cost this project several rebuilds each time.
            const f64 metric = script.field.metric_slack(script.paint[i].test);
            const bool boxed = !script.field.bounds_of(script.paint[i].test).infinite();
            const char* wrote = (i < script.paint_source.size())
                                    ? script.paint_source[i].c_str()
                                    : "?";
            std::printf("  %5.1f%%  %12llu  %-14s %-10s %-11s %-7s  %s%s\n",
                        100.0 * static_cast<f64>(count) / static_cast<f64>(all),
                        static_cast<unsigned long long>(count), name,
                        script.paint[i].has_place ? "placed" : "unplaced",
                        (metric < forge::Field::kInfiniteSlack) ? "settleable" : "per-voxel",
                        boxed ? "boxed" : "NO BOX", wrote,
                        script.paint[i].facing_axis < 3 ? " +normal" : "");
        }

        // AND THE RULES THAT NEVER FIRED AT ALL, which the table above can never show, because it
        // is sorted by cost and truncated: a rule that did nothing is at the bottom of a list
        // whose top is the only part anybody reads.
        //
        // This is a defect and not a curiosity. `paint gilt where=rotunda_urns` cost nothing,
        // painted nothing, and left four urns wearing the base coat in a room built around what
        // they would reflect -- silently, because a rule that does not fire produces no error, no
        // warning and no difference anybody can point at without knowing what the urn was meant
        // to be. measure.hpp has claimed since it was written that the histogram "catches a paint
        // rule that never fires". It catches it only if somebody knows the answer already. This
        // says it.
        //
        // Zero is not always wrong -- `--clip-part` builds one fragment, so every rule belonging
        // to the other twenty-one is legitimately idle -- which is why this counts them and names
        // them rather than failing.
        {
            std::vector<usize> idle;
            for (usize i = 0; i < built.rule_evaluations.size(); ++i) {
                if (built.rule_evaluations[i] == 0) idle.push_back(i);
            }
            if (!idle.empty()) {
                std::printf("never fired   %zu of %zu rules painted nothing%s\n", idle.size(),
                            built.rule_evaluations.size(),
                            options.clip_part.empty()
                                ? " -- each one is a coat somebody wrote and nothing wears"
                                : " (one part only, so most of these belong to other fragments)");
                for (usize n = 0; n < idle.size() && n < 24; ++n) {
                    const usize i = idle[n];
                    const u32 type = script.paint[i].type;
                    const char* name = (type < script.material_names.size() &&
                                        !script.material_names[type].empty())
                                           ? script.material_names[type].c_str()
                                           : "?";
                    const char* wrote = (i < script.paint_source.size())
                                            ? script.paint_source[i].c_str()
                                            : "?";
                    std::printf("  idle       %-12s %s\n", name, wrote);
                }
            }
        }
    }

    // Slices, asked for as `axis,at` or `axis,at,step`. More than one may be given.
    for (const std::string& request : options.clip_slices) {
        i64 values[3]{1, -1, 1};
        parse_numbers(request, values, 3);
        const u32 axis = static_cast<u32>(std::clamp<i64>(values[0], 0, 2));
        const i32 size = built.clip.size[axis];
        const i32 at = (values[1] < 0) ? size / 2 : static_cast<i32>(values[1]);
        const i32 step = static_cast<i32>(std::max<i64>(values[2], 1));
        std::printf("\nslice along %c at %d of %d, one character per %d voxels\n",
                    "xyz"[axis], at, size, step);
        std::printf("%s", forge::slice_text(varied.clip, axis, at, step).c_str());
    }

    // The checks that catch what a measurement cannot: matter that is not held up, and surfaces
    // nobody can walk on. Always run, because both were wrong in the facility and neither showed
    // in any number until somebody looked at a picture.
    {
        const forge::Connectivity joined = forge::connectivity(varied.clip);
        std::printf("\nconnected     %llu components, largest %llu voxels",
                    static_cast<unsigned long long>(joined.components),
                    static_cast<unsigned long long>(joined.largest));
        if (joined.floating_voxels > 0) {
            std::printf(", %llu voxels NOT joined to it\n",
                        static_cast<unsigned long long>(joined.floating_voxels));
            for (const forge::Island& island : joined.islands) {
                std::printf("  floating   %8llu voxels at (%d,%d,%d)-(%d,%d,%d)\n",
                            static_cast<unsigned long long>(island.voxels), island.low[0],
                            island.low[1], island.low[2], island.high[0], island.high[1],
                            island.high[2]);
            }
        } else {
            std::printf(", all of it joined\n");
        }

        // A step is 0.18 m and a doorway is 2.0 m, in voxels at this clip's resolution.
        const i32 per_metre = script.settings.voxels_per_metre;
        const i32 max_step = std::max(1, (per_metre * 20) / 100);
        const i32 head_room = std::max(1, per_metre * 2);
        const forge::Walkability walk = forge::walkability(varied.clip, max_step, head_room);
        std::printf("walkable      %.1f%% of %llu standable columns reachable from the lowest; "
                    "worst rise %d voxels (%.2f m) at (%d,%d,%d)\n",
                    walk.reachable_fraction() * 100.0,
                    static_cast<unsigned long long>(walk.surfaces), walk.max_rise,
                    static_cast<f64>(walk.max_rise) / static_cast<f64>(per_metre),
                    walk.max_rise_at[0], walk.max_rise_at[1], walk.max_rise_at[2]);

        // WHAT IT IS MADE OF. The header of measure.hpp has advertised this since the file was
        // written -- "histogram: how much of each material, which catches a paint rule that never
        // fires" -- and it was computed on every run and printed on none of them. It is the first
        // thing anybody wants with `--clip-part`: a part that should be one material and comes
        // back as four has been painted by something that does not belong to it, and until now
        // finding out which meant reading colours off a screenshot.
        //
        // Measured on the BUILT clip and not the varied one, for the same reason the speck audit
        // is: variation mints a record per voxel, so a histogram taken after it is a list of
        // nine hundred thousand materials with one voxel each.
        {
            const forge::Measurement what =
                forge::measure(built.clip, script.settings.voxels_per_metre);
            if (!what.types.empty()) {
                std::printf("made of       %zu materials\n", what.types.size());
                for (const forge::TypeShare& share : what.types) {
                    const char* name = (share.type < script.material_names.size() &&
                                        !script.material_names[share.type].empty())
                                           ? script.material_names[share.type].c_str()
                                           : "?";
                    std::printf("  %-12s #%-4u %10llu  %6.2f%%\n", name,
                                static_cast<unsigned>(share.type),
                                static_cast<unsigned long long>(share.count),
                                share.fraction * 100.0);
                }
            }
        }

        // And single voxels wearing the wrong material. See paint_specks: run on the BUILT clip
        // and not the varied one, because variation gives almost every voxel a record of its own
        // and after it every voxel is alone in its type.
        if (cleaned.repainted > 0 || cleaned.left > 0) {
            std::printf("despeckled    %llu lone voxels repainted, %llu left as a stipple\n",
                        static_cast<unsigned long long>(cleaned.repainted),
                        static_cast<unsigned long long>(cleaned.left));
            for (const forge::TypeShare& share : cleaned.by_type) {
                const char* name = (share.type < script.material_names.size() &&
                                    !script.material_names[share.type].empty())
                                       ? script.material_names[share.type].c_str()
                                       : "?";
                std::printf("  repainted  %-12s #%-4u %8llu\n", name,
                            static_cast<unsigned>(share.type),
                            static_cast<unsigned long long>(share.count));
            }
        }
        const forge::SpeckReport dots = forge::paint_specks(built.clip);
        std::printf("specks        %llu of %llu surface voxels alone in their material (%.3f%%)\n",
                    static_cast<unsigned long long>(dots.specks),
                    static_cast<unsigned long long>(dots.surface),
                    100.0 * static_cast<f64>(dots.specks) /
                        static_cast<f64>(std::max<u64>(1, dots.surface)));
        for (const forge::TypeShare& share : dots.by_type) {
            const char* name = (share.type < script.material_names.size() &&
                                !script.material_names[share.type].empty())
                                   ? script.material_names[share.type].c_str()
                                   : "?";
            // A dither is meant to look like this and an accident is not, and the fraction is
            // what tells them apart -- tens of per cent against a fraction of one. Said on the
            // line rather than left to be worked out, because the whole value of this number is
            // that somebody reads it without having been told what to look for.
            const char* verdict = (share.fraction > 0.05) ? "a stipple, presumably deliberate"
                                                          : "SCATTERED -- check what is 2 cm away";
            // The type id as well as the name. Several ids can carry ONE name — a fragment is
            // allowed to re-declare a material with its own properties — and a report listing
            // "marble" eight times with eight different counts is a report nobody can act on.
            std::printf("  %-12s #%-4u %8llu  %6.2f%% of its own surface   %s\n", name,
                        static_cast<unsigned>(share.type),
                        static_cast<unsigned long long>(share.count), share.fraction * 100.0,
                        verdict);
        }
        for (const forge::Speck& one : dots.examples) {
            const char* name = (one.type < script.material_names.size() &&
                                !script.material_names[one.type].empty())
                                   ? script.material_names[one.type].c_str()
                                   : "?";
            std::printf("  speck      %-12s at (%d,%d,%d)\n", name, one.at[0], one.at[1],
                        one.at[2]);
        }
    }

    // Alignment: which parts nearly line up with each other, and by how much they miss.
    //
    // Architecture is mostly things lining up. A column under a beam, a wall over a wall, a sill
    // level with a sill ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and the failure that matters is never a part in wildly the wrong place,
    // which anyone sees at once. It is the part that is *almost* right: four centimetres proud,
    // a voxel short, a face that was meant to be flush and is not. Those read as sloppiness
    // without anyone being able to say why, and no measurement of a single part can find one,
    // because each part is individually fine.
    //
    // So this measures every named part's box and looks for near misses: faces that differ by
    // less than a hand's width but are not equal. Exact agreement is silence; that is the point.
    if (options.clip_align) {
        struct PartBox {
            std::string name;
            f64 low[3];
            f64 high[3];
            bool any = false;
        };
        std::vector<PartBox> boxes;
        for (const auto& part : script.parts) {
            const forge::Field::Aabb box = script.field.bounds_of(part.second);
            if (box.infinite()) continue;   // nothing bounded to compare
            PartBox entry;
            entry.name = part.first;
            entry.low[0] = box.low.x; entry.low[1] = box.low.y; entry.low[2] = box.low.z;
            entry.high[0] = box.high.x; entry.high[1] = box.high.y; entry.high[2] = box.high.z;
            entry.any = true;
            boxes.push_back(entry);
        }

        // A hand's width. Closer than this and they were meant to meet; further and they are
        // simply different things in different places.
        constexpr f64 kNear = 0.12;
        constexpr f64 kExact = 1e-6;
        u32 found = 0;
        std::printf("\nalignment     %zu parts with a known extent\n", boxes.size());
        for (usize i = 0; i < boxes.size(); ++i) {
            for (usize j = i + 1; j < boxes.size(); ++j) {
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f64 pairs[4][2] = {
                        {boxes[i].low[axis], boxes[j].low[axis]},
                        {boxes[i].high[axis], boxes[j].high[axis]},
                        {boxes[i].low[axis], boxes[j].high[axis]},
                        {boxes[i].high[axis], boxes[j].low[axis]},
                    };
                    const char* what[4] = {"starts", "ends", "start/end", "end/start"};
                    for (u32 k = 0; k < 4; ++k) {
                        const f64 gap = std::abs(pairs[k][0] - pairs[k][1]);
                        if (gap <= kExact || gap > kNear) continue;
                        if (found < 40) {
                            std::printf("  %-12s %-12s %c %s differ by %.3f m\n",
                                        boxes[i].name.c_str(), boxes[j].name.c_str(),
                                        "xyz"[axis], what[k], gap);
                        }
                        ++found;
                    }
                }
            }
        }
        std::printf("  %u near misses%s\n", found,
                    (found > 40) ? " (first 40 shown)" : (found == 0 ? " ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â everything is flush" : ""));
    }

    // Symmetry, when asked. Cheap, and it catches a whole class of mistake nothing else does.
    if (options.clip_symmetry) {
        for (u32 axis = 0; axis < 3; ++axis) {
            const u64 differ = forge::mirror_mismatch(varied.clip, axis);
            std::printf("symmetry %c   %llu cells differ (%.4f%%)\n", "xyz"[axis],
                        static_cast<unsigned long long>(differ),
                        100.0 * static_cast<f64>(differ) /
                            static_cast<f64>(std::max<u64>(varied.clip.cell_count(), 1)));
        }
    }
    return 0;
}

}  // namespace ws

int main(int argc, char** argv) {
    // First statement in the program, before anything exists that could fault. A crash
    // before this point is a crash nobody can report.
    ws::crash_install();

    const ws::Options options = ws::parse_options(argc, argv);
    if (options.help) {
        ws::print_help();
        return 0;
    }
    if (!options.clip_file.empty() && options.screenshot.empty()) {
        return ws::run_clip_tool(options);
    }
    // A modal dialog in an automated run is a hang, so scripted modes get the stderr line
    // and nothing else.
    ws::crash_set_dialog(!options.headless && options.screenshot.empty());

    // "frame" is the one kind that has to happen inside the running game, because what it
    // proves is that a report carries the camera and the device with it.
    if (!options.crash_test.empty() && options.crash_test != "frame") {
        return ws::run_crash_test(options.crash_test);
    }
    // Said out loud, because a run that stops early has to be recognisable as having stopped
    // early rather than as having finished.
    if (options.max_seconds > 0.0) {
        WS_LOG_INFO("app", "deadline {:.0f} s; the run reports where it got to when it expires",
                    options.max_seconds);
    }

    if (options.headless) return ws::run_headless(options);

    return ws::run_windowed(options);
}
