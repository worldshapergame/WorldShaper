#pragma once
// Checking whether a newer release exists, and installing it.
//
// # Why this is written the way it is
//
// A game that updates itself is a game that downloads and runs code on someone else's
// machine. That deserves care rather than convenience, so:
//
//  - **It asks.** The check happens by itself; the download never does. The player is told
//    what version is available and has to say yes. An update that installs itself while you
//    were trying to play is not a feature.
//  - **It only ever looks at one place** — the releases of this repository, over HTTPS, with
//    certificate checking left on. The URL is built from the owner and repository baked into
//    the build, never from anything the server said, so a compromised response cannot point
//    the download somewhere else.
//  - **It uses the operating system's own HTTPS stack.** No HTTP library to depend on, to
//    licence, or to keep patched — which matters for a project that has to stay free to
//    publish (documentation/08).
//  - **It never overwrites the running program.** Windows will not allow it anyway; the
//    running executable is renamed aside and the new one put in its place, and the old one
//    is cleaned up the next time the game starts.
//  - **It is off in a development build.** Nothing is more annoying than a build you are
//    debugging replacing itself with a release.
//
// The check costs one small HTTPS request on a background thread and never blocks the game
// starting. If the network is down, or GitHub is unreachable, or the response makes no
// sense, the answer is "no update" and nothing is said.

#include <atomic>
#include <string>
#include <thread>

#include "core/types.hpp"

namespace ws {

struct UpdateInfo {
    bool available = false;
    std::string version;      // "0.6.0"
    std::string tag;          // "v0.6.0"
    std::string download_url; // the release asset, as GitHub gave it
    std::string notes;        // the release's title
    u64 size_bytes = 0;
};

enum class UpdateState : u8 {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Installed,   // ready; takes effect on the next start
    Failed,
};

class Updater {
public:
    ~Updater();

    // Starts the check on its own thread. Returns immediately.
    void begin_check();

    // Starts the download. Only valid once a check has found something, and only after the
    // player has said yes — this is never called from the check.
    void begin_download();

    UpdateState state() const { return state_.load(std::memory_order_acquire); }
    // Safe to read whenever state() is Available or later; the worker publishes it before
    // moving the state on.
    const UpdateInfo& info() const { return info_; }
    const std::string& message() const { return message_; }
    f32 progress() const { return progress_.load(std::memory_order_relaxed); }

    // Removes the previous executable left beside this one by an earlier update. Called once
    // at startup, and quietly does nothing when there is not one.
    static void clean_up_previous();

private:
    void join();

    std::thread worker_;
    std::atomic<UpdateState> state_{UpdateState::Idle};
    std::atomic<f32> progress_{0.0f};
    UpdateInfo info_;
    std::string message_;
};

}  // namespace ws
