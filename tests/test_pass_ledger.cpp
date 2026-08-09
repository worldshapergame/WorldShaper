// The bookkeeping behind GPU pass timing.
//
// Every case here is one the old implementation got wrong or would have got wrong, and the one
// that matters is `nested passes keep their own slots` — that is the fault that hid the path
// tracer's entire cost behind a plausible-looking list of passes for months. See the note at the
// top of src/core/pass_ledger.hpp.

#include <doctest/doctest.h>

#include "core/pass_ledger.hpp"

using namespace ws;

TEST_CASE("a flat frame gives every pass its own slot, in order") {
    PassLedger ledger;
    ledger.reset();

    CHECK(ledger.open("streaming", 0.8) == 0);
    CHECK(ledger.close() == 0);
    CHECK(ledger.open("visibility", 9.5) == 1);
    CHECK(ledger.close() == 1);
    CHECK(ledger.open("resolve", 0.8) == 2);
    CHECK(ledger.close() == 2);

    CHECK(ledger.count() == 3);
    CHECK(ledger.balanced());
    CHECK(ledger.name(0) == "streaming");
    CHECK(ledger.name(1) == "visibility");
    CHECK(ledger.name(2) == "resolve");
    CHECK(ledger.budget(1) == doctest::Approx(9.5));
    CHECK(ledger.pass_depth(0) == 0);
    CHECK(ledger.pass_depth(2) == 0);
}

TEST_CASE("nested passes keep their own slots") {
    // This is the frame shape that broke it: the cloud dispatch sits inside the tracer's pass.
    //
    // The old code claimed a slot at close() rather than at open(), so `cloud` was handed
    // `pathtrace`'s slot and overwrote its name; cloud's close cleared the single open index; and
    // pathtrace's close then found nothing open and wrote no end timestamp at all. The tracer's
    // own dispatch — everything after the inner pass — fell outside every timestamp in the frame.
    PassLedger ledger;
    ledger.reset();

    const i32 outer = ledger.open("pathtrace", 1000.0);
    const i32 inner = ledger.open("cloud", 0.55);
    CHECK(inner != outer);
    CHECK(ledger.close() == inner);    // cloud closes first
    CHECK(ledger.close() == outer);    // and the tracer still has an end to write

    CHECK(ledger.count() == 2);
    CHECK(ledger.balanced());
    CHECK(ledger.name(static_cast<u32>(outer)) == "pathtrace");
    CHECK(ledger.name(static_cast<u32>(inner)) == "cloud");
    CHECK(ledger.pass_depth(static_cast<u32>(outer)) == 0);
    CHECK(ledger.pass_depth(static_cast<u32>(inner)) == 1);
}

TEST_CASE("depth is recorded so a child is not counted towards the frame twice") {
    PassLedger ledger;
    ledger.reset();
    ledger.open("a", 0.0);
    ledger.open("b", 0.0);
    ledger.open("c", 0.0);
    CHECK(ledger.depth() == 3);
    ledger.close();
    ledger.close();
    ledger.close();
    CHECK(ledger.depth() == 0);
    CHECK(ledger.pass_depth(0) == 0);
    CHECK(ledger.pass_depth(1) == 1);
    CHECK(ledger.pass_depth(2) == 2);
}

TEST_CASE("bytes are charged to the innermost open pass") {
    PassLedger ledger;
    ledger.reset();
    ledger.open("outer", 0.0);
    ledger.add_bytes(100);
    ledger.open("inner", 0.0);
    ledger.add_bytes(7);
    ledger.close();
    ledger.add_bytes(5);
    ledger.close();

    CHECK(ledger.bytes(0) == 105);
    CHECK(ledger.bytes(1) == 7);
}

TEST_CASE("an unclosed pass is reported rather than silently dropped") {
    PassLedger ledger;
    ledger.reset();
    ledger.open("opened", 0.0);
    CHECK_FALSE(ledger.balanced());
    ledger.close();
    CHECK(ledger.balanced());
}

TEST_CASE("closing with nothing open is refused rather than corrupting the stack") {
    PassLedger ledger;
    ledger.reset();
    CHECK(ledger.close() == -1);
    CHECK(ledger.depth() == 0);
    // And the ledger still works afterwards: a stray close must not poison the frame.
    CHECK(ledger.open("after", 0.0) == 0);
    CHECK(ledger.close() == 0);
}

TEST_CASE("running out of slots refuses cleanly and stays balanced") {
    PassLedger ledger;
    ledger.reset();
    for (u32 i = 0; i < PassLedger::kMaxPasses; ++i) {
        CHECK(ledger.open("pass", 0.0) == static_cast<i32>(i));
        CHECK(ledger.close() == static_cast<i32>(i));
    }
    // One past the end: refused, and its close is refused with it, so the pair still balances.
    CHECK(ledger.open("too many", 0.0) == -1);
    CHECK(ledger.close() == -1);
    CHECK(ledger.count() == PassLedger::kMaxPasses);
    CHECK(ledger.balanced());
}

TEST_CASE("a refused open inside a real one does not steal its parent's close") {
    // The case that makes the sentinel necessary. If a refused open pushed nothing, the inner
    // close would pop the OUTER pass — writing the outer's end timestamp early and leaving the
    // outer's own close to pop an empty stack. That is the original bug wearing a different hat.
    PassLedger ledger;
    ledger.reset();
    for (u32 i = 0; i < PassLedger::kMaxPasses - 1; ++i) {
        ledger.open("filler", 0.0);
        ledger.close();
    }
    const i32 outer = ledger.open("outer", 0.0);
    CHECK(outer == static_cast<i32>(PassLedger::kMaxPasses - 1));
    CHECK(ledger.open("refused", 0.0) == -1);   // no slots left
    CHECK(ledger.close() == -1);                // the refused one
    CHECK(ledger.close() == outer);             // and the outer still gets its end
    CHECK(ledger.balanced());
}

TEST_CASE("reset clears everything, including the byte counters") {
    PassLedger ledger;
    ledger.reset();
    ledger.open("a", 1.0);
    ledger.add_bytes(999);
    ledger.close();
    CHECK(ledger.count() == 1);

    ledger.reset();
    CHECK(ledger.count() == 0);
    CHECK(ledger.depth() == 0);
    CHECK(ledger.balanced());
    CHECK(ledger.open("b", 2.0) == 0);
    CHECK(ledger.bytes(0) == 0);   // not carrying the previous frame's traffic
    ledger.close();
}
