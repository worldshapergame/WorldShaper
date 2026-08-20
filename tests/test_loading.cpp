// The loading screen's state: the way in, the step ledger, and where the button is.
//
// None of this needs a device, which is the point of it living in `src/app/loading.hpp` rather
// than beside the shader that draws it. What is being checked here is the handful of rules that
// make an early entry safe, and every one of them is a rule that would otherwise only be checked
// by a person clicking a button on a machine with a card in it.

#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "app/loading.hpp"

using namespace ws;

TEST_CASE("a way in cannot be taken before it is offered") {
    LoadProgress progress;
    LoadHistory history;
    progress.begin(history, false);

    CHECK(!progress.offered_early_entry());
    CHECK(!progress.asked_to_enter());

    // A press that arrives before the offer is DISCARDED rather than queued. This is the rule the
    // whole feature rests on: the offer is the build saying "everything from here can be built
    // again", and a press remembered from before it would take the player out of a stage that has
    // no way out -- the parse, whose result the ladder cannot exist without.
    progress.ask_to_enter();
    CHECK(!progress.asked_to_enter());

    progress.offer_early_entry();
    CHECK(progress.offered_early_entry());
    CHECK(!progress.asked_to_enter());   // offering is not pressing

    progress.ask_to_enter();
    CHECK(progress.asked_to_enter());
}

TEST_CASE("the snapshot tells the drawing which of the three states the button is in") {
    LoadProgress progress;
    LoadHistory history;
    progress.begin(history, false);

    // Nothing offered: no button at all, which is every frame of a load until the ladder has its
    // plan, and every frame of a scripted run.
    LoadProgress::Snapshot look = progress.look();
    CHECK(!look.may_enter);
    CHECK(!look.entering);

    progress.offer_early_entry();
    look = progress.look();
    CHECK(look.may_enter);
    CHECK(!look.entering);

    progress.ask_to_enter();
    look = progress.look();
    // Offered and taken are mutually exclusive, so the drawing never has to decide which of two
    // true things to show: a pressed button is not also an offered one.
    CHECK(!look.may_enter);
    CHECK(look.entering);
}

TEST_CASE("a finished load still says whether the player left it early") {
    LoadProgress progress;
    LoadHistory history;
    progress.begin(history, false);
    progress.offer_early_entry();
    progress.ask_to_enter();
    progress.finish();

    // `look` returns early once the load is complete, and it used to return before reading these.
    // The frame that draws a full bar and the frame that draws a pressed button are the same
    // frame, so both have to survive the early return.
    const LoadProgress::Snapshot look = progress.look();
    CHECK(look.complete);
    CHECK(look.entering);
}

TEST_CASE("beginning a load forgets the last one's button") {
    LoadProgress progress;
    LoadHistory history;
    progress.begin(history, false);
    progress.offer_early_entry();
    progress.ask_to_enter();
    progress.note_entered_early();
    CHECK(progress.entered_early());

    // A level change is the same operation as a load (see the comment on `progress_` in main.cpp),
    // so a second world must not inherit the first one's press. Inheriting it would skip the
    // second world's up-front build with nobody having asked.
    progress.begin(history, false);
    CHECK(!progress.offered_early_entry());
    CHECK(!progress.asked_to_enter());
    CHECK(!progress.entered_early());
}

TEST_CASE("the step ledger keeps the order and drops what rounds to nothing") {
    LoadSteps steps;
    steps.begin();
    steps.add("parse the clip", 0.052);
    steps.add("a step nobody can see", 0.0001);   // under half a millisecond
    steps.add("visibility.comp", 13.223);

    const std::string line = steps.line();
    // The order is the order they happened in, because the whole use of the line is reading down
    // it to find where the seconds went.
    CHECK(line.find("parse the clip 52ms") != std::string::npos);
    CHECK(line.find("visibility.comp 13223ms") != std::string::npos);
    CHECK(line.find("parse the clip") < line.find("visibility.comp"));
    // A dozen steps under a millisecond would bury the one that took thirteen seconds.
    CHECK(line.find("a step nobody can see") == std::string::npos);
    // ...but they still count towards the total, so the line and the sum cannot disagree about
    // what the load cost.
    CHECK(steps.total() == doctest::Approx(13.2751));

    steps.begin();
    CHECK(steps.line().empty());
    CHECK(steps.total() == doctest::Approx(0.0));
}

TEST_CASE("the step ledger survives being written from several threads") {
    // Six shaders now compile on six threads and each files its own step. A vector written from
    // six threads with no lock is not a slow ledger, it is a crash on the load screen.
    LoadSteps steps;
    steps.begin();
    std::vector<std::thread> writers;
    for (u32 i = 0; i < 6; ++i) {
        writers.emplace_back([&steps] {
            for (u32 n = 0; n < 200; ++n) steps.add("shader.comp", 0.001);
        });
    }
    for (std::thread& writer : writers) writer.join();
    CHECK(steps.total() == doctest::Approx(1.2));
}

TEST_CASE("the way in is somewhere a person can reach") {
    // 1280x720 is what this machine's desktop clamps to, and 3840x2160 is the other end.
    for (const auto& size : {std::pair<u32, u32>{1280, 720}, std::pair<u32, u32>{3840, 2160},
                             std::pair<u32, u32>{800, 600}}) {
        const u32 width = size.first;
        const u32 height = size.second;
        const LoadingButtonRect rect = loading_button_rect(width, height);

        // Inside the window, or it cannot be clicked at all.
        CHECK(rect.x0 >= 0.0f);
        CHECK(rect.y0 >= 0.0f);
        CHECK(rect.x1 <= static_cast<f32>(width));
        CHECK(rect.y1 <= static_cast<f32>(height));

        // Centred horizontally on the column, like everything else on that screen.
        CHECK(rect.x0 + rect.x1 == doctest::Approx(static_cast<f32>(width)));

        // Its own centre is inside it, which is the property the hit test needs and the one that
        // an off-by-one in `contains` would break.
        CHECK(rect.contains((rect.x0 + rect.x1) * 0.5f, (rect.y0 + rect.y1) * 0.5f));
        CHECK(!rect.contains(rect.x0 - 1.0f, (rect.y0 + rect.y1) * 0.5f));
        CHECK(!rect.contains((rect.x0 + rect.x1) * 0.5f, rect.y1 + 1.0f));

        // BELOW the bar and below the last row of text. The bar is twelve interface pixels tall
        // centred on the middle of the window and the last text row sits at 32; a button that
        // overlapped either would be a button the player cannot read and cannot aim at.
        const f32 scale = loading_ui_scale(width, height);
        CHECK(rect.y0 > static_cast<f32>(height) * 0.5f + 40.0f * scale);

        // Big enough to hit. Eight characters at two interface pixels a glyph pixel is about
        // sixty pixels of label, and a target under twenty pixels tall is one people miss.
        CHECK(rect.width() >= 60.0f);
        CHECK(rect.height() >= 20.0f);
    }
}

TEST_CASE("the interface scale never collapses on a small window") {
    // The floor matters: `scale` divides the hit test's coordinates, and a scale of nought would
    // put the button everywhere or nowhere.
    CHECK(loading_ui_scale(320, 200) == doctest::Approx(1.0f));
    CHECK(loading_ui_scale(1280, 720) == doctest::Approx(1.0f));
    CHECK(loading_ui_scale(3840, 2160) == doctest::Approx(5.0f));
    // Sized from the SHORT side, so a very wide window does not grow the column off the sides.
    CHECK(loading_ui_scale(3840, 720) == doctest::Approx(loading_ui_scale(1280, 720)));
}

TEST_CASE("a load spent finishing the world is a BUILD, not a cache hit") {
    // The two shapes are kept apart so that a cache hit never teaches the next cold build that
    // building is free -- a stage weighted at nothing cannot move the bar however long it runs,
    // and that pinned a real bar at eight per cent for a hundred and forty seconds.
    //
    // `shape_of` used to ask that question of `Sampling` alone, which was the whole of the
    // world-making until the whole-world pass existed. It is now where nearly all of a cold
    // load's time goes: a world read from the cache in 200 ms and then FINISHED over twenty
    // minutes is a twenty-minute build, and filing it as a cache hit is the exact fault the two
    // shapes exist to prevent.
    f64 finished_the_world[static_cast<usize>(LoadStage::Count)]{};
    finished_the_world[static_cast<usize>(LoadStage::Reading)] = 0.2;
    finished_the_world[static_cast<usize>(LoadStage::Stamping)] = 0.3;
    finished_the_world[static_cast<usize>(LoadStage::Filling)] = 1200.0;
    CHECK(LoadHistory::shape_of(finished_the_world) == LoadHistory::kBuilt);

    // ...and a world that came back COMPLETE spends nothing there, which is a cache hit and has
    // to stay one.
    f64 read_it_back[static_cast<usize>(LoadStage::Count)]{};
    read_it_back[static_cast<usize>(LoadStage::Reading)] = 0.2;
    read_it_back[static_cast<usize>(LoadStage::Stamping)] = 3.0;
    read_it_back[static_cast<usize>(LoadStage::Uploading)] = 1.0;
    read_it_back[static_cast<usize>(LoadStage::Filling)] = 0.05;
    CHECK(LoadHistory::shape_of(read_it_back) == LoadHistory::kCached);

    // And the cold up-front build, which is what the question used to be about and must not have
    // stopped working.
    f64 sampled_it[static_cast<usize>(LoadStage::Count)]{};
    sampled_it[static_cast<usize>(LoadStage::Reading)] = 0.2;
    sampled_it[static_cast<usize>(LoadStage::Sampling)] = 30.0;
    sampled_it[static_cast<usize>(LoadStage::Stamping)] = 1.0;
    CHECK(LoadHistory::shape_of(sampled_it) == LoadHistory::kBuilt);
}

TEST_CASE("every stage has a name, including the last one") {
    // The screen packs `stage_name` into the push block and the shader draws whatever arrives, so
    // a stage added to the enum and not to the switch is a blank line on the loading screen rather
    // than a compile error. Every one of them, by walking the enum.
    for (u32 i = 0; i < static_cast<u32>(LoadStage::Count); ++i) {
        const char* name = stage_name(static_cast<LoadStage>(i));
        REQUIRE(name != nullptr);
        CHECK(std::string(name).size() > 0);
        // Twenty-three characters is the whole of a text slot in `gpu/loading_screen.cpp`, and a
        // name longer than that is silently cut in half on the one screen nobody tests by eye.
        CHECK(std::string(name).size() <= 23);
    }
}

TEST_CASE("a finished load reports the last stage rather than a named one") {
    LoadProgress progress;
    LoadHistory history;
    progress.begin(history, false);
    progress.enter(LoadStage::Filling);
    progress.finish();
    const LoadProgress::Snapshot look = progress.look();
    CHECK(look.complete);
    CHECK(look.fraction == doctest::Approx(1.0));
    // It named `settling` here until the stage after it existed, which drew the wrong icon on the
    // one frame that says the game is up.
    CHECK(look.stage == static_cast<LoadStage>(static_cast<u32>(LoadStage::Count) - 1));
}

TEST_CASE("the whole-world stage carries most of the bar on a first run") {
    // The nominal weights are only ever used on the very first run of a clip, and getting this one
    // wrong is not cosmetic: a `finishing the world` stage weighted like `settling` would put the
    // bar in the high nineties within a second of launching and leave it there for twenty minutes,
    // which is the failure the whole file is written against.
    LoadProgress progress;
    LoadHistory history;   // nothing known: the nominal weights
    progress.begin(history, false);
    progress.enter(LoadStage::Filling);
    progress.within(0.0);
    const LoadProgress::Snapshot at_the_start = progress.look();
    CHECK(at_the_start.fraction < 0.35);
}
