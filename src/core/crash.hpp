#pragma once
// Turns a crash into a file on disk instead of a window that vanishes.
//
// A build launched from Explorer has no console, so an access violation, a failed WS_CHECK
// or an out-of-memory abort all look identical from the outside: the window appears and
// then it is gone. That is unreportable. After crash_install(), every one of those writes a
// report naming the fault, the call stack that produced it and the last few hundred log
// lines leading up to it, plus a minidump for postmortem debugging, and then tells the
// player where those files are.

#include <string>
#include <string_view>

namespace ws {

// Install the handlers. Call once, as the first statement of main, before anything exists
// that could fault. `log_name` is the session log written alongside any crash report.
void crash_install(std::string_view log_name = "worldshaper.log");

// The directory reports and logs are written to. Under %LOCALAPPDATA% rather than next to
// the executable, because an installed copy may sit somewhere the player cannot write.
// Created if it does not exist. Empty only if that failed.
const std::string& crash_log_dir();

// Path of the session log, for printing at startup so a player knows what to attach.
const std::string& crash_log_path();

// Record a line in every crash report from here on: the map being loaded, the shader being
// compiled, whatever a fault at this moment would need explaining. Call with an empty view
// to clear. Cheap enough for per-frame use; safe from any thread.
void crash_set_context(std::string_view key, std::string_view value);

// Whether a crash pops a dialog naming the report. On by default, because a player who
// never sees a console otherwise has no idea a file was written. Scripted runs turn it off:
// a modal dialog in an automated test is a hang.
void crash_set_dialog(bool enabled);

// Force a report without dying, for diagnosing a hang or a wedged device. Returns the path
// written, or an empty string.
std::string crash_write_report(const char* reason);

}  // namespace ws
