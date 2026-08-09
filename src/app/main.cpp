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
#include "gpu/node_buffers.hpp"
#include "gpu/render_params.hpp"
#include "gpu/profiler.hpp"
#include "gpu/screenshot.hpp"
#include "gpu/shader.hpp"
#include "gpu/swapchain.hpp"
#include "gpu/world_buffers.hpp"
#include "platform/window.hpp"
#include "world/history.hpp"
#include "world/op.hpp"
#include "world/raycast.hpp"
#include "world/face_store.hpp"
#include "world/node_pool.hpp"
#include "world/residency.hpp"
#include "world/serialize.hpp"
#include "world/world_cache.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

// Room on the GPU for the ghosts. A *preview* budget, not a limit on the tool: a clip too
// big to fit here is still selected, transformed and stamped exactly — it is drawn as an
// outline instead of as voxels. Thirty-two megabytes each side of the copy.
inline constexpr u64 kMaxClipPoolCells = 8ull * 1024ull * 1024ull;

struct Options {
    bool headless = false;
    bool stream_audit = false;
    u64 stream_frames = 0;
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
    std::string clip_part;        // build only this let name, for looking at one piece

    // A smaller box to sample, overriding the clip's own.
    //
    // Sampling cost is per voxel and per field evaluation, so a representative slice at FULL
    // resolution measures the thing that actually matters, in seconds rather than minutes.
    // Measuring at a coarser --clip-metre instead changes the very thing under test — how often
    // a box can settle depends on how large a voxel is — so it answers a different question,
    // confidently and wrongly.
    std::string clip_bounds;
    bool stream_log = false;   // per-second residency report, for diagnosing streaming
    // Start the measurement window when the world stops changing, rather than at frame nought.
    // Any figure meant to be compared with another run wants this; see the note at its use.
    bool settle = false;
    bool path_trace = false;   // start in the reference path tracer
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
    bool node_pool = true;
    u32 hollow = 0;            // shell thickness for the scripted edit, and the starting value

    // Automatic quality. Off, or pinned, or aimed at something other than the monitor.
    f32 target_fps = 0.0f;        // 0 means "the monitor's refresh rate"
    bool no_auto_quality = false;
    bool benchmark = false;       // re-run the machine measurement and save the result
    u64 edit_frame = 0;           // apply --edit on this frame instead of frame 100
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
    // are inside anybody's patience — but a quarter-detail build is markedly closer to the real
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
    // subject stays in frame for the whole run — which a constant velocity cannot do, and a
    // benchmark that flies past the building in the first second measures empty sky.
    std::string orbit;

    // frame,x,y,z,yaw,pitch — the camera jumps, once, at that measured frame.
    //
    // The instrument for anything that has to catch up with the view: light, streaming, residency.
    // Every other camera here moves smoothly, and smooth motion reveals a sliver of new world per
    // frame, so it measures the *rate* a system converges at while hiding what it does when handed
    // a whole screen at once. A cut is the worst case and it is also the ordinary one — turning
    // round in a doorway is a cut as far as the face store is concerned.
    //
    // Counted in MEASURED frames, so with --settle it fires after the world has stopped building
    // and the only thing missing from the new view is the thing under test.
    std::string cut;

    // Scripted chisel, for checking the tool without a person holding the mouse.
    // --edit "x0,y0,z0,x1,y1,z1,material" applies one edit through the history at startup;
    // material 0 carves. --preview takes the same six numbers plus a state (1 carve,
    // 2 place, 3 refused) and forces the preview box on. Both are in voxels.
    std::string edit;
    std::string preview;
    u32 material = 0;   // which entry of the chisel's palette starts selected

    // Scripted clipboard: --clip "x0,y0,z0,x1,y1,z1,dx,dy,dz,copies,turn" captures that box
    // at frame 100, offsets the ghost by (dx,dy,dz), fans out `copies`, and turns it `turn`
    // degrees about y. Everything after the box is optional.
    std::string clip;

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
        } else if (arg == "--ticks") {
            options.ticks = next_number(0);
            options.headless = true;
        } else if (arg == "--stream-frames") {
            options.stream_frames = next_number(600);
            options.stream_audit = true;
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
            if (i + 1 < argc) options.cut = argv[++i];
        } else if (arg == "--edit") {
            if (i + 1 < argc) options.edit = argv[++i];
        } else if (arg == "--preview") {
            if (i + 1 < argc) options.preview = argv[++i];
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
        } else if (arg == "--no-clip-cache") {
            options.no_clip_cache = true;
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
        } else if (arg == "--stream-log") {
            options.stream_log = true;
        } else if (arg == "--target-fps" && i + 1 < argc) {
            options.target_fps = static_cast<f32>(std::atof(argv[++i]));
        } else if (arg == "--edit-frame" && i + 1 < argc) {
            options.edit_frame = static_cast<u64>(std::atoll(argv[++i]));
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
        } else if (arg == "--crash-test" && i + 1 < argc) {
            options.crash_test = argv[++i];
        } else if (arg == "--hollow" && i + 1 < argc) {
            options.hollow = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--clip-coarse" && i + 1 < argc) {
            options.clip_coarse = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg == "--pathtrace") {
            options.path_trace = true;
        } else if (arg == "--node-pool") {
            options.node_pool = true;
        } else if (arg == "--chunk-marcher") {
            options.node_pool = false;
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
    return options;
}

void print_help() {
    std::puts(
        "WorldShaper\n"
        "  --headless            run with no window or GPU\n"
        "  --ticks N             headless world audit over N ops, then exit\n"
        "  --stream-frames N     headless streaming audit over N frames, then exit\n"
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
        "                        distinct visible faces (tools\\facecount.ps1)\n"
        "  --pathtrace           start in the reference path tracer (F4 toggles)\n"
        "  --chunk-marcher       march the old chunk grid instead of the node pool\n"
        "  --settle              start the measurement window once the world stops sharpening,\n"
        "                        rather than at frame nought. Any figure to be compared with\n"
        "                        another run needs this\n"
        "  --target-fps N        frame rate to hold (default: the monitor's refresh rate)\n"
        "  --quality N           pin the quality level 0-7 instead of deciding it\n"
        "  --no-auto-quality     leave quality where it is and never adjust it\n"
        "  --benchmark           measure this machine again and save the result\n"
        "  --fly vx,vy,vz,vyaw   move the camera every frame (m/s, deg/s), so a screenshot\n"
        "                        is of the moving picture rather than a settled one\n"
        "  --cut f,x,y,z,yaw,pitch  jump the camera once, at measured frame f. The worst case\n"
        "                        for anything that has to catch up with the view, and what\n"
        "                        turning round in a doorway looks like to the face store\n"
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
        "In game:  F1 developer panel   F2 overlay   F4 path trace   F5 reload shaders\n"
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
// A half is the film convention — a 180-degree shutter — and it is what an eye raised on film
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

constexpr f32 kShutterFraction = 0.5f;

// The longest streak, in pixels. A bound on cost rather than on looks: a fast spin can put a
// point most of the way across the screen in a frame, and there is no picture in a streak that
// long — only taps.
constexpr f32 kLongestStreak = 24.0f;

struct TracePush {
    f32 sun[4]{};          // xyz towards the sun, w cos of its angular radius
    f32 sun_colour[4]{};
    // x sample index, z frame, w world changed. y is spare: it used to carry a bounce limit,
    // which the shader never read and could not have used — see src/game/quality.hpp for why
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
// Ninety-six bytes, which is inside the 128 every Vulkan implementation is required to offer —
// the same bound src/gpu/render_params.hpp records having already been walked into once.
static_assert(sizeof(TracePush) == 96, "TracePush must match the shader's push block");

// Where the compiled shaders are.
//
// Beside the running executable first, which is what makes an unzipped release work wherever
// it is put. Only if they are not there does it fall back to the directory this build was
// configured with — useful for a developer running the exe from somewhere odd, and useless to
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
    // looks like, sits near the 0.4 bytes/voxel documentation/03 Â§3 budgets for.
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
// — which is the whole point of the format, and it means the scene can be edited without a
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
// changed, so that is what is measured — the newest modification time across the forge and the
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

int run_stream_audit(const Options& options) {
    const u64 frames = (options.stream_frames > 0) ? options.stream_frames : 600;
    WS_LOG_INFO("app", "streaming audit: {} frames over the test scene", frames);

    TagRegistry tags;
    PropertyRegistry properties;
    VoxelTypeTable types;
    World world;
    MatterLedger ledger;

    const u64 build_start = now_ns();
    {
        forge::Script script = forge::load_clip_script(default_clip_path(), types, tags);
        if (script.ok()) {
            JobSystem build_jobs;
            forge::SampleResult built = forge::sample(script.field, script.solid, script.paint,
                                                      script.settings, &build_jobs);
            forge::apply_variation(built.clip, types, script.field, script.variation,
                                   script.settings, built, &build_jobs);
            std::vector<Op> ops;
            clip_to_ops(built.clip, built.origin_voxel[0], built.origin_voxel[1],
                        built.origin_voxel[2], PasteMode::SolidOnly, 1, 1, ops);
            for (const Op& op : ops) apply_op(world, op, ledger);
        }
    }
    world.compact();
    const f64 build_ms = ns_to_ms(now_ns() - build_start);

    const WorldStats world_stats = world.stats();
    WS_LOG_INFO("audit", "scene: {} chunks, {} bricks, {} solid voxels, {} MB ({:.3f} B/voxel), built in {:.0f} ms",
                world_stats.chunks, world_stats.bricks, world_stats.solid_voxels,
                world_stats.bytes / (1024 * 1024), world_stats.bytes_per_voxel(), build_ms);

    // Deliberately smaller than the scene so eviction and re-upload are exercised, but
    // large enough to hold the working set the camera path actually looks at â€” a budget
    // that cannot hold what is on screen measures thrashing rather than streaming.
    ResidencyBudget budget;
    budget.payload_bytes = 24ull << 20;
    budget.max_bricks = 80000;
    ResidencyManager residency;
    residency.create(budget, types);

    bool ok = true;
    u64 checked = 0;
    f64 worst_update_ms = 0.0;
    u64 worst_update_frame = 0;
    f64 total_update_ms = 0.0;

    for (u64 frame = 1; frame <= frames; ++frame) {
        const f64 seconds = static_cast<f64>(frame) / 60.0;
        const f64 angle = seconds * 0.25;
        const i64 focus_x = static_cast<i64>(std::cos(angle) * 700.0);
        const i64 focus_z = static_cast<i64>(std::sin(angle) * 700.0);
        const ChunkCoord centre = chunk_coord_of(focus_x, 0, focus_z);

        for (i64 z = -3; z <= 3; ++z) {
            for (i64 y = -1; y <= 1; ++y) {
                for (i64 x = -3; x <= 3; ++x) {
                    const ChunkCoord coord{centre.x + x, centre.y + y, centre.z + z};
                    if (world.has_chunk(coord)) residency.request(coord);
                }
            }
        }

        const u64 update_start = now_ns();
        residency.update(world, frame);
        const f64 update_ms = ns_to_ms(now_ns() - update_start);
        total_update_ms += update_ms;
        if (update_ms > worst_update_ms) {
            worst_update_ms = update_ms;
            worst_update_frame = frame;
        }

        if (!residency.validate()) {
            WS_LOG_ERROR("audit", "residency invariants FAILED at frame {}", frame);
            ok = false;
            break;
        }

        // Sample the shader's own lookup path â€” wrapped grid, record, mask, rank â€” near
        // the camera. This is what catches grid aliasing and stale records, which a
        // per-chunk hash comparison cannot see because it never goes through the grid.
        for (u32 sample = 0; sample < 256; ++sample) {
            const u64 h = hash_cell(static_cast<i64>(frame), sample, 0, frame, 0x57414C4Bull);
            const i64 x = focus_x + static_cast<i64>(hash_range(h, 512)) - 256;
            const i64 y = static_cast<i64>(hash_range(hash_mix(h + 1), 64)) - 32;
            const i64 z = focus_z + static_cast<i64>(hash_range(hash_mix(h + 2), 512)) - 256;
            if (!residency.resident(chunk_coord_of(x, y, z))) continue;
            ++checked;
            if (residency.mirror_voxel_world(x, y, z) != world.get(x, y, z)) {
                WS_LOG_ERROR("audit", "shader-path lookup disagreed at ({}, {}, {}) on frame {}",
                             x, y, z, frame);
                ok = false;
                break;
            }
        }
        if (!ok) break;

        // Everything resident must agree with the world, every frame.
        for (const ChunkCoord& coord : world.sorted_chunk_coords()) {
            if (!residency.resident(coord)) continue;
            ++checked;
            if (residency.mirror_hash(coord) != world.chunk_hash(coord)) {
                WS_LOG_ERROR("audit", "mirror mismatch at chunk ({}, {}, {}) on frame {}",
                             coord.x, coord.y, coord.z, frame);
                ok = false;
                break;
            }
        }
        if (!ok) break;
    }

    const ResidencyStats stats = residency.stats();
    WS_LOG_INFO("audit", "resident {} chunks / {} bricks, {} KB payload of {} KB",
                stats.resident_chunks, stats.resident_bricks, stats.payload_in_use / 1024,
                stats.payload_capacity / 1024);
    WS_LOG_INFO("audit", "uploads {}  evictions {}  hit rate {:.1f}%  chunk checks {}",
                stats.uploads, stats.evictions, stats.hit_rate() * 100.0, checked);
    WS_LOG_INFO("audit", "residency update: {:.3f} ms average, {:.3f} ms worst (frame {})",
                total_update_ms / static_cast<f64>(frames), worst_update_ms,
                worst_update_frame);

    if (!ok) {
        WS_LOG_FATAL("audit", "FAILED");
        return 1;
    }
    WS_LOG_INFO("audit", "the mirror matched the world on every frame");
    return 0;
}

class Application {
public:
    int run(const Options& options);

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
    // keeps moving through the parts that are NOT the world build — the pipelines, the residency,
    // the first upload. Those are the parts a bar usually leaves out, which is exactly why it
    // reaches ninety-nine per cent and then sits there.
    void draw_loading();
    std::string loading_cache_path() const;

    void stream(f64 seconds);
    void update_tools(const InputState& input, bool chisel_has_wheel, bool clipboard_has_wheel,
                      f64 dt);
    void invalidate_edited_chunks(const std::vector<Op>& ops);
    void rebuild_coarse_grids();

    Options options_;
    Window window_;
    Device device_;
    Swapchain swapchain_;
    GpuProfiler profiler_;
    Hud hud_;

    // The load, and the screen that reports it. The screen is torn down once the game is up — it
    // holds a full-resolution image and nothing after startup needs it — but the progress itself
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
    // state — interning the varied materials, writing voxels, replaying the edit log — happens on
    // the main thread between frames, because the type table and the world are not thread-safe and
    // making them so to save a few milliseconds a minute would be a poor trade.
    std::unique_ptr<forge::Script> refine_script_;
    std::unique_ptr<JobSystem> refine_jobs_;
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
    // times the last, so the final one is minutes, and until it lands EVERYTHING is coarse — the
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
    // finished" — see the note in start_refinement — but it IS a fixed point, so it is the state a
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
    // world and the run that loads a half-built one plan the same grid — if they did not, the
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

    ComputePipeline visibility_;
    ComputePipeline resolve_;
    // R3: one invocation per face, working out light on the surface instead of on the screen.
    ComputePipeline shade_faces_;

    // The reference path tracer. A separate pipeline that shares only the world, so with the
    // mode off it costs exactly nothing — no branch in the marcher, no extra binding.
    ComputePipeline pathtrace_;
    // The cloud volume, marched once per four-by-four block. See shaders/clouds.comp.
    ComputePipeline clouds_;
    GpuImage cloud_image_;
    GpuImage cloud_image_prev_;
    GpuImage cloud_marched_;
    bool cloud_ready_ = false;   // transitioned out of UNDEFINED once, then left in GENERAL
    u32 cloud_parity_ = 0;       // which of the two the cloud pass writes this frame
    VkDescriptorSetLayout pathtrace_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet pathtrace_set_ = VK_NULL_HANDLE;
    GpuImage accum_image_;
    // One entry per voxel face the camera has looked at, 32 bytes each. Light is computed
    // once per face and shared by every pixel that sees it, and because the key is a place in
    // the world rather than on the screen, turning the camera does not throw the work away.
    GpuBuffer face_cache_;
    bool face_cache_dirty_ = true;
    // Whole-frame numbers the tracer adds up on the GPU, so that next frame can be exposed
    // for the picture this one turned out to be. See gpu/render_params.hpp for the layout and
    // for why there are two slots rather than one.
    GpuBuffer frame_stats_;
    bool frame_stats_zeroed_ = false;   // the very first frame has no previous frame to read
    bool path_trace_ = false;
    u32 trace_samples_ = 0;      // samples accumulated since the last reset
    // How many more frames an edited world holds its accumulator down. Long enough that the light
    // around the edit has actually moved, short enough that it is over before anybody looks for it.
    static constexpr u32 kEditSettleFrames = 20;
    static constexpr f32 kEditKeepsWeight = 24.0f;
    u32 edited_recently_ = 0;

    // Holds the frame rate by spending detail where it is worth most. Measured on the machine
    // it is running on, once, the first time the game starts. See documentation/19.
    // Frames left during which every surface re-measures its shadow, set by an edit. About two
    // seconds: long enough for a distant face covered by one pixel to gather the samples it
    // needs, short enough that it is over before anyone places the next voxel.
    u32 shadow_refresh_frames_ = 0;
    static constexpr u32 kShadowRefreshFrames = 120;

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
    // of them, so the first frame — three hundred milliseconds of driver warm-up, every time — is
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

    // --cut: where the camera jumps to, and at which measured frame. See Options::cut.
    f64 cut_pose_[5]{};
    u64 cut_at_ = 0;
    bool cut_pending_ = false;
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
    // player's — so honest weather at honest speed crosses the sky at a few hundred metres a
    // second, which does not read as weather at all. It reads as smoke in a wind tunnel. The
    // coupling to game time is kept, because a cloud should cross a field in an in-game hour and
    // not an in-game week; the speed is set by how it looks.
    f32 cloud_wind_[2]{0.40f, 0.16f};
    f32 prev_cloud_time_ = 0.0f;
    bool accum_ready_ = false;   // transitioned out of UNDEFINED once, then left in GENERAL
    bool face_ready_ = false;    // same, and for the same reason: it must survive a frame that
                                 // does not write it
    bool face_cleared_ = false;  // and holds kNoFace everywhere, so no clear is owed
    GpuImage visibility_image_;
    // Which face store slot each pixel's surface lives in. One word a pixel, resolved by the
    // marcher where the key is already known and read by the composite.
    GpuImage face_image_;
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
    ResidencyManager residency_;
    SummaryTree summary_tree_;
    ThumbnailCache thumb_tiers_[kSummaryTiers];
    ThumbnailBudget thumb_budgets_[kSummaryTiers];
    u32 thumb_total_slots_ = 0;
    u32 thumb_total_grid_ = 0;
    ChunkCoord world_min_{};
    ChunkCoord world_max_{};
    bool world_bounds_valid_ = false;
    WorldBuffers world_buffers_;
    ResidencyBudget residency_budget_;
    FeedbackBuffer feedback_;
    GpuBuffer params_buffer_;
    // The clipboard's held clip, as the resolve pass sees it: one cell per voxel, holding
    // the type id plus one so that zero can mean "outside the clip". Device-local, because
    // a ghost that fills the screen is read once per pixel and reading that over the bus
    // would make the preview cost more than the world behind it.
    GpuBuffer clip_buffer_;
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
    u32 last_feedback_accepted_ = 0;
    u32 last_feedback_rejected_ = 0;   // reported, but the world has no chunk there
    u32 last_thumbs_resident_ = 0;
    u32 last_thumbs_wanted_ = 0;
    u32 last_thumbs_built_ = 0;
    f64 residency_ms_ = 0.0;
    f64 worst_residency_ms_ = 0.0;
    // The same, for the pool. The chunk path has been timed since Stage 2 and the node path never
    // was, so the CPU cost of building the tree has never appeared in any figure -- which is a
    // problem when a frame is 275 ms, the GPU is 7 ms, and nothing accounts for the rest.
    f64 node_ms_ = 0.0;
    f64 worst_node_ms_ = 0.0;
    // WHICH frame was the worst, which is the question three attempts at making it smaller should
    // have started with: a worst-of-run taken over startup is not something steady play can feel.
    u64 worst_node_frame_ = 0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

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
    };
    NodePush make_node_push(u32 face_count) const;

    // The node pool, beside the chunk grid rather than replacing it yet. See
    // documentation/21-renderer-rewrite.md section 8, sub-step R1c.
    NodePool node_pool_;
    NodeBuffers node_buffers_;
    // R3. Claimed from what the marcher reports and mirrored to the card; nothing shades it yet.
    FaceStore face_store_;
    FaceBuffers face_buffers_;
    u32 last_faces_seen_ = 0;
    ComputePipeline node_visibility_;
    VkDescriptorSetLayout node_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet node_set_ = VK_NULL_HANDLE;
    bool use_node_pool_ = false;
    u32 last_node_built_ = 0;
    u32 last_node_evicted_ = 0;
    u32 last_node_deferred_ = 0;
    FrameStats stats_;
    // A device that dies of a timeout and a device that dies of a bad address leave exactly the
    // same message behind. The difference is in the frames just before it, and those are gone by
    // the time anyone reads the report — so they are kept here, and a frame slow enough to be
    // heading for the driver's patience says so at the time.
    f64 worst_frame_ms_ = 0.0;
    u64 worst_frame_at_ = 0;
};

bool Application::create_render_target(u32 width, u32 height) {
    visibility_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32G32B32A32_UINT,
                                             "visibility");
    face_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32_UINT, "face slots");
    render_target_ = create_storage_image(device_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                          "render_target");
    depth_target_ = create_storage_image(device_, width, height, VK_FORMAT_R32_SFLOAT,
                                         "depth_target");
    // Full float, and not an economy. At a few thousand samples the differences between two
    // materials are smaller than an 8-bit step, so an 8-bit accumulator would quantise away
    // exactly what this mode exists to show.
    accum_image_ = create_storage_image(device_, width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
                                        "path trace accumulation");
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

    trace_samples_ = 0;
    accum_ready_ = false;   // a new image, so it needs its one transition out of UNDEFINED
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

    VkDescriptorImageInfo accum_info{};
    accum_info.imageView = accum_image_.view;
    accum_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo cloud_info[2]{};
    cloud_info[0].imageView = cloud_image_.view;
    cloud_info[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    cloud_info[1].imageView = cloud_image_prev_.view;
    cloud_info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Eleven, not nine: the last two are the node marcher's output images.
    //
    // They were missing, and the failure was silent in the worst way. The node pipeline ran, did
    // all its work, and stored its result into an unwritten descriptor - so the visibility image
    // kept whatever was in it and the picture never changed no matter what the marcher did. Five
    // separate changes to the traversal produced bit-identical images while the *timing* moved
    // with every one of them, which is the signature: the shader is running and its output is
    // going nowhere.
    //
    // Descriptors for the images are the only bindings that change on resize, which is why they
    // live here rather than beside the buffer writes where the node set was otherwise assembled -
    // and that split is exactly how they came to be forgotten.
    VkWriteDescriptorSet writes[13]{};
    for (VkWriteDescriptorSet& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].dstSet = descriptor_set_;
    writes[0].dstBinding = 0;
    writes[0].pImageInfo = &vis_info;
    writes[1].dstSet = descriptor_set_;
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &depth_info;
    writes[2].dstSet = resolve_set_;
    writes[2].dstBinding = 0;
    writes[2].pImageInfo = &vis_info;
    writes[3].dstSet = resolve_set_;
    writes[3].dstBinding = 1;
    writes[3].pImageInfo = &colour_info;
    writes[4].dstSet = pathtrace_set_;
    writes[4].dstBinding = 0;
    writes[4].pImageInfo = &accum_info;
    writes[5].dstSet = pathtrace_set_;
    writes[5].dstBinding = 1;
    writes[5].pImageInfo = &colour_info;
    VkDescriptorImageInfo marched_info{};
    marched_info.imageView = cloud_marched_.view;
    marched_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[6].dstSet = pathtrace_set_;
    writes[6].dstBinding = kCloudBinding;
    writes[6].descriptorCount = 2;
    writes[6].pImageInfo = cloud_info;
    writes[7].dstSet = pathtrace_set_;
    writes[7].dstBinding = kCloudMarchedBinding;
    writes[7].pImageInfo = &marched_info;
    // And the same pair to the raster pass, which draws the sky the player actually sees.
    writes[8].dstSet = resolve_set_;
    writes[8].dstBinding = kCloudBinding;
    writes[8].descriptorCount = 2;
    writes[8].pImageInfo = cloud_info;
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
    writes[9].dstSet = resolve_set_;
    writes[9].dstBinding = 7;
    writes[9].pImageInfo = &face_info;
    u32 write_count = 10;
    if (node_set_ != VK_NULL_HANDLE) {
        writes[10].dstSet = node_set_;
        writes[10].dstBinding = 0;
        writes[10].pImageInfo = &vis_info;
        writes[11].dstSet = node_set_;
        writes[11].dstBinding = 1;
        writes[11].pImageInfo = &depth_info;
        writes[12].dstSet = node_set_;
        writes[12].dstBinding = 11;
        writes[12].pImageInfo = &face_info;
        write_count = 13;
    }
    vkUpdateDescriptorSets(device_.handle(), write_count, writes, 0, nullptr);
    return true;
}

void Application::destroy_render_target() {
    if (visibility_image_.valid()) destroy_image(device_, visibility_image_);
    if (face_image_.valid()) destroy_image(device_, face_image_);
    if (render_target_.valid()) destroy_image(device_, render_target_);
    if (depth_target_.valid()) destroy_image(device_, depth_target_);
    if (accum_image_.valid()) destroy_image(device_, accum_image_);
    // The cloud history and its march buffer, created here with the rest and until now not
    // released with them. Three images and their memory, leaked once per resize and once per
    // run; validation names them at vkDestroyDevice.
    if (cloud_image_.valid()) destroy_image(device_, cloud_image_);
    if (cloud_image_prev_.valid()) destroy_image(device_, cloud_image_prev_);
    if (cloud_marched_.valid()) destroy_image(device_, cloud_marched_);
}

// Both axes scale by the same number and neither is rounded to the workgroup, because the
// aspect ratio has to survive: the dispatch already rounds up and the shaders already discard
// invocations past the resolution in the parameter block — they must, since a 1600x900 window
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

std::string Application::loading_cache_path() const {
    const std::string clip =
        options_.clip_file.empty() ? default_clip_path() : options_.clip_file;
    return clip + ".load";
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
    // setting for it yet — but it is only ever reached in the narrow band where inversion has
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
        // There is no ladder at all — the clip was built at its authored detail in one pass, or
        // the world came back from the cache already finished. Nothing here can be improved, and
        // that is precisely what settled means.
        //
        // It used to return without saying so, and --settle waits on this flag: a run with no
        // ladder therefore waited for a fixed point that had already happened and never took its
        // measurement at all. That was reachable before — `--clip-coarse 1 --settle` hangs — and
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

        // And whether it is in front. Not a frustum test — a box behind the player is not merely
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
        // the world answers it — the world the ray crosses is the coarse one, and a blocky wall
        // occludes exactly as well as a sharp one for this purpose.
        //
        // Asked last, and only of a box that is already the front runner, because a raycast is far
        // dearer than the arithmetic above and most boxes are eliminated by it.
        if (best != refine_regions_.size() && keen <= keenest) continue;

        if (reach > 1e-6) {
            const f64 v = static_cast<f64>(kVoxelsPerMetre);
            const RayHit blocked = raycast(world_, cx * v, cy * v, cz * v, to_x, to_y, to_z,
                                           reach * v);
            // Something solid, and not merely the box's own front face — anything within its own
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
        // Refinement has run out of things this camera can improve, and — because this branch is
        // only reached with no box in flight — the last one has landed. That is the moment the
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
    // larger. Overlapped, the sampler is never waiting on a paste it takes no part in — which very
    // nearly halves how long it takes for what you are looking at to come good.
    //
    // Safe because nothing below reads the script: the paste needs only the result, and variation —
    // the one thing that did read it — no longer runs per region. The box is marked done first, so
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
    // through — an assert, mid-play, after several minutes of hitching.
    //
    // It was also most of the hitch. Interning a million materials on the main thread is what those
    // two-hundred to nine-hundred millisecond frames were.
    //
    // So the world keeps the flat materials its paint rules give it. The no-two-voxels-alike
    // shading is lost until there is somewhere to put it that does not scale with how the world was
    // divided up — which is a real gap and is recorded as one, not a decision that this looks
    // better.

    // REPLACE, so the box supersedes the coarse voxels standing in for it. Stamped instead, the
    // blocky overshoot survives outside the finer surface and the world only ever grows.
    const PasteStats stamped = paste_clip(
        world_, ledger_, finished->clip, finished->origin_voxel[0] + refine_at_[0],
        finished->origin_voxel[1] + refine_at_[1],
        finished->origin_voxel[2] + refine_at_[2], PasteMode::Replace,
        MatterReason::PlayerPlace, 1, refine_jobs_.get(), types_.type_count(), 1);
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

    // Everything the player did, done again. An op is a SHAPE — FillBox carries two corners in
    // world voxels, not the voxels it happened to change — so replaying it against finer geometry
    // re-cuts the same volume at the new detail. The cut re-measures itself.
    const std::vector<Op>& done = op_log_.ops();
    if (!done.empty()) apply_ops(world_, done, ledger_);

    finished.reset();

    usize left = 0;
    for (const RefineRegion& box : refine_regions_) {
        if (!box.done) ++left;
    }
    WS_LOG_INFO("clip", "region: sampled {:.0f} ms ({} voxels asked), pasted {:.0f} ms, {} left",
                refine_sample_ms_, refine_asked_, ns_to_ms(now_ns() - began), left);

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
        return;
    }

    start_refinement();
}

// What the world on disk is worth, and when it is worth writing.
//
// It used to be written only when the LAST box landed, and the last box never lands: a box behind
// a wall is skipped by the occlusion test in start_refinement and stays coarse for as long as the
// camera stands where it does. The facility settles at fourteen boxes of eighteen from its own
// default camera, so the cache was never written once, and every launch — and every one of the
// forty-two runs of the measurement grid — rebuilt a hundred and twenty-five million voxels from
// the field. Two minutes, every time, for a file that was already sitting there in every sense
// except that nobody had saved it.
//
// So it is written at the fixed point instead, with the flags that say what it is. The next run
// loads it in a second, and if it stands somewhere else it sharpens what it can see from there and
// writes again — the world converges across runs rather than being thrown away at the end of each.
void Application::save_refined_world() {
    if (refine_cache_path_.empty() || refine_regions_.empty()) return;

    usize done = 0;
    for (const RefineRegion& box : refine_regions_) {
        if (box.done) ++done;
    }
    // Nothing has been sharpened since the file was written. Rewriting six hundred megabytes to
    // say the same thing is the sort of cost that only shows up as a stutter nobody can explain.
    if (done <= refine_saved_regions_) return;

    // An edited world is not cached MID-ladder, and the reason is not obvious. A region paste is
    // a Replace over its box, so a later box would put pristine clip geometry back over anything
    // carved inside it — which the live session survives because pump_refinement replays the op
    // log after every paste, and a fresh run would not, because its op log starts empty. The edits
    // would quietly come back. A world that is FINISHED has no later box to undo them and is
    // cached as it stands, which is what it has always done.
    if (done < refine_regions_.size() && !op_log_.ops().empty()) {
        WS_LOG_INFO("clip",
                    "{} of {} regions sharpened, but the world has been edited; not caching a "
                    "half-built world an edit would be replayed over",
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
    if (!write_world_cache(refine_cache_path_, refine_cache_key_, cache)) return;
    refine_saved_regions_ = done;
    WS_LOG_INFO("clip", "kept the world with {} of {} regions sharpened", done,
                refine_regions_.size());
}

// Boxes of about twelve metres, cut from the clip's own bounds.
//
// Refining the whole world a rung at a time is the wrong shape for this: every rung is eight times
// the last, so the final one is minutes, and until it lands EVERYTHING is coarse — the wall you are
// standing at included. Sampling the box you are standing in instead is a second.
//
// Twelve metres, and the number comes from measuring where the time goes. Sampling a four-metre box
// took about a hundred milliseconds for ten thousand voxels — ten microseconds each, against barely
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
// standing — which is usually somewhere else, which is how a world that no single camera can finish
// still finishes.
//
// The boxes are checked rather than trusted, and this is not belt and braces. The cache key covers
// the clip's text, the resolution, and the modification times of src/forge, src/world and
// src/game/clip.* — deliberately, so that editing a menu label does not throw away a minute of
// resampling. plan_refine_regions is in NEITHER, so changing how the grid is cut leaves every
// existing cache file matching its key while its flags refer to boxes that no longer exist. The
// alternative — adding this file to the key — would invalidate every built world on every edit to
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
        const std::string path =
            options_.clip_file.empty() ? default_clip_path() : options_.clip_file;

        // The clip is read once, here, with everything it includes spliced in, and that spliced
        // text is what everything downstream works from — the cache key as well as the parser.
        // Keying the cache on the whole assembly and not just the manifest is what makes editing
        // one fragment of a twenty-fragment building rebuild the building.
        progress_.enter(LoadStage::Reading);

        std::vector<forge::SourceLine> origin;
        std::vector<forge::ScriptError> trouble;
        const std::string source = forge::expand_includes(path, origin, trouble);

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
                // and the part comes out wearing the materials it will wear in the building —
                // which is the point of looking at it.
                script.solid = piece;
                script.has_solid = true;
            } else {
                WS_LOG_ERROR("clip", "no part called '{}' — check the `let` name",
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
        const std::string cache_path = path + ".world";
        const u64 key =
            world_cache_key(source + "|part=" + options_.clip_part,
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
                materials_ = cache.materials;
                if (materials_.empty()) materials_.push_back(1);
                material_index_ = options_.material % materials_.size();
                chisel_.set_material(materials_[material_index_]);
                // What came off the disk may be a world that stopped short — see
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
            const u64 sampled_at = now_ns();
            progress_.enter(LoadStage::Varying);
            // Every voxel gets its own version of its material before it goes in, so the world
            // holds the varied clip rather than the flat one — but only at the detail that keeps
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
            materials_ = script.material_types;
            if (materials_.empty()) materials_.push_back(1);
            material_index_ = options_.material % materials_.size();
            chisel_.set_material(materials_[material_index_]);
            const WorldStats clip_stats = world_.stats();
            WS_LOG_INFO("world", "'{}' built in {:.0f} ms: {} chunks, {} solid voxels", path,
                        ns_to_ms(now_ns() - start), clip_stats.chunks, clip_stats.solid_voxels);

            // Kept, so the next run does not do any of that again — but NOT while it is still
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
        WS_LOG_ERROR("clip", "'{}' did not build — the world is empty", path);
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
    // already converged — which is the case a player is actually in, and the one where new
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
            rebuild_coarse_grids();
            // The same invalidation the interactive path does. Without it the summary tree
            // never hears that these chunks exist, so nothing past the streaming range is
            // ever drawn — which made a scripted edit behave differently from the identical
            // edit made by hand, and hid the difference behind "it works when I play it".
            invalidate_edited_chunks({op});
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

    if (input.was_pressed(Key::Q) && !materials_.empty()) {
        material_index_ = (material_index_ + materials_.size() - 1) % materials_.size();
        chisel_.set_material(materials_[material_index_]);
    }
    if (input.was_pressed(Key::E) && !materials_.empty()) {
        material_index_ = (material_index_ + 1) % materials_.size();
        chisel_.set_material(materials_[material_index_]);
    }

    // Undo on Z or Ctrl+Z, redo on X, Y or Ctrl+Y — and all of them repeat while held.
    //
    // Undoing thirty steps should be one long press, not thirty presses. The repeat is the
    // same time-based one the clipboard's counters use: a pause before it starts, so a single
    // tap is still a single step, then steadily.
    const bool ctrl = input.is_down(Key::Ctrl);
    const bool undo_down = input.is_down(Key::Z);
    const bool redo_down = input.is_down(Key::X) || input.is_down(Key::Y) ||
                           (ctrl && input.is_down(Key::Y));
    if (repeat_undo_.poll(undo_down, dt) > 0) {
        if (history_.undo(world_, ledger_, op_log_, tick_++, kLocalPlayer)) {
            rebuild_coarse_grids();
        }
    }
    if (repeat_redo_.poll(redo_down, dt) > 0) {
        if (history_.redo(world_, ledger_, op_log_, tick_++, kLocalPlayer)) {
            rebuild_coarse_grids();
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
        tool.add_point = input.was_pressed(Key::X);
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
        tool.add_point = input.was_pressed(Key::X);
        tool.wheel = chisel_has_wheel ? input.wheel : 0.0f;
        tool.adjust_distance = chisel_has_wheel;
        tool.clear_points = input.was_pressed(Key::R);
        tool.cancel = input.was_pressed(Key::Backspace);
        tool.toggle_overwrite = input.was_pressed(Key::P);
        tool.toggle_anchor = input.was_pressed(Key::O);

        Op op;
        if (!chisel_.update(world_, tool, origin, direction, tick_, kLocalPlayer, op)) return;

        // A hollow box is six slabs with the middle left alone — untouched rather than
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
    // one. The coarse occupancy grids describe what the world contains, so they are what
    // tells the marcher there is now something to look for here.
    if (result.voxels_changed > 0) {
        rebuild_coarse_grids();
        invalidate_edited_chunks(ops);
    }
}

// Tells streaming which chunks an edit changed.
//
// This has to be pushed, because it cannot be pulled. The renderer's feedback reports chunks
// it wanted and *could not find* — that is the whole mechanism. A chunk that is resident but
// out of date is found, so it is never reported, so it is never refreshed, and it goes on
// showing what it used to hold. Carve a room inside a hill you have already looked at and
// the hill stays solid until something unrelated evicts that chunk.
//
// The edit knows exactly which chunks it touched, so it says so.
void Application::invalidate_edited_chunks(const std::vector<Op>& ops) {
    // And for a couple of seconds afterwards, every surface re-measures its shadow.
    //
    // A converged face stops tracing shadow rays and is only refreshed by a two per cent
    // trickle. Close to the camera a face is covered by hundreds of pixels, so two per cent of
    // them is a steady stream and a new shadow arrives at once; at distance a face is covered
    // by one pixel or less, two per cent of that is nothing, and the shadow of something just
    // placed never appears. Worse, a face below kShadowSeed samples leans on a parent node
    // sixty-four voxels across, which is dominated by surface that is still lit — so the new
    // shadow is not merely late, it is actively averaged away.
    //
    // That is exactly the report: new voxels cast no shadow until the camera comes close, and
    // then it fades in and stays. Coming close is what finally supplies the samples.
    //
    // So an edit says "look again" to everything, briefly. Not a wipe of the cache — that was
    // tried and it is the smearing, every voxel placed relighting the whole scene at once.
    // This keeps every measured value and simply re-measures faster for a moment.
    shadow_refresh_frames_ = kShadowRefreshFrames;
    lights_dirty_ = true;   // a placed lamp is a light nothing can aim at until this is rebuilt

    // And it says it to the region rather than to the world. Everything the edit could have
    // changed the light of is inside its own bounds grown by the reach of a shadow; nothing
    // outside that can have changed at all, so nothing outside is disturbed.
    bool first = true;
    for (const Op& raw : ops) {
        Op op = raw;
        op.normalise();
        const i64 lo[3] = {op.x0, op.y0, op.z0};
        const i64 hi[3] = {op.x1, op.y1, op.z1};
        for (u32 axis = 0; axis < 3; ++axis) {
            edit_lo_[axis] = first ? lo[axis] : std::min(edit_lo_[axis], lo[axis]);
            edit_hi_[axis] = first ? hi[axis] : std::max(edit_hi_[axis], hi[axis]);
        }
        first = false;
    }
    for (u32 axis = 0; axis < 3; ++axis) {
        edit_lo_[axis] -= kEditShadowReach;
        edit_hi_[axis] += kEditShadowReach;
    }

    for (const Op& raw : ops) {
        Op op = raw;
        op.normalise();
        for (i64 cz = chunk_of(op.z0); cz <= chunk_of(op.z1); ++cz) {
            for (i64 cy = chunk_of(op.y0); cy <= chunk_of(op.y1); ++cy) {
                for (i64 cx = chunk_of(op.x0); cx <= chunk_of(op.x1); ++cx) {
                    const ChunkCoord coord{cx, cy, cz};
                    // invalidate(), not request(): a request lives for one frame and is
                    // dropped when that frame's upload budget runs out. A large edit touches
                    // hundreds of chunks and the budget serves four, so all but the first
                    // four were being forgotten — and nothing ever asked again, because a
                    // stale chunk is one the renderer can still find.
                    if (!world_.has_chunk(coord)) continue;
                    residency_.invalidate(coord);

                    // And ask for it outright, which invalidate() alone does not do: it
                    // refreshes a chunk that is already resident and drops one that is not.
                    //
                    // A chunk nobody has looked at closely is not resident, and a shadow ray
                    // cannot be occluded by geometry that is not there. Shadow rays are also
                    // the one kind that deliberately never request streaming, so a structure
                    // built at a distance was never fetched by the rays that needed it: it
                    // cast no shadow until the camera came close enough for *primary* rays to
                    // pull it in, at which point the shadow was measured, cached, and stayed —
                    // which is precisely the "shadows only appear when I get close, then fade
                    // in and remain" that was reported. Measured: with the camera 400 m away,
                    // an edited region was 0 of 101 chunks resident.
                    //
                    // A chunk the player just built is not a guess about what might be looked
                    // at. It is the one thing on screen they are certain to care about.
                    residency_.request(coord);
                    // A summary is a summary of contents, so changing the contents makes it
                    // wrong too — and it is what this chunk draws as from a distance. The
                    // tree first: it holds the node that every level above this chunk was
                    // folded from, and the cache only reads its answers.
                    summary_tree_.invalidate(coord);
                    // The accumulated *image* is of a world that changed, so it restarts. The
                    // camera moving does the same; the face cache does not, because it is
                    // keyed to places in the world rather than to the screen.
                    //
                    // The face cache deliberately is *not* wiped. Wiping it meant every voxel
                    // placed relit the entire scene at once, which is what the smearing while
                    // building actually was — not a settle but the whole room changing under
                    // you. Entries average over a sliding window instead, so a face follows
                    // what was built beside it within a few frames on its own and nothing
                    // further away flinches. See kFaceWindow in pathtrace.comp.
                    trace_samples_ = 0;
                    for (ThumbnailCache& tier : thumb_tiers_) tier.invalidate(coord);
                }
            }
        }

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
        // the node and every ancestor folded from it. The op is normalised above, so these bounds
        // are already the right way round.
        for (i64 bz = op.z0 >> kLeafLevel; bz <= (op.z1 >> kLeafLevel); ++bz) {
            for (i64 by = op.y0 >> kLeafLevel; by <= (op.y1 >> kLeafLevel); ++by) {
                for (i64 bx = op.x0 >> kLeafLevel; bx <= (op.x1 >> kLeafLevel); ++bx) {
                    node_pool_.invalidate(bx << kLeafLevel, by << kLeafLevel, bz << kLeafLevel);
                }
            }
        }
    }
}

// Stands in for the renderer's feedback buffer until Stage 3: request every chunk within
// a radius of a moving focus point. The residency manager cannot tell the difference, and
// neither can the eviction path, which is the point of exercising it now.
// Rebuilds the world-occupancy grids around wherever the camera is now.
//
// Level 0 of those grids — the one that answers "the world has a chunk here and you do not
// have it" — is only recorded within a window around this point, because that is what stops
// two distant regions colliding in the wrapped grid and inventing chunks that do not exist.
// So the grids have to follow the camera, not only world edits.
void Application::rebuild_coarse_grids() {
    residency_.rebuild_coarse(
        world_, ChunkCoord{camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()});
    // Every path that changes which chunks exist already comes through here, which makes it
    // the one place the thumbnail cache needs telling that its work list is stale.
    for (ThumbnailCache& tier : thumb_tiers_) tier.mark_world_changed();

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

    // Keep level 0 centred on the camera. Rebuilt on a margin rather than on every chunk
    // boundary, so walking to and fro across one does not rebuild twice a second. The
    // margin is well inside the window, so the grid is never consulted past what it
    // describes.
    const ChunkCoord centre{camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()};

    // The wrapped record grid can only describe one period at a time, so residency follows
    // the camera. Set before any request is made this frame, because request() uses it.
    residency_.set_view_centre(centre);

    const ChunkCoord& built = residency_.coarse_centre();
    constexpr i64 kCoarseFollowMargin = 8;   // chunks, 64 m
    if (std::abs(centre.x - built.x) > kCoarseFollowMargin ||
        std::abs(centre.y - built.y) > kCoarseFollowMargin ||
        std::abs(centre.z - built.z) > kCoarseFollowMargin) {
        rebuild_coarse_grids();
    }

    // What the renderer asked for, two frames ago. This is the rule that makes residency
    // follow the *view* rather than the camera position — a chunk 300 m away that covers
    // a hundred pixels gets streamed; one 10 m away behind a wall does not.
    const std::vector<FeedbackEntry>& wanted = feedback_.read(swapchain_.frame_index());
    last_feedback_ = feedback_.last_reported();
    last_feedback_truncated_ = feedback_.last_truncated();
    u32 accepted = 0;
    u32 rejected = 0;

    // Which format this frame's reports are in, and it is decided by **who wrote the buffer**
    // rather than by which marcher is configured. A node entry carries a node coordinate at its
    // own level; a chunk entry carries a chunk coordinate and a detail level nothing shifts by.
    //
    // The path tracer is the case that separates the two. It has not been ported to the node
    // pool, so it marches `world.glsl` and reports chunks — while `use_node_pool_` is still
    // true, because that flag says what the *visibility* pass would do and the visibility pass
    // does not run in this mode at all. Reading a chunk coordinate as a node coordinate shifts
    // it by a detail level and asks for a chunk kilometres from the one that was missing, so
    // streaming stops serving the tracer: measured 52 of 68 chunks against 57.
    const bool node_feedback = use_node_pool_ && !path_trace_;

    for (const FeedbackEntry& entry : wanted) {
        // Chunk residency is fed whichever marcher ran, because the path tracer reads the chunk
        // buffers directly — so with the node pool marching and this left as it was, pressing F4
        // gave an empty world.
        // A used-report is not a request for anything; chunk residency has nothing to do with
        // it, and its level field carries a flag that would shift the coordinate into nonsense.
        if (node_feedback &&
            (entry.level & (kFeedbackUsed | kFeedbackRead | kFeedbackFace)) != 0) {
            continue;
        }

        ChunkCoord coord{entry.x, entry.y, entry.z};
        if (node_feedback) {
            const u32 level = static_cast<u32>(entry.level);
            if (level > 40) continue;
            coord = chunk_coord_of(static_cast<i64>(entry.x) << level,
                                   static_cast<i64>(entry.y) << level,
                                   static_cast<i64>(entry.z) << level);
        }
        if (!world_.has_chunk(coord)) {
            // Reported, but the world has nothing there. A few of these are normal — the
            // grid answers at block granularity, so a ray inside an occupied block still
            // asks about individual chunks that turn out to be empty.
            //
            // A *large* number is a stall, and it used to be a silent one. A ray reports
            // only its nearest miss, so a phantom hides the real chunk behind it forever:
            // streaming asks for the same nothing every frame while the world stays
            // unloaded. It is counted so it can be seen rather than deduced.
            ++rejected;
            continue;
        }
        residency_.request(coord);
        ++accepted;

        // Also pull in the immediate neighbours. A ray reports one chunk, and only the
        // chunks some ray happened to land on would ever be requested — which left visible
        // notches along the edges of what had streamed. Dilating by one face covers the
        // gaps between sample points and costs nothing: neighbours that are already
        // resident are a hit, and ones the world does not have are one lookup.
        const ChunkCoord neighbours[6] = {
            {coord.x - 1, coord.y, coord.z}, {coord.x + 1, coord.y, coord.z},
            {coord.x, coord.y - 1, coord.z}, {coord.x, coord.y + 1, coord.z},
            {coord.x, coord.y, coord.z - 1}, {coord.x, coord.y, coord.z + 1},
        };
        for (const ChunkCoord& neighbour : neighbours) {
            if (world_.has_chunk(neighbour)) residency_.request(neighbour);
        }
    }
    last_feedback_accepted_ = accepted;
    last_feedback_rejected_ = rejected;

    // The same reports, read the node pool's way.
    //
    // A node feedback entry carries the node coordinate at its own level in xyz and the level in
    // w, where a chunk entry carried a chunk coordinate and a detail level it did not use. The two
    // marchers write the same buffer in the same format, so whichever one ran this frame, the
    // other's consumer sees numbers it can make sense of — which is what lets the two be swapped
    // at run time without a second feedback path.
    //
    // Gated on the same thing as the loop above, and for the sharper half of the same reason: a
    // chunk coordinate read as a node key is a request for a node the world does not have, and
    // the pool builds toward it. In the path tracer that spent budget on 488 nodes of nothing.
    u32 faces_seen = 0;
    if (node_feedback) {
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
                bool first_time = false;
                face_store_.claim(FaceKey{entry.x, entry.y, entry.z, level, face}, frame_counter_,
                                  &first_time);
                ++faces_seen;

                // And the coarse face standing over it, which the marcher reads while this one is
                // still being found. Derived here rather than reported, because an ancestor key is
                // a shift of its descendant's and a coordinate computable from another coordinate
                // is not information: sending it would double the face traffic through a buffer
                // that is already the binding constraint, and buy nothing.
                //
                // What it buys instead is the wait. A face is claimed only when a primary ray lands
                // on it, at one pixel in sixty-four, so a surface that was hidden behind something
                // and is now visible has no light of its own for about a second — and the composite
                // falls back to full sun on it, which indoors is the most wrong answer available.
                // Five hundred and twelve fine faces share one stand-in, so the stand-in is claimed
                // the frame the region appears and settled a few frames later, and what a player
                // sees is a blocky shadow sharpening rather than no shadow arriving. R9d.
                //
                // Only when the face under it is NEW, which is the whole reason `was_new` exists.
                // Doing it on every report is 16,000 extra probes a frame that change nothing —
                // measured at 0.24 ms of CPU while turning — and the case they would serve cannot
                // occur: a stand-in is wanted for geometry the store has not seen, and geometry the
                // store has not seen has no repeat reports to hang the claim off. What the repeats
                // would buy is keeping a stand-in warm past its cold window while its children stay
                // live, and a stand-in whose children are all live is a stand-in nothing reads.
                const u32 coarse_level = level + kFaceAncestorStep;
                if (first_time && coarse_level <= kMaxNodeLevel) {
                    face_store_.claim(FaceKey{entry.x >> kFaceAncestorStep,
                                              entry.y >> kFaceAncestorStep,
                                              entry.z >> kFaceAncestorStep, coarse_level, face},
                                      frame_counter_);
                }
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

            const u32 level = static_cast<u32>(entry.level);
            if (level < kLeafLevel || level > kMaxNodeLevel) continue;
            node_pool_.request(NodeKey{entry.x, entry.y, entry.z, level});

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
            for (const NodeKey& adjacent : around) node_pool_.request(adjacent);
        }
    }

    last_faces_seen_ = faces_seen;

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
    face_store_.evict_cold(frame_counter_);

    // A small radius around the camera on top, so the ground under your feet is resident
    // before it has been looked at. Feedback cannot report what has never been on screen.
    // (The window itself is set before the loop above, so requests outside it are already
    // being dropped rather than queued and then discarded.)
    const i64 radius_chunks = 2;
    for (i64 z = -radius_chunks; z <= radius_chunks; ++z) {
        for (i64 y = -1; y <= 1; ++y) {
            for (i64 x = -radius_chunks; x <= radius_chunks; ++x) {
                const ChunkCoord coord{centre.x + x, centre.y + y, centre.z + z};
                if (world_.has_chunk(coord)) residency_.request(coord);
            }
        }
    }
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

    const std::vector<LightSource> lights = build_light_list(
        world_, types_, camera_.chunk_x() * kChunkEdge + static_cast<i64>(camera_.local_x()),
        camera_.chunk_y() * kChunkEdge + static_cast<i64>(camera_.local_y()),
        camera_.chunk_z() * kChunkEdge + static_cast<i64>(camera_.local_z()));

    light_count_ = static_cast<u32>(lights.size());
    if (light_count_ > 0 && light_buffer_.mapped != nullptr) {
        std::memcpy(light_buffer_.mapped, lights.data(), lights.size() * sizeof(LightSource));
    }
    WS_LOG_INFO("light", "{} emitters", light_count_);
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
// Most of these are read fresh every frame — from the parameter block or the push constants —
// so changing a level costs nothing and cannot fail.
//
// Resolution is the exception, and it is the reason this function can be slow. Rendering
// smaller than the window means new images, new descriptors and a new dispatch size, so the
// device has to be idle before the old ones go away. That is a stall of a millisecond or two,
// taken at most once every twenty frames (kFramesToDrop in game/quality.cpp) and only on the
// three rungs where the scale actually changes — 3 to 2, 2 to 1, 1 to 0. Everything above
// level 3 renders at the window size and never pays it. Paying it here is what makes the
// dispatch and the accumulation image genuinely smaller; scaling on presentation alone would
// have traced the same pixels and saved nothing.
//
// This must not be called with a frame already recording — the images it frees are bound to
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
    // CONVERGENCE, never on framerate (§6).
    //
    // A face that has not settled is never held back — a new surface reaches its answer in a
    // handful of frames however busy the store is, and only the refresh rate of settled faces
    // degrades. That is the right thing to give up: a settled face is looking at a sun that has
    // not moved.
    const u32 live = std::max(face_store_.watermark(), 1u);
    push.face_stride = std::max(1u, (live + kFacesPerFrame - 1) / kFacesPerFrame);

    // Where the card may claim faces of its own, and R3e is the whole of why it may.
    push.provisional_base = face_buffers_.provisional_base();
    push.face_first = 0;
    return push;
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
    trace.sun_colour[0] = 3.2f;
    trace.sun_colour[1] = 3.05f;
    trace.sun_colour[2] = 2.75f;
    trace.control[0] = trace_samples_;
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
                                &pathtrace_set_, 1, &trace_offset);
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
    // anything measured in pixels of the *picture* — the dispatch, the parameter block's
    // resolution, the bandwidth counter — has to use this one rather than the window's.
    const VkExtent2D render_extent = render_target_.extent;

    profiler_.begin_frame(cmd, swapchain_.frame_index());
    feedback_.begin_frame(cmd);

    // ---- streaming ------------------------------------------------------------------
    // The node pool, updated and copied before anything reads it. Its own pass, so the cost is
    // visible beside streaming rather than folded into it — the two are alternatives and the
    // whole point of R1 is which of them is cheaper.
    {
        profiler_.begin_pass(cmd, "nodes", 0.8);
        const f64 camera_voxel[3] = {
            static_cast<f64>(camera_.chunk_x()) * 256.0 + camera_.local_x(),
            static_cast<f64>(camera_.chunk_y()) * 256.0 + camera_.local_y(),
            static_cast<f64>(camera_.chunk_z()) * 256.0 + camera_.local_z(),
        };
        const u64 node_start = now_ns();
        const NodeUploadBatch& node_batch = node_pool_.update(world_, camera_voxel, frame_counter_);
        node_ms_ = ns_to_ms(now_ns() - node_start);
        if (node_ms_ > worst_node_ms_) {
            worst_node_ms_ = node_ms_;
            worst_node_frame_ = frame_counter_;
        }
        last_node_built_ = node_batch.built;
        last_node_evicted_ = node_batch.evicted;
        last_node_deferred_ = node_batch.deferred;
        node_buffers_.upload(cmd, node_pool_);
        profiler_.add_bytes(node_buffers_.stats().uploaded_this_frame);
        profiler_.end_pass(cmd);
    }

    profiler_.begin_pass(cmd, "streaming", 0.8);
    stream(static_cast<f64>(time_seconds));

    // The face store's mirror, AFTER the claims rather than before them.
    //
    // It used to be uploaded beside the node pool, which is recorded a few lines above `stream()` —
    // so every face claimed this frame missed this frame's copy and reached the card on the next
    // one. A whole frame of latency on the newest faces, spent on nothing, and invisible because
    // the picture it produces is the one that arrives anyway a frame later. `shade_faces` is
    // dispatched far below this point, so the only thing that ever needed the earlier position was
    // the audit, which reads the CPU's copy and not the card's.
    face_buffers_.upload(cmd, face_store_);
    profiler_.add_bytes(face_buffers_.stats().uploaded_this_frame);
    const u64 residency_start = now_ns();
    const UploadBatch& batch = residency_.update(world_, frame_counter_);
    residency_ms_ = ns_to_ms(now_ns() - residency_start);
    if (residency_ms_ > worst_residency_ms_) worst_residency_ms_ = residency_ms_;
    world_buffers_.upload(cmd, residency_, batch, swapchain_.frame_index());
    // If the occupancy grid did not fit in this frame's staging, ask for it again. Clearing
    // the dirty flag over an upload that never happened is what left holes in the world.
    if (world_buffers_.stats().coarse_incomplete) residency_.mark_coarse_dirty();

    // The other tier. Pushed from the camera rather than pulled from the view, so it never
    // waits on a round trip and cannot deadlock on what a ray did or did not reach.
    {
        const ChunkCoord centre{camera_.chunk_x(), camera_.chunk_y(), camera_.chunk_z()};
        last_thumbs_resident_ = 0;
        last_thumbs_wanted_ = 0;
        last_thumbs_built_ = 0;
        for (u32 level = 0; level < kSummaryTiers; ++level) {
            const ThumbnailBatch& thumbs =
                thumb_tiers_[level].update(world_, centre, frame_counter_);
            if (!world_buffers_.upload_thumbnails(cmd, thumb_tiers_[level], thumbs)) {
                thumb_tiers_[level].defer_last_batch();
            }
            last_thumbs_resident_ += thumb_tiers_[level].resident_count();
            if (options_.stream_log && frame_counter_ % 300 == 0) {
                WS_LOG_INFO("diag", "  level {} holds {} blocks", level,
                            thumb_tiers_[level].resident_count());
            }
            last_thumbs_wanted_ += thumbs.wanted;
            last_thumbs_built_ += thumbs.built;
        }
    }

    // Geometry arriving or leaving means the samples already in the accumulator were taken of
    // a different world, so the average has to start again.
    //
    // Without this the tracer keeps them for ever. The accumulator is a running mean over
    // every sample since the last reset, so a hundred bright frames taken while a wall was
    // still a summary block — drawn as if it stood in daylight — stay in the average once the
    // real wall arrives and the true answer is zero. A sealed box with no lights in it settled
    // at 8 to 16 of 255 rather than black, uniformly, and no amount of waiting cleared it:
    // every later sample was correct and simply diluted the old ones more slowly.
    //
    // This is also why light "did not update properly" anywhere else. Anything streaming in
    // behind you left its stand-in's brightness baked into the picture.
    // Arriving or leaving is a different world; a chunk being EDITED is not.
    //
    // The reasoning above holds when a summary block becomes a real wall: the samples in the
    // accumulator were taken of geometry that was never there, and they have to go. It does not
    // hold for a voxel placed with the chisel. That chunk is refreshed, the reset fires, and the
    // running mean of the entire screen goes back to a single sample — which is a raw path traced
    // sample, which is noise with a colour in it. Reported as blue and cyan artefacts flashing over
    // everything whenever a voxel is placed, settling a moment later.
    //
    // The mean was very nearly right, because one voxel is not a new world. What is wrong after an
    // edit is only how much that mean should be TRUSTED, so the accumulator is demoted rather than
    // emptied: the average stays exactly where it was and a few dozen frames are enough to replace
    // it. See the weight clamp in pt_post.glsl.
    if (path_trace_) {
        if (batch.chunks_added > 0 || batch.chunks_evicted > 0) {
            trace_samples_ = 0;
            edited_recently_ = 0;
        } else if (batch.chunks_refreshed > 0) {
            edited_recently_ = kEditSettleFrames;
        }
    }

    world_buffers_.upload_tables(cmd, types_);
    profiler_.add_bytes(world_buffers_.stats().staged_bytes);

    profiler_.end_pass(cmd);

    {
        const ResidencyStats residency = residency_.stats();
        const WorldBufferStats buffers = world_buffers_.stats();
        StreamingReport report;
        report.world_chunks = world_.chunk_count();
        report.resident_chunks = residency.resident_chunks;
        report.resident_bricks = residency.resident_bricks;
        report.resident_bytes = residency.total_bytes;
        report.payload_in_use = residency.payload_in_use;
        report.payload_capacity = residency.payload_capacity;
        report.staged_bytes = buffers.staged_bytes;
        report.deferred_bytes = buffers.deferred_bytes;
        report.copy_regions = buffers.copy_regions;
        report.raw_regions = buffers.raw_regions;
        report.evictions = residency.evictions;
        report.hit_rate = residency.hit_rate();
        report.out_of_memory = batch.out_of_memory;
        report.update_ms = residency_ms_;
        report.worst_update_ms = worst_residency_ms_;
        report.feedback_reports = last_feedback_;
        report.feedback_dropped = last_feedback_truncated_;
        hud_.set_streaming(report);
        if (options_.stream_log && (frame_counter_ % 60 == 0)) {
            WS_LOG_INFO("diag",
                        "f{} resident {}/{} added {} refreshed {} evicted {} deferred {} "
                        "bricks {} oom {} feedback {} accepted {} phantom {} thumbs {} want {}",
                        frame_counter_, report.resident_chunks, report.world_chunks,
                        batch.chunks_added, batch.chunks_refreshed, batch.chunks_evicted,
                        batch.chunks_deferred, report.resident_bricks,
                        batch.out_of_memory ? 1 : 0, last_feedback_, last_feedback_accepted_,
                        last_feedback_rejected_, last_thumbs_resident_,
                        last_thumbs_wanted_);
        }

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
    // Uploaded only when its *contents* move — a new selection or a rotation. Sliding the
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

    // The face slots, which are not in that loop, because only one of the two marchers writes
    // them. Discard-and-rewrite is right for an image every frame overwrites in full; this one is
    // overwritten in full only while the node pool is marching. Toggle to the chunk grid, or open
    // the path tracer, and the composite would be reading a face index the driver was free to
    // invent. So: transitioned once, and then CLEARED on the frames nothing fills it, which says
    // "no face here" in the one value the composite already knows how to ignore.
    const bool node_writes_faces = use_node_pool_ && !path_trace_;
    if (!face_ready_) {
        image_barrier(cmd, face_image_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
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
    if (!accum_ready_) {
        image_barrier(cmd, accum_image_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        accum_ready_ = true;
    } else if (path_trace_) {
        VkMemoryBarrier2 accum_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        accum_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        accum_barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        accum_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        accum_barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo accum_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        accum_dependency.memoryBarrierCount = 1;
        accum_dependency.pMemoryBarriers = &accum_barrier;
        vkCmdPipelineBarrier2(cmd, &accum_dependency);
    }

    // ---- frame parameters -----------------------------------------------------------
    (void)time_seconds;
    RenderParams params{};

    // Anything that changes what the image *should* be invalidates every sample taken so far,
    // so the average has to start again. Averaging the old view into the new one does not
    // produce a slightly stale picture, it produces a smear that never clears — the samples
    // have equal weight however wrong they are.
    //
    // Compared against the camera as it will be used this frame, not against a "did the player
    // press a key" flag: the camera also moves from momentum and from being placed by script.
    {
        const f32 here[6] = {camera_.local_x(),           camera_.local_y(),
                             camera_.local_z(),           static_cast<f32>(camera_.chunk_x()),
                             static_cast<f32>(camera_.chunk_y()),
                             static_cast<f32>(camera_.chunk_z())};
        f32 forward[3];
        camera_.forward_vector(forward);
        bool moved = false;
        for (u32 i = 0; i < 6; ++i) {
            if (here[i] != trace_camera_[i]) moved = true;
            trace_camera_[i] = here[i];
        }
        for (u32 i = 0; i < 3; ++i) {
            if (forward[i] != trace_forward_[i]) moved = true;
            trace_forward_[i] = forward[i];
        }
        if (moved) trace_samples_ = 0;
    }

    params.origin[0] = camera_.local_x();
    params.origin[1] = camera_.local_y();
    params.origin[2] = camera_.local_z();
    camera_.forward_vector(params.forward);
    camera_.right_vector(params.right);
    camera_.up_vector(params.up);
    params.camera_chunk[0] = static_cast<i32>(camera_.chunk_x());
    params.camera_chunk[1] = static_cast<i32>(camera_.chunk_y());
    params.camera_chunk[2] = static_cast<i32>(camera_.chunk_z());

    // Where the camera was last frame, for the motion blur. Carried in the same space as `origin`,
    // which is relative to the camera's own chunk — so when the player crosses a chunk boundary
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
        // Nought means "trust the accumulator as far as it has earned"; anything else caps it.
        params.motion[3] = (edited_recently_ > 0) ? kEditKeepsWeight : 0.0f;
        if (edited_recently_ > 0) --edited_recently_;

        // The weather. Coverage is what kind of day it is; the time is what moves the decks, and
        // moving the decks is what moves their shadows across the ground.
        params.sky_cloud[0] = cloud_coverage_;
        // In GAME seconds, not real ones. A second at the keyboard is a minute in the world, so
        // the weather has to move sixty times as fast or a cloud takes an in-game hour to cross a
        // field it should cross in a minute — which reads as a painted sky that happens to drift.
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
        params.edit_min[3] = 1;
    }
    params.grid_dims[0] = residency_budget_.grid_width;
    params.grid_dims[1] = residency_budget_.grid_height;
    params.grid_dims[2] = residency_budget_.grid_depth;
    // Clip rays to what is resident, plus a margin.
    //
    // The margin is not slack, it is the mechanism: feedback can only report chunks a ray
    // actually reached, so clipping tightly to the resident set means rays can never
    // discover anything outside it and streaming deadlocks — the first version of this
    // rendered nothing at all for exactly that reason. With a margin the frontier
    // advances by that many chunks per frame until it covers whatever is visible.
    constexpr i64 kExploreMargin = 24;   // chunks, so ~190 m of frontier per frame
    ChunkCoord bounds_lo{};
    ChunkCoord bounds_hi{};
    if (residency_.resident_bounds(bounds_lo, bounds_hi)) {
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
    // against 0.78 on the test scene. The clip is not slack — it is what stops a ray that
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
    params.thumb_dims[0] = static_cast<i32>(thumb_budgets_[0].grid_width);
    params.thumb_dims[1] = static_cast<i32>(thumb_budgets_[0].grid_height);
    params.thumb_dims[2] = static_cast<i32>(thumb_budgets_[0].grid_depth);
    params.thumb_dims[3] = static_cast<i32>(kSummaryTiers);
    for (u32 level = 0; level < kSummaryTiers; ++level) {
        params.thumb_tiers[level][0] = static_cast<i32>(thumb_budgets_[level].grid_offset);
        params.thumb_tiers[level][1] = static_cast<i32>(thumb_budgets_[level].slot_base);
        params.thumb_tiers[level][2] = static_cast<i32>(summary_span(level));
        params.thumb_tiers[level][3] = 0;
    }

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

        u32 box = 0;
        const auto add_box = [&](const i64 lo[3], const i64 hi[3], i32 state,
                                 bool outline = true) {
            if (box >= kMaxPreviewBoxes) return;
            for (int axis = 0; axis < 3; ++axis) {
                params.box_min[box][axis] = static_cast<i32>(lo[axis] - base[axis]);
                params.box_max[box][axis] = static_cast<i32>(hi[axis] - base[axis]);
            }
            params.box_min[box][3] = state;
            // Low byte is the outline flag, next byte is the shell thickness, so the
            // preview can draw the void a hollow placement will leave.
            params.box_max[box][3] = (outline ? 1 : 0) | (static_cast<i32>(hollow_ & 0xFFu) << 8);
            ++box;
        };

        if (toolbelt_.active() == ToolKind::Clipboard) {
            const ClipboardPreview& ghost = clipboard_.preview();
            if (ghost.selecting) {
                add_box(ghost.select_min, ghost.select_max, ghost.too_large ? 3 : 1);
            }
            // State 5: march the clip inside this box rather than outlining it, so the
            // ghost shows the voxels that are about to land instead of the space they will
            // land in. Falls back to an outline when the clip is too large to upload.
            for (u32 n = 0; n < ghost.instances && n < kMaxPreviewInstances; ++n) {
                const u32 shape = ghost.shape[n];
                const bool voxel_ghost = shape < clip_slots_.size();
                // Only the last copy — the one the wheel is steering — gets an outline.
                // Sixteen wireframes is both noise to look at and six plane tests a pixel
                // each; the voxels are what says where the others are.
                const bool outline = !voxel_ghost || (n + 1 == ghost.instances);
                const u32 index = box;
                add_box(ghost.min[n], ghost.max[n], voxel_ghost ? 5 : 2, outline);
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
            // single colour to outline it in. Both tints stay at zero, which the shader
            // reads as "invert the backdrop" — the neutral answer, and the one that reads
            // over anything.
        } else {
            const ChiselPreview& preview = chisel_.preview();
            if (preview.active) {
                // Idle — just the voxel under the crosshair — is not a decision yet, so it
                // gets its own state and stays neutral.
                add_box(preview.min, preview.max,
                        (preview.mode == ChiselMode::Carve)
                            ? 1
                            : ((preview.mode == ChiselMode::Place) ? 2 : 4));

                if (preview.mode == ChiselMode::Place) {
                    // The material's own colour in the open, its inverse where it is buried.
                    set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                    set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
                } else if (preview.mode == ChiselMode::Carve && preview.removing != kAir) {
                    // Inverted backdrop in the open (w stays 0), the doomed material where
                    // it is buried.
                    set_tint(params.tint_occluded, types_.visual_of(preview.removing), false);
                }
                // Everything else leaves both at zero, which the shader reads as "invert
                // the backdrop" — the right answer when there is no material to speak of.
            }
        }
        if (!options_.preview.empty()) {
            i64 values[7]{0, 0, 0, 0, 0, 0, 2};
            parse_numbers(options_.preview, values, 7);
            const i64 lo[3] = {values[0], values[1], values[2]};
            const i64 hi[3] = {values[3], values[4], values[5]};
            box = 0;   // the scripted box replaces whatever the tool wanted to show
            add_box(lo, hi, static_cast<i32>(values[6]));
            // Same colour rules as the live tool, so a scripted screenshot shows what a
            // player would see rather than a stand-in.
            if (values[6] == 2 && !materials_.empty()) {
                set_tint(params.tint_visible, types_.visual_of(chisel_.material()), false);
                set_tint(params.tint_occluded, types_.visual_of(chisel_.material()), true);
            } else if (values[6] == 1 && !materials_.empty()) {
                set_tint(params.tint_occluded, types_.visual_of(materials_[0]), false);
            }
        }
        u32 slot = 0;
        for (const std::array<i64, 3>& point : chisel_.constraints()) {
            if (slot >= kMaxPreviewMarks) break;
            params.marks[slot][0] = static_cast<i32>(point[0] - base[0]);
            params.marks[slot][1] = static_cast<i32>(point[1] - base[1]);
            params.marks[slot][2] = static_cast<i32>(point[2] - base[2]);
            params.marks[slot][3] = 1;
            ++slot;
        }
    }

    // The slot stride is the device's alignment, not sizeof — the dynamic offset passed at
    // bind time uses the aligned stride, so writing at sizeof would put frame 1's data
    // where the shader is not looking.
    std::memcpy(static_cast<u8*>(params_buffer_.mapped) +
                    static_cast<usize>(swapchain_.frame_index()) * params_stride_,
                &params, sizeof(RenderParams));

    // ---- reference path tracer ------------------------------------------------------
    //
    // Replaces both passes below while it is on, and shares nothing with them but the world.
    // It has no budget: it accumulates while you hold still and is expected to take seconds.
    if (path_trace_) {
        // Clearing the cache is what "the world changed" means to it. A face's cached light
        // describes a world that no longer exists the moment something is carved next to it,
        // and unlike screen-space accumulation it would never wash out on its own — a stale
        // face keeps its old light until something evicts it, which is never.
        if (face_cache_dirty_) {
            vkCmdFillBuffer(cmd, face_cache_.buffer, 0, VK_WHOLE_SIZE, 0);
            VkMemoryBarrier2 clear_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            clear_barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            clear_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            clear_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            clear_barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            VkDependencyInfo clear_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            clear_dependency.memoryBarrierCount = 1;
            clear_dependency.pMemoryBarriers = &clear_barrier;
            vkCmdPipelineBarrier2(cmd, &clear_dependency);
            face_cache_dirty_ = false;
        }

        // Carry this frame's brightness forward and start a fresh count.
        //
        // Slot 1 is given whatever slot 0 finished the last traced frame as, and only then is
        // slot 0 zeroed — so while the shader adds into an empty slot 0 it can read a whole,
        // still slot 1 and expose for it. Doing it here rather than at the end of the frame
        // means one place to look and no dependence on where a frame is considered to end.
        {
            auto memory_barrier = [cmd](VkPipelineStageFlags2 src_stage, VkAccessFlags2 src,
                                        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst) {
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask = src_stage;
                barrier.srcAccessMask = src;
                barrier.dstStageMask = dst_stage;
                barrier.dstAccessMask = dst;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
            };

            constexpr VkDeviceSize kSlot = sizeof(FrameStatistics);
            memory_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            if (frame_stats_zeroed_) {
                VkBufferCopy carry{};
                carry.srcOffset = 0;
                carry.dstOffset = kSlot;
                carry.size = kSlot;
                vkCmdCopyBuffer(cmd, frame_stats_.buffer, frame_stats_.buffer, 1, &carry);
                // The fill overwrites what the copy just read, which is a hazard the hardware
                // will not spot on its own.
                memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdFillBuffer(cmd, frame_stats_.buffer, 0, kSlot, 0);
            } else {
                // Nothing has been measured yet, so there is no previous frame worth keeping
                // and both slots go to zero. Device memory arrives uninitialised, and a first
                // frame exposed for whatever was left in it would flash.
                vkCmdFillBuffer(cmd, frame_stats_.buffer, 0, VK_WHOLE_SIZE, 0);
                frame_stats_zeroed_ = true;
            }
            memory_barrier(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        }

        profiler_.begin_pass(cmd, "pathtrace", 1000.0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pathtrace_.pipeline());
        const u32 trace_offset =
            static_cast<u32>(swapchain_.frame_index()) * static_cast<u32>(params_stride_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pathtrace_.layout(), 0, 1,
                                &pathtrace_set_, 1, &trace_offset);

        TracePush trace = make_trace_push();
        vkCmdPushConstants(cmd, pathtrace_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(TracePush), &trace);

        dispatch_clouds(cmd, trace, render_extent, trace_offset);
        // The tracer's own pipeline back, since the cloud pass bound its own over it.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pathtrace_.pipeline());


        vkCmdDispatch(cmd, (render_extent.width + 7) / 8, (render_extent.height + 7) / 8, 1);
        profiler_.end_pass(cmd);
        ++trace_samples_;

        VkMemoryBarrier2 trace_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        trace_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        trace_barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        trace_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        trace_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo trace_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        trace_dependency.memoryBarrierCount = 1;
        trace_dependency.pMemoryBarriers = &trace_barrier;
        vkCmdPipelineBarrier2(cmd, &trace_dependency);
    }

    // ---- primary visibility ---------------------------------------------------------
    if (!path_trace_) {
    profiler_.begin_pass(cmd, "visibility", 9.5);
    const u32 params_offset =
        static_cast<u32>(swapchain_.frame_index()) * static_cast<u32>(params_stride_);
    if (use_node_pool_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, node_visibility_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, node_visibility_.layout(), 0,
                                1, &node_set_, 1, &params_offset);
        // The entry table's size and how far a probe may run. A push constant rather than a
        // field in the parameter block, because it belongs to this pipeline and nothing else
        // reads it — and because the block is already at the size AMD gives (128 bytes) once.
        const NodePush node_constants = make_node_push(0);
        vkCmdPushConstants(cmd, node_visibility_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(node_constants), &node_constants);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visibility_.layout(), 0, 1,
                                &descriptor_set_, 1, &params_offset);
    }

    vkCmdDispatch(cmd, (render_extent.width + 7) / 8, (render_extent.height + 7) / 8, 1);
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
    if (use_node_pool_) {
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
        if (face_count > 0 || provisional_count > 0) {
            profiler_.begin_pass(cmd, "faces", 4.4);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shade_faces_.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shade_faces_.layout(), 0,
                                    1, &node_set_, 1, &params_offset);
            if (face_count > 0) {
                const NodePush shade_push = make_node_push(face_count);
                vkCmdPushConstants(cmd, shade_faces_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(shade_push), &shade_push);
                vkCmdDispatch(cmd, (face_count + 63) / 64, 1, 1);
            }

            // And the card's own faces, as a second dispatch rather than a longer first one.
            //
            // They sit in the tail of the same array, above `max_faces`, while the store's
            // watermark is far below it — so one dispatch spanning both would be a million
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
    }   // !path_trace_

    // Hand this frame's "what I could not find" list back to the CPU. Without this the
    // shader's report is written and then thrown away, and streaming never learns
    // anything — which is exactly what happened until it was noticed.
    feedback_.end_frame(cmd, swapchain_.frame_index());

    // ---- present ------------------------------------------------------------------
    image_barrier(cmd, render_target_.image, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, swapchain_.current_image(), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);

    // This is also where a scaled render is put back to the size of the window: source is the
    // render target at whatever the quality level chose, destination is the whole swapchain
    // image, and the filter below does the stretching. Nothing else in the frame needs to know.
    profiler_.begin_pass(cmd, "blit", 0.4);
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
    profiler_.end_pass(cmd);

    // Leave the render target in GENERAL. A frame that ends with an image in a transfer
    // layout is a trap for anything that reads it afterwards — which is exactly what the
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
}

int Application::run(const Options& options) {
    options_ = options;

    const std::string title = std::string("WorldShaper v") + kVersion;
    if (!window_.create(title, options_.width, options_.height,
                        options_.size_explicit)) {
        return 1;
    }
    if (!device_.create(&window_, options_.validation)) return 1;
    {
        // Which card and which driver, in every crash report from here on. A fault that only
        // happens on one machine is answerable; a fault on "a PC" is not.
        const DeviceCapabilities& caps = device_.caps();
        crash_set_context(
            "gpu", std::format("{} (vendor 0x{:04X}, driver {}.{}.{}, {} MB)", caps.name,
                               caps.vendor_id, VK_API_VERSION_MAJOR(caps.driver_version),
                               VK_API_VERSION_MINOR(caps.driver_version),
                               VK_API_VERSION_PATCH(caps.driver_version),
                               caps.device_local_bytes >> 20));
    }
    if (!swapchain_.create(device_, window_.width(), window_.height(), options_.vsync)) {
        return 1;
    }
    if (!profiler_.create(device_)) return 1;

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
    if (loading_quit_) return 0;

    // Everything from here to the first frame is the part a progress bar normally leaves out, and
    // leaving it out is exactly why bars sit at ninety-nine per cent: the residency tables, the
    // summary tree, the three pipelines and their shader compiles are all real time, and none of
    // it is "the world building". So the bar keeps running through it, with a frame drawn between
    // each step. The last stage in the list is the first frame that can actually be shown, which
    // is what makes a hundred per cent mean the game is up rather than nearly up.
    progress_.enter(LoadStage::Uploading);
    draw_loading();

    // Sized from detected VRAM, and never resized afterwards
    // (documentation/03-voxel-data-model.md Â§8).
    const u64 vram = device_.caps().device_local_bytes;

    // A share of the card, rather than a step function that stops caring at eight gigabytes.
    //
    // It was 1 GB for anything with 8 GB or more, and that ceiling is what a sixteen-gigabyte
    // card got: six per cent of it. The facility is 86 chunks and 460 MB of payload held 54 of
    // them, so a third of the building could not be resident at once and residency spent every
    // frame swapping which third — which is a world that flickers while you stand still.
    //
    // Half the card, less a fixed floor for everything that is not brick payload: the render
    // targets, the face cache (256 MB), the type tables, the summary thumbnails, and whatever
    // the compositor and the driver want. A card is not ours alone and a build that takes all
    // of it is a build that stutters against everything else on the desktop.
    //
    // Small cards keep their old shares, because the reasoning that produced them was about
    // fitting at all rather than about generosity.
    const u64 reserved = 1536ull << 20;
    const u64 vram_budget =
        (vram >= (8ull << 30))
            ? std::max<u64>(1ull << 30, (vram > reserved) ? (vram - reserved) / 2 : (1ull << 30))
        : (vram >= (4ull << 30)) ? (384ull << 20)
                                 : (192ull << 20);

    // A brick costs two separate things, and they have to be budgeted separately.
    //
    //   its **payload** — the packed voxel indices and palette, anywhere from 8 bytes for a
    //   uniform brick to 2 KB for one where every voxel differs
    //   its **slot** — a header and a 64-byte occupancy mask, the same for every brick
    //
    // The slot count used to be derived from the payload budget as `payload / 1024`, on the
    // assumption that a brick averages about a kilobyte. That assumption fails in exactly the
    // case that matters: a large flat build is almost entirely *uniform* bricks, which cost
    // eight bytes of payload each and a full slot each. Filling a 3 km square with one
    // material used 8 MB of a 1 GB payload budget and ran clean out of slots at 128 chunks —
    // whereupon residency evicted a chunk for every chunk it added, and the world blinked in
    // and out as you moved. The payload gauge said 1% and everything looked fine.
    //
    // So: split the budget, and size the slots by what a slot actually costs.
    //
    // Thumbnails take a slice off the top, because they buy something the other two cannot
    // buy at any price. Full detail is bounded by memory however it is split: a chunk has to
    // be entirely resident to draw at all, so past that bound the world is not drawn coarsely,
    // it is simply not drawn. A thumbnail is two kilobytes, so the same memory that holds a
    // few hundred chunks at full detail holds tens of thousands of them at a metre — which is
    // the difference between a view that ends and one that does not.
    constexpr u64 kSlotBytes = sizeof(GpuBrickHeader) + kBrickWords * sizeof(u64);
    // What the chunk system gets, which since the node pool became the marcher is "enough to
    // serve the path tracer" rather than "most of the card".
    //
    // Nothing else reads it. `visibility.comp` is behind --chunk-marcher, and `pathtrace.comp` is
    // the only thing left that includes world.glsl. Sized for the whole card it costs **1,432 ms
    // of chunk residency and 266 ms of thumbnail tiers on every single load** -- 83% of a 2,033 ms
    // start -- almost all of it zeroing pools that the frame never touches, plus about 12 ms of
    // CPU a frame keeping them current.
    //
    // A tenth of the share is still hundreds of megabytes, which is a reference path tracer's
    // working set on one scene and is what it had before any of this existed. R1e deletes the rest
    // of it; this stops it being paid for by everybody in the meantime.
    const u64 chunk_share = options_.path_trace ? 100 : 10;
    const u64 thumb_bytes = vram_budget * 15 / 100 * chunk_share / 100;
    residency_budget_.payload_bytes = vram_budget * 45 / 100 * chunk_share / 100;
    residency_budget_.max_bricks =
        static_cast<u32>((vram_budget * 40 / 100 * chunk_share / 100) / kSlotBytes);
    residency_budget_.max_chunk_uploads_per_frame = 8;
    residency_budget_.max_bricks_per_frame = 8192;
    const u64 t_residency = now_ns();
    residency_.create(residency_budget_, types_);
    WS_LOG_INFO("load", "chunk residency {:.0f} ms  [t+{:.0f} ms]",
                ns_to_ms(now_ns() - t_residency), ns_to_ms(now_ns() - load_began_ns_));
    progress_.within(0.15);
    draw_loading();

    // One cache per level of the summary octree, all sharing one pair of GPU buffers at
    // their own base offsets. Half the memory goes to level 0 and each level above halves
    // again — the area a level covers grows as the square of its reach, and its cost per
    // unit of area falls by eight, so the far levels reach enormously further for very
    // little. The reach printed below is what that works out to.
    constexpr u64 kThumbBytes = kThumbSlotWords * sizeof(u32);
    summary_tree_.create(types_);

    u32 slot_base = 0;
    u32 grid_offset = 0;
    f64 tier_ms = 0.0;
    u64 share = thumb_bytes / 2;
    for (u32 level = 0; level < kSummaryTiers; ++level) {
        ThumbnailBudget budget;
        budget.level = level;
        budget.slot_base = slot_base;
        budget.grid_offset = grid_offset;
        budget.max_thumbs = static_cast<u32>(std::max<u64>(2048, share / kThumbBytes));
        budget.radius_chunks = 96;   // in blocks, so 2^level times further each level up
        budget.max_builds_per_frame = (level == 0) ? 32u : 8u;
        thumb_budgets_[level] = budget;
        const u64 t_tier = now_ns();
        thumb_tiers_[level].create(budget, summary_tree_);
        tier_ms += ns_to_ms(now_ns() - t_tier);

        slot_base += budget.max_thumbs;
        grid_offset += budget.grid_width * budget.grid_height * budget.grid_depth;
        if (level + 1 < kSummaryTiers) share /= 2;

        WS_LOG_INFO("app", "summary level {}: {} chunks/block, {} slots, {} MB, reach {:.1f} km",
                    level, summary_span(level), budget.max_thumbs,
                    (static_cast<u64>(budget.max_thumbs) * kThumbBytes) >> 20,
                    static_cast<f64>(budget.radius_chunks * summary_span(level) * 8) / 1000.0);
    }
    WS_LOG_INFO("load", "thumbnail tiers {:.0f} ms  [t+{:.0f} ms]", tier_ms,
                ns_to_ms(now_ns() - load_began_ns_));
    thumb_total_slots_ = slot_base;
    thumb_total_grid_ = grid_offset;

    // Coarse occupancy comes from the world, so the marcher can tell "nothing here" from
    // "something here that you have not streamed yet". Without it, feedback never fires.
    rebuild_coarse_grids();
    progress_.within(0.45);
    draw_loading();
    const u64 t_world_buffers = now_ns();
    if (!world_buffers_.create(device_, residency_budget_, thumb_total_slots_, thumb_total_grid_,
                               32ull << 20)) {
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

    // Four million faces at 32 bytes, so 128 MB.
    //
    // A million was enough until parent seeding arrived. Seeding keeps an entry for a node
    // *and* its parent, so the table holds two levels for everything it sees, and that extra
    // pressure took uncached surface pixels from 3,661 to 18,400 — a band of noise back along
    // the skyline, traded for the halved first-look noise seeding buys. This pays for both.
    //
    // A face that cannot find a slot is not wrong, it just goes uncached and noisy, and that
    // failure is invisible until someone wonders why one wall is grainier than the rest —
    // which is what debug view 5 is for.
    // Doubled again when the sun moved into entries of its own. Two kinds of entry per surface
    // instead of one is more pressure on the same table, and the table answers pressure by
    // refusing slots: refusals in an enclosed room went from 4.6% of surface pixels to 12.2%,
    // and a refused face has no light at all, so they show up as grainy patches the size of
    // whatever region saturated. Lengthening the probe instead reached 7% and cost a fifth of
    // the frame; the memory is the cheaper answer.
    // Sized from VRAM, like the world buffers, rather than fixed. A quarter of a gigabyte is
    // the right answer on a card with sixteen and plainly the wrong one on a handheld with
    // four, and a table that cannot be allocated is worse than a smaller one: a refused face
    // is noisy, an absent buffer is nothing at all.
    const u64 face_vram = device_.caps().device_local_bytes;
    const u64 kFaceCacheEntries = (face_vram >= (8ull << 30))   ? (8ull << 20)
                                  : (face_vram >= (4ull << 30)) ? (4ull << 20)
                                                                : (2ull << 20);
    face_cache_ = create_device_buffer(device_, kFaceCacheEntries * 32,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "face cache");

    // Where the lamps are, so a shadow ray can be aimed at one. Small enough to be a staging
    // buffer written straight from the CPU: a thousand lights is 28 KB and it only changes
    // when the world does.
    light_buffer_ = create_staging_buffer(device_, kMaxLights * sizeof(LightSource),
                                          "light list", VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    WS_LOG_INFO("app", "face cache: {} entries, {} MB", kFaceCacheEntries,
                (kFaceCacheEntries * 32) >> 20);

    // Thirty-two bytes for the whole frame's brightness. TRANSFER_SRC on top of the storage
    // usage because this frame's slot is copied over the previous one before it is zeroed, and
    // create_device_buffer only asks for TRANSFER_DST.
    frame_stats_ = create_device_buffer(
        device_, sizeof(FrameStatistics) * kFrameStatsSlots,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        "frame statistics");

    const u64 clip_bytes = kMaxClipPoolCells * sizeof(u32);
    clip_buffer_ = create_device_buffer(device_, clip_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        "clip cells");
    clip_staging_ = create_staging_buffer(device_, clip_bytes, "clip staging");

    // Visibility set: bindings 0-1 are the output images; 2-8 are the world, in the order
    // the marcher walks them. See shaders/visibility.comp.
    constexpr u32 kImageBindings = 2;
    constexpr u32 kBufferBindings = 11;   // 10 world buffers plus the feedback buffer
    VkDescriptorSetLayoutBinding bindings[kImageBindings + kBufferBindings + 1]{};
    for (u32 i = 0; i < kImageBindings + kBufferBindings + 1; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            (i < kImageBindings) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : (i < kImageBindings + kBufferBindings)
                ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = kImageBindings + kBufferBindings + 1;
    layout_info.pBindings = bindings;
    WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &layout_info, nullptr, &set_layout_));

    // Resolve set: the visibility image in, the colour image out, plus the two tables it
    // needs to turn a voxel type into a colour.
    // Seven, not six: the last is the cloud history at kCloudBinding, which this pass reads so the
    // sky it draws is the same sky the path tracer draws. Binding numbers need not be contiguous,
    // and using the same number as the tracer means shaders/pt_clouds.glsl and its consumers do not
    // have to care which pass included them.
    // Nine now: 6 is the face store and 7 is the face-slot image, which together are how the
    // picture stops lighting itself per pixel and starts reading light off the surface.
    VkDescriptorSetLayoutBinding resolve_bindings[9]{};
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

    VkDescriptorSetLayoutCreateInfo resolve_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    resolve_layout_info.bindingCount = 9;
    resolve_layout_info.pBindings = resolve_bindings;
    WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &resolve_layout_info, nullptr,
                                      &resolve_layout_));

    const VkDescriptorPoolSize pool_sizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        // The three sets between them bind the world twice, the type tables, the clip, the
        // face cache and the light list. Counted generously: running out of pool is a failure
        // at start-up with a message nobody connects to the binding they just added.
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 48},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    WS_VK(vkCreateDescriptorPool(device_.handle(), &pool_info, nullptr, &descriptor_pool_));

    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &set_layout_;
    WS_VK(vkAllocateDescriptorSets(device_.handle(), &set_alloc, &descriptor_set_));

    VkDescriptorSetAllocateInfo resolve_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    resolve_alloc.descriptorPool = descriptor_pool_;
    resolve_alloc.descriptorSetCount = 1;
    resolve_alloc.pSetLayouts = &resolve_layout_;
    WS_VK(vkAllocateDescriptorSets(device_.handle(), &resolve_alloc, &resolve_set_));

    // The path tracer's own set, allocated here with the others because create_render_target
    // writes image descriptors into all three and cannot write into a set that does not exist
    // yet. Its two images are 0 and 1, then the same world bindings the marcher uses — it
    // includes the same traversal, so the binding numbers come with it — then the parameter
    // block at 13 and the type tables at 14 and 15.
    {
        // Twenty-one: the two images, the world, the parameter block, the type tables, the clip,
        // the face cache, the light list at 18, the frame statistics at 19, and the cloud buffer
        // at 20 — which the cloud pass writes and this one reads. They share this layout whole,
        // because the cloud pass needs the parameter block and the sun and nothing else, and a set
        // of its own would be the same set with holes in it.
        constexpr u32 kTraceBindings = kCloudMarchedBinding + 1;
        VkDescriptorSetLayoutBinding trace_bindings[kTraceBindings]{};
        for (u32 i = 0; i < kTraceBindings; ++i) {
            trace_bindings[i].binding = i;
            trace_bindings[i].descriptorType =
                (i < 2 || i >= kCloudBinding) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : (i == 13)                   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            // Two at the cloud binding: the history is read and written alternately, so both are
            // bound and the shader picks by parity rather than the descriptors being rewritten.
            trace_bindings[i].descriptorCount = (i == kCloudBinding) ? 2 : 1;
            trace_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo trace_layout{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        trace_layout.bindingCount = kTraceBindings;
        trace_layout.pBindings = trace_bindings;
        WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &trace_layout, nullptr,
                                          &pathtrace_layout_));

        VkDescriptorSetAllocateInfo trace_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        trace_alloc.descriptorPool = descriptor_pool_;
        trace_alloc.descriptorSetCount = 1;
        trace_alloc.pSetLayouts = &pathtrace_layout_;
        WS_VK(vkAllocateDescriptorSets(device_.handle(), &trace_alloc, &pathtrace_set_));
    }

    {
        const VkExtent2D render = scaled_extent();
        create_render_target(render.width, render.height);
    }

    // The three pipelines, which is where the rest of the wait lives: a path tracer is a large
    // shader and compiling it is seconds, not milliseconds. This is the last stage, so it is also
    // the one that has to be included or the bar reaches its end and the screen stays up anyway.
    progress_.enter(LoadStage::Settling);
    draw_loading();

    // Compiled shaders sit beside the executable, and *beside* means beside the one that is
    // running — asked at run time, not baked in at build time. The source tree location
    // still comes from the build, because hot reload only ever runs where the source is.
    const std::filesystem::path shaders = compiled_shader_dir();
    const std::filesystem::path spirv = shaders / "visibility.comp.spv";
    const std::filesystem::path source =
        std::filesystem::path(WS_SHADER_SOURCE_DIR) / "visibility.comp";

    // The node pool, its buffers, its descriptor set and its pipeline. Created unconditionally
    // rather than behind the flag: the point of R1c is that the two marchers can be swapped at
    // run time and diffed on one camera, and a path that is only built when it is asked for is a
    // path nobody notices has stopped compiling.
    {
        NodePoolBudget node_budget;
        WS_LOG_INFO("load", "world buffers {:.0f} ms  [t+{:.0f} ms]",
                ns_to_ms(now_ns() - t_world_buffers), ns_to_ms(now_ns() - load_began_ns_));
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
        face_store_.create(face_budget);
        if (!face_buffers_.create(device_, face_budget)) {
            WS_LOG_FATAL("app", "could not create the face store buffers");
            return 1;
        }
        WS_LOG_INFO("load", "node buffers {:.0f} ms  [t+{:.0f} ms]",
                    ns_to_ms(now_ns() - t_node_buffers), ns_to_ms(now_ns() - load_began_ns_));

        // 0-1 out images, 2-6 the pool, 7 feedback, 8 the parameter block, 9-10 the faces.
        //
        // One set for both the marcher and the face shader. They need the same tree — the shading
        // pass marches shadow rays through exactly the geometry the primary ray stopped on — and
        // two sets would be two places for the same buffers to be bound, which is how the node
        // pipeline's output images came to be written in one place and forgotten in the other
        // (§4 trap 1). A shader need not use every binding in a set.
        // Thirteen: 0-1 the visibility and depth images, 2-7 the pool and feedback, 8 the parameter
        // block, 9-10 the face store, 11 the face-slot image the composite reads, 12 the card's own
        // provisional face table (R3e), which is the store's shape minus the host.
        VkDescriptorSetLayoutBinding node_bindings[13]{};
        for (u32 i = 0; i < 13; ++i) {
            node_bindings[i].binding = i;
            node_bindings[i].descriptorType =
                (i < 2 || i == 11) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : (i == 8)         ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                   : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            node_bindings[i].descriptorCount = 1;
            node_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo node_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        node_layout_info.bindingCount = 13;
        node_layout_info.pBindings = node_bindings;
        WS_VK(vkCreateDescriptorSetLayout(device_.handle(), &node_layout_info, nullptr,
                                          &node_layout_));

        VkDescriptorSetAllocateInfo node_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        node_alloc.descriptorPool = descriptor_pool_;
        node_alloc.descriptorSetCount = 1;
        node_alloc.pSetLayouts = &node_layout_;
        WS_VK(vkAllocateDescriptorSets(device_.handle(), &node_alloc, &node_set_));

        const VkBuffer node_pool_buffers[9]{
            node_buffers_.entries(), node_buffers_.nodes(),     node_buffers_.leaves(),
            node_buffers_.occupancy(), node_buffers_.payload(), feedback_.buffer(),
            face_buffers_.faces(), face_buffers_.entries(),    face_buffers_.provisional(),
        };
        VkDescriptorBufferInfo node_infos[9]{};
        VkWriteDescriptorSet node_writes[10]{};
        for (u32 i = 0; i < 9; ++i) {
            node_infos[i].buffer = node_pool_buffers[i];
            node_infos[i].offset = 0;
            node_infos[i].range = VK_WHOLE_SIZE;
            node_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            node_writes[i].dstSet = node_set_;
            // 2..7 are the pool and the feedback buffer; 8 is the parameter block, which is a
            // different descriptor type and is written separately below; 9 and 10 are the faces,
            // and 12 is the provisional table, which skips 11 because that is an image.
            node_writes[i].dstBinding = (i < 6) ? (2 + i) : (i == 8) ? 12 : (3 + i);
            node_writes[i].descriptorCount = 1;
            node_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            node_writes[i].pBufferInfo = &node_infos[i];
        }
        VkDescriptorBufferInfo node_params{};
        node_params.buffer = params_buffer_.buffer;
        node_params.offset = 0;
        node_params.range = sizeof(RenderParams);
        node_writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        node_writes[9].dstSet = node_set_;
        node_writes[9].dstBinding = 8;
        node_writes[9].descriptorCount = 1;
        node_writes[9].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        node_writes[9].pBufferInfo = &node_params;
        vkUpdateDescriptorSets(device_.handle(), 10, node_writes, 0, nullptr);

        // And the two output images, which the render target owns. It was created before this
        // set existed, so its own binding pass skipped them.
        VkDescriptorImageInfo node_vis{};
        node_vis.imageView = visibility_image_.view;
        node_vis.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo node_depth{};
        node_depth.imageView = depth_target_.view;
        node_depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet node_images[3]{};
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
        vkUpdateDescriptorSets(device_.handle(), 3, node_images, 0, nullptr);

        const std::filesystem::path node_spirv = shaders / "node_visibility.comp.spv";
        const std::filesystem::path node_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "node_visibility.comp";
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

        // The same push range as the face shader, because they include the same file and a
        // stage may declare only one block: the marcher writes the first two fields and ignores
        // the rest, but its layout has to reserve what the block declares.
        if (!node_visibility_.create(device_, node_source, node_spirv, node_layout_,
                                     sizeof(NodePush))) {
            WS_LOG_FATAL("app", "could not create the node visibility pipeline: {}",
                         node_visibility_.last_error());
            return 1;
        }
        use_node_pool_ = options_.node_pool;
    }

    const u64 t_pipelines = now_ns();
    if (!visibility_.create(device_, source, spirv, set_layout_, 0)) {
        WS_LOG_FATAL("app", "could not create the visibility pipeline: {}",
                     visibility_.last_error());
        return 1;
    }

    progress_.within(0.25);
    draw_loading();

    const std::filesystem::path resolve_spirv = shaders / "resolve.comp.spv";
    const std::filesystem::path resolve_source =
        std::filesystem::path(WS_SHADER_SOURCE_DIR) / "resolve.comp";
    // The same push constant the tracer takes. It carries the sun, the weather and the air, none
    // of which this pass could see before — which is why it drew a hardcoded gradient.
    if (!resolve_.create(device_, resolve_source, resolve_spirv, resolve_layout_,
                         sizeof(TracePush))) {
        WS_LOG_FATAL("app", "could not create the resolve pipeline: {}",
                     resolve_.last_error());
        return 1;
    }

    progress_.within(0.40);
    draw_loading();

    {
        const std::filesystem::path trace_spirv = shaders / "pathtrace.comp.spv";
        const std::filesystem::path trace_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "pathtrace.comp";
        // The cloud volume, on the tracer's own set and push constants.
        const std::filesystem::path cloud_spirv = shaders / "clouds.comp.spv";
        const std::filesystem::path cloud_source =
            std::filesystem::path(WS_SHADER_SOURCE_DIR) / "clouds.comp";
        if (!clouds_.create(device_, cloud_source, cloud_spirv, pathtrace_layout_,
                            sizeof(TracePush))) {
            // Not fatal. A sky with no cloud in it is a worse picture and a working one.
            WS_LOG_ERROR("app", "no clouds this run: {}", clouds_.last_error());
        }

        WS_LOG_INFO("load", "pipelines before the tracer {:.0f} ms",
                    ns_to_ms(now_ns() - t_pipelines));
        const u64 t_tracer = now_ns();
        struct TracerTimer {
            u64 began;
            u64 origin;
            ~TracerTimer() { WS_LOG_INFO("load", "path tracer pipeline {:.0f} ms  [t+{:.0f} ms]",
                                         ns_to_ms(now_ns() - began),
                                         ns_to_ms(now_ns() - origin)); }
        } tracer_timer{t_tracer, load_began_ns_};
        if (!pathtrace_.create(device_, trace_source, trace_spirv, pathtrace_layout_,
                               sizeof(TracePush))) {
            WS_LOG_FATAL("app", "could not create the path tracing pipeline: {}",
                         pathtrace_.last_error());
            return 1;
        }
    }

    // The world buffers never move, so they are bound once.
    const VkBuffer marcher_buffers[]{
        world_buffers_.grid(),     world_buffers_.records(),   world_buffers_.masks(),
        world_buffers_.prefixes(), world_buffers_.headers(),   world_buffers_.occupancy(),
        world_buffers_.payload(), world_buffers_.coarse(), world_buffers_.thumb_grid(),
        world_buffers_.thumbs(), feedback_.buffer(),
    };
    static_assert(kBufferBindings == 11, "marcher buffer list must match the binding count");
    // Four now: the face store joins them, because the composite reads light off a face rather
    // than working it out per pixel.
    const VkBuffer resolve_buffers[]{world_buffers_.types(), world_buffers_.visuals(),
                                     clip_buffer_.buffer, face_buffers_.faces()};

    // The parameter block, bound to both sets with a dynamic offset chosen per frame.
    VkDescriptorBufferInfo params_info{};
    params_info.buffer = params_buffer_.buffer;
    params_info.offset = 0;
    params_info.range = sizeof(RenderParams);
    VkWriteDescriptorSet params_writes[2]{};
    for (u32 i = 0; i < 2; ++i) {
        params_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        params_writes[i].dstSet = (i == 0) ? descriptor_set_ : resolve_set_;
        params_writes[i].dstBinding = (i == 0) ? (kImageBindings + kBufferBindings) : 4;
        params_writes[i].descriptorCount = 1;
        params_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        params_writes[i].pBufferInfo = &params_info;
    }
    vkUpdateDescriptorSets(device_.handle(), 2, params_writes, 0, nullptr);

    constexpr u32 kResolveBuffers = 4;   // types, visuals, clip cells, the face store
    VkDescriptorBufferInfo buffer_infos[kBufferBindings + kResolveBuffers]{};
    VkWriteDescriptorSet buffer_writes[kBufferBindings + kResolveBuffers]{};
    for (u32 i = 0; i < kBufferBindings + kResolveBuffers; ++i) {
        const bool marcher = (i < kBufferBindings);
        buffer_infos[i].buffer = marcher ? marcher_buffers[i] : resolve_buffers[i - kBufferBindings];
        buffer_infos[i].offset = 0;
        buffer_infos[i].range = VK_WHOLE_SIZE;
        buffer_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        buffer_writes[i].dstSet = marcher ? descriptor_set_ : resolve_set_;
        // Resolve's storage buffers are bindings 2, 3, 5 and 6 — 4 is the parameter block and 7
        // is an image.
        const u32 resolve_index = i - kBufferBindings;
        const u32 resolve_binding = (resolve_index < 2)   ? (2 + resolve_index)
                                    : (resolve_index == 2) ? 5u
                                                           : 6u;
        buffer_writes[i].dstBinding = marcher ? (kImageBindings + i) : resolve_binding;
        buffer_writes[i].descriptorCount = 1;
        buffer_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        buffer_writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_.handle(), kBufferBindings + kResolveBuffers, buffer_writes, 0,
                           nullptr);

    // The path tracer's world bindings. The same buffers at the same numbers as the marcher,
    // because it includes the same traversal and the binding numbers come with it, plus the
    // type tables at 14 and 15 so a hit becomes a material.
    {
        // world, types, visuals, clip, faces, lights, frame statistics
        constexpr u32 kTraceBuffers = kBufferBindings + 6;
        const VkBuffer trace_buffers[kTraceBuffers]{
            marcher_buffers[0], marcher_buffers[1], marcher_buffers[2],  marcher_buffers[3],
            marcher_buffers[4], marcher_buffers[5], marcher_buffers[6],  marcher_buffers[7],
            marcher_buffers[8], marcher_buffers[9], marcher_buffers[10], world_buffers_.types(),
            world_buffers_.visuals(), clip_buffer_.buffer,   face_cache_.buffer,
            light_buffer_.buffer, frame_stats_.buffer,
        };
        // The last buffer in that list is the frame statistics, and the binding it lands on
        // has to be the one the shader and gpu/render_params.hpp agree about.
        static_assert(2 + (kTraceBuffers - 1) + 1 == kFrameStatsBinding,
                      "the frame statistics must land on kFrameStatsBinding");
        VkDescriptorBufferInfo trace_infos[kTraceBuffers]{};
        VkWriteDescriptorSet trace_writes[kTraceBuffers + 1]{};
        for (u32 i = 0; i < kTraceBuffers; ++i) {
            trace_infos[i].buffer = trace_buffers[i];
            trace_infos[i].offset = 0;
            trace_infos[i].range = VK_WHOLE_SIZE;
            trace_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            trace_writes[i].dstSet = pathtrace_set_;
            // 2..12 are the world, 13 is the parameter block, then 14 upwards in order —
            // which lands the frame statistics on kFrameStatsBinding.
            trace_writes[i].dstBinding = (i < kBufferBindings) ? (2 + i) : (2 + i + 1);
            trace_writes[i].descriptorCount = 1;
            trace_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            trace_writes[i].pBufferInfo = &trace_infos[i];
        }
        trace_writes[kTraceBuffers].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        trace_writes[kTraceBuffers].dstSet = pathtrace_set_;
        trace_writes[kTraceBuffers].dstBinding = 13;
        trace_writes[kTraceBuffers].descriptorCount = 1;
        trace_writes[kTraceBuffers].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        trace_writes[kTraceBuffers].pBufferInfo = &params_info;
        vkUpdateDescriptorSets(device_.handle(), kTraceBuffers + 1, trace_writes, 0, nullptr);
    }

    // The test scene spans about 64 m. Start at one corner of it, above the ground slab,
    // looking back toward the origin so the towers, arch and lattice are all in frame.
    // On the approach, off the axis, looking up at the portico.
    //
    // Where a building is first seen from is a decision somebody makes, and for a building with a
    // front it is not a corner of the bounding box. The facility faces south — down negative z —
    // so this stands out on the lawn a little to the west of the centre line, at the height of
    // somebody's eyes, and looks back at the steps and the columns above them. Three quarters
    // rather than square on, because a portico read head-on is a row of verticals and read at an
    // angle is a building.
    // The origin, standing in the middle of the rotunda looking out of the main door.
    //
    // Yaw of minus ninety because forward is (cos yaw, sin pitch, sin yaw) — so minus ninety is
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
    if (!options_.cut.empty()) {
        f64 values[6] = {30.0, 0.0, 0.0, 0.0, 90.0, 0.0};
        parse_reals(options_.cut, values, 6);
        cut_at_ = static_cast<u64>(values[0] < 0.0 ? 0.0 : values[0]);
        for (u32 i = 0; i < 5; ++i) cut_pose_[i] = values[i + 1];
        cut_pending_ = true;
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
    // A scripted run must be repeatable, so it never benchmarks, never drifts, and — this is
    // the part that matters — ignores whatever level was saved. Otherwise every screenshot in
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
    // reports as "it says a hundred and then hangs" — so the bar's last drawn state is the high
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
    path_trace_ = options_.path_trace;
    hollow_ = options_.hollow;
    if (!options_.no_update_check) updater_.begin_check();
    WS_LOG_INFO("app", "ready. F1 developer panel, F2 overlay, F5 reload shaders, Esc quit");

    const u64 start_ns = now_ns();
    u64 last_ns = start_ns;

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
        if (input.was_pressed(Key::Escape)) {
            if (mouse_look_) {
                mouse_look_ = false;   // first Escape releases the mouse
                window_.set_relative_mouse(false);
            } else {
                break;
            }
        }
        if (input.was_pressed(Key::F1)) hud_.toggle_developer_panel();
        if (input.was_pressed(Key::F2)) hud_.toggle_overlay();
        if (input.was_pressed(Key::F3)) debug_mode_ = (debug_mode_ + 1) % 7;
        if (input.was_pressed(Key::F4)) {
            path_trace_ = !path_trace_;
            trace_samples_ = 0;
            WS_LOG_INFO("app", "path tracing {}", path_trace_ ? "on" : "off");
        }
        if (input.was_pressed(Key::F5)) {
            visibility_.force_reload();
            resolve_.force_reload();
            pathtrace_.force_reload();
            trace_samples_ = 0;
        }
        // Swap marchers where you are standing, without restarting.
        //
        // Both are built and both are fed every frame while R1e is outstanding, so this costs
        // a branch and nothing else — and it is the only way to compare them on the thing a
        // fixed camera cannot show: what loading and turning round actually feel like. A
        // grid of settled means is blind to that by construction, which is how a marcher that
        // is faster on all seven cameras can still be reported as laggy and both be true.
        //
        // It is also the escape hatch. If the new one is worse in front of you, press F6 and
        // you are back on the old one for the rest of the session.
        if (input.was_pressed(Key::F6)) {
            use_node_pool_ = !use_node_pool_;
            trace_samples_ = 0;
            WS_LOG_INFO("app", "marcher: {}", use_node_pool_ ? "node pool" : "chunk grid");
        }
        if (input.was_pressed(Key::F11)) swapchain_.set_vsync(!swapchain_.vsync());
        // The only thing that starts a download. Nothing else does, and nothing does it
        // automatically.
        if (input.was_pressed(Key::F8) && updater_.state() == UpdateState::Available) {
            updater_.begin_download();
        }

        // Clicking the world captures the mouse; from then on the buttons belong to the
        // chisel. That first click is swallowed, or capturing the mouse would also start a
        // cut you never asked for.
        if (!mouse_look_) {
            if ((input.mouse_left || input.mouse_right) && !hud_.wants_mouse()) {
                mouse_look_ = true;
                swallow_click_ = true;
                window_.set_relative_mouse(true);
            }
        } else if (swallow_click_ && !input.mouse_left && !input.mouse_right) {
            swallow_click_ = false;
        }

        visibility_.reload_if_changed();
        resolve_.reload_if_changed();

        // Who gets the wheel this frame. It is the most contested input in the game, so the
        // rule is written once, here, rather than being discovered by each tool:
        //   a number key held  -> cycle tools within that slot
        //   G held             -> the chisel's working distance
        //   otherwise          -> the active tool, and the free camera if it does not want it
        u32 held_slot = kToolSlots;
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            const Key key = static_cast<Key>(static_cast<u16>(Key::Digit1) + slot);
            if (input.is_down(key)) {
                held_slot = slot;
                break;
            }
        }
        const bool cycling = held_slot < kToolSlots;

        // H takes the wheel and sets how thick a shell a placement leaves. Zero is solid,
        // which is where it starts and what it goes back to.
        //
        // It claims the wheel ahead of everything else, including tool cycling, because it is
        // a modifier you hold deliberately — the same bargain G already makes for distance.
        const bool hollow_has_wheel = input.is_down(Key::H);
        if (hollow_has_wheel && input.wheel != 0.0f) {
            const i32 step = (input.wheel > 0.0f) ? 1 : -1;
            hollow_ = static_cast<u32>(std::max(0, static_cast<i32>(hollow_) + step));
            WS_LOG_INFO("tool", "hollow {}",
                        (hollow_ == 0) ? std::string("off (solid)")
                                       : std::to_string(hollow_) + " voxel shell");
        }

        const bool chisel_has_wheel = !hollow_has_wheel && input.is_down(Key::G);
        // The clipboard only claims the wheel once it is holding something. With nothing
        // selected it has nothing to slide, so the wheel goes back to flight speed — which
        // is what you want while flying somewhere to make a selection.
        const bool clipboard_has_wheel = !cycling && !chisel_has_wheel && !hollow_has_wheel &&
                                         toolbelt_.active() == ToolKind::Clipboard &&
                                         clipboard_.holding();
        const bool tool_has_wheel = cycling || chisel_has_wheel || clipboard_has_wheel ||
                                    hollow_has_wheel;

        const ToolKind tool_before = toolbelt_.active();
        for (u32 slot = 0; slot < kToolSlots; ++slot) {
            const Key key = static_cast<Key>(static_cast<u16>(Key::Digit1) + slot);
            if (input.was_pressed(key)) toolbelt_.select_slot(slot);
        }
        if (cycling && input.wheel != 0.0f) {
            toolbelt_.select_slot(held_slot);
            toolbelt_.cycle(static_cast<i32>(input.wheel));
        }
        // Putting a tool away puts down what it was holding. A ghost that survived a trip
        // through the chisel and reappeared later would be a surprise, not a convenience.
        if (toolbelt_.active() != tool_before) clipboard_.drop();

        const f64 dt = (stats_.last_ms() > 0.0) ? stats_.last_ms() * 0.001 : 1.0 / 60.0;
        camera_.update(input, (dt > 0.1) ? 0.1 : dt, mouse_look_, !tool_has_wheel);

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
        // stopped building — so what the new view is missing is faces and nothing else. Without
        // that gate the cut fires during the load and measures streaming again, which is the
        // confusion this instrument exists to end.
        if (cut_pending_ && (!options_.settle || settled_seen_) &&
            frame_counter_ - settle_frame_ >= cut_at_) {
            camera_.set_position_metres(cut_pose_[0], cut_pose_[1], cut_pose_[2]);
            camera_.set_look(cut_pose_[3], cut_pose_[4]);
            for (u32 i = 0; i < 5; ++i) fly_state_[i] = cut_pose_[i];
            cut_pending_ = false;
            WS_LOG_INFO("frame", "camera cut at measured frame {}", cut_at_);
        }
        update_tools(input, chisel_has_wheel, clipboard_has_wheel, (dt > 0.1) ? 0.1 : dt);

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
            const ResidencyStats residency = residency_.stats();
            std::snprintf(what, sizeof(what),
                          "tool %d  path trace %d  debug %u  resident %u/%zu chunks",
                          static_cast<int>(toolbelt_.active()), path_trace_ ? 1 : 0,
                          debug_mode_, residency.resident_chunks, world_.chunk_count());
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
        // It is invisible in every measurement in this file — the same work happens in the same
        // order on the GPU and the frame time is identical — and it is the difference between a
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
            if (!settled_seen_ && frame_counter_ > kSettleGiveUp) {
                settled_seen_ = true;
                settle_frame_ = frame_counter_;
                WS_LOG_WARN("frame",
                            "gave up waiting for the world to settle after {} frames; measuring "
                            "from here anyway, and this figure is not comparable with a settled "
                            "one", kSettleGiveUp);
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
        // the loading screen rather than the renderer — the same reasoning the first-run
        // benchmark in documentation/19-auto-quality.md already uses.
        if (!options_.screenshot.empty() && measuring && measured == options_.screenshot_frame / 2) {
            profiler_.reset_averages();
        }

        if (!options_.screenshot.empty() && measuring && measured >= options_.screenshot_frame) {
            device_.wait_idle();
            save_image_png(device_, render_target_, options_.screenshot);

            // A figure taken while the world is still being built is not comparable to anything,
            // and nothing said so.
            //
            // The scene is sharpened region by region over the opening frames and the result is
            // cached — but the cache is only written when the LAST region lands, and a scripted
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
            // voxel counts are a proxy — two different worlds can share both — and the question
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
            // by the worst frame and not by the mean one (documentation/09 §9).
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
            WS_LOG_INFO("frame", "CPU node pool {:.3f} ms, worst {:.3f} on frame {}; chunk "
                                 "residency {:.3f} ms, worst {:.3f}",
                        node_ms_, worst_node_ms_, worst_node_frame_, residency_ms_,
                        worst_residency_ms_);
            const ResidencyStats residency = residency_.stats();
            WS_LOG_INFO("frame",
                        "resident {} of {} chunks, {} bricks; feedback {} reports ({} dropped)",
                        residency.resident_chunks, world_.chunk_count(),
                        residency.resident_bricks, last_feedback_,
                        last_feedback_truncated_);
            // In bytes as well as in chunks, because the claim the rewrite makes about streaming
            // is about memory following the *screen* — and a chunk count cannot show that. Two
            // views holding the same number of chunks at different resolutions should differ
            // here, and that difference is the thing being aimed at.
            // What the card actually holds, against what the pool holds. Run at the screenshot
            // rather than per frame because it stalls the device; that is often enough to catch
            // an upload that is dropping or misplacing something, which is the class of fault
            // this exists for.
            if (use_node_pool_) {
                node_buffers_.audit(node_pool_);
                face_buffers_.audit(face_store_);
                const FaceStoreStats face_stats = face_store_.stats();
                WS_LOG_INFO("frame",
                            "faces: {} live, {} seen this frame, {} claims {} already there, "
                            "{} evicted, {} bytes of faces ({} with the table)",
                            face_stats.faces, last_faces_seen_, face_stats.claims,
                            face_stats.hits, face_stats.evictions, face_stats.face_bytes,
                            face_stats.total_bytes);

                // What SIZE the faces are, which is the size of the smallest shadow the frame can
                // cast. The plan's arithmetic assumes level 0 near the camera -- a voxel covers a
                // whole pixel at 22.5 m at 1440 lines, so everything nearer gains nothing from
                // more pixels (§6). A store with no level 0 in it is not shading voxel faces
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
                        "node pool {}: {} nodes, {} leaves, {} bytes ({} for the screen); "
                        "built {} evicted {}; "
                        "requests {} hits {} deferred {}",
                        use_node_pool_ ? "marching" : "idle", node_stats.nodes,
                        node_stats.leaves, node_stats.total_bytes, node_stats.screen_bytes,
                        last_node_built_,
                        last_node_evicted_, node_stats.requests, node_stats.hits,
                        last_node_deferred_);
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
            WS_LOG_INFO("frame", "resident bytes {} payload, {} index, {} total",
                        residency.payload_in_use, residency.index_bytes,
                        residency.total_bytes);
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
    world_buffers_.destroy();
    feedback_.destroy();
    destroy_buffer(device_, params_buffer_);
    destroy_buffer(device_, face_cache_);
    destroy_buffer(device_, frame_stats_);
    destroy_buffer(device_, clip_buffer_);
    destroy_buffer(device_, clip_staging_);
    destroy_buffer(device_, light_buffer_);
    visibility_.destroy();
    node_visibility_.destroy();
    // The face pass and its store, which were added without being added here.
    //
    // The cost of the omission is exactly what the note below predicts: validation reports three
    // buffers still alive at vkDestroyDevice, and the pipeline's own destructor then runs against
    // a device that no longer exists and takes an access violation inside the driver, with this
    // file nowhere in the stack. It reads as a driver bug and is a missing line.
    shade_faces_.destroy();
    face_buffers_.destroy();
    node_buffers_.destroy();
    if (node_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), node_layout_, nullptr);
        node_layout_ = VK_NULL_HANDLE;
    }
    resolve_.destroy();
    pathtrace_.destroy();
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
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), set_layout_, nullptr);
    }
    if (resolve_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), resolve_layout_, nullptr);
    }
    if (pathtrace_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), pathtrace_layout_, nullptr);
    }
    profiler_.destroy();
    swapchain_.destroy();
    device_.destroy();
    window_.destroy();
    return 0;
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
    // at is almost never the whole clip — it is a portico, or a room — and framing needs that
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
    const forge::SampleResult built =
        forge::sample(script.field, script.solid, script.paint, script.settings, &jobs);
    const u64 sampled = now_ns();

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

    // Where the matter actually is, in metres, in the world. Not the sampled box — the matter.
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
    }

    // Alignment: which parts nearly line up with each other, and by how much they miss.
    //
    // Architecture is mostly things lining up. A column under a beam, a wall over a wall, a sill
    // level with a sill — and the failure that matters is never a part in wildly the wrong place,
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
                    (found > 40) ? " (first 40 shown)" : (found == 0 ? " — everything is flush" : ""));
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
    ws::crash_set_dialog(!options.headless && !options.stream_audit &&
                         options.screenshot.empty());

    // "frame" is the one kind that has to happen inside the running game, because what it
    // proves is that a report carries the camera and the device with it.
    if (!options.crash_test.empty() && options.crash_test != "frame") {
        return ws::run_crash_test(options.crash_test);
    }
    if (options.stream_audit) return ws::run_stream_audit(options);
    if (options.headless) return ws::run_headless(options);

    ws::Application app;
    return app.run(options);
}
