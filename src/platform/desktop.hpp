#pragma once
// The two things the interface has to ask the desktop for, and nothing else.
//
// `src/platform/window.cpp` is the only file that knows SDL exists; this is the only file that
// knows the shell of the operating system exists. Both are here for the same reason: a library
// that reaches for `ShellExecute` is a library that only builds on Windows, and the interface
// above this line should not be one.
//
// Everything else the library does is `std::filesystem`, which is portable and already enough.
// These two are not expressible that way at all — showing a folder to a person and putting a file
// somewhere the person can get it back from are both things only the desktop can do.

#include <filesystem>

namespace ws {

// Show a folder to the player, in whatever the system's file manager is. False if there is no
// desktop to show it on, which is what a headless run and a test both are.
bool open_in_file_manager(const std::filesystem::path& path);

// Move a file or a folder to the system's own recycle bin — not a folder of ours (D491).
//
// A delete a player cannot undo is not what they pressed, and that was the whole argument for the
// game keeping a `trash\` of its own. The argument was right and the answer was wrong: every player
// already has an undoable delete, it is the one every other file on their machine goes through,
// they already know where it is and how to empty it, and it survives the game not being running.
// A second one inside the game is a second place to look and a second thing to explain.
//
// False when the file could not be recycled, which is a real case — a drive with no recycle bin,
// or a file the shell refuses — and the caller must not fall back to deleting outright, because
// "could not put it where you can get it back" and "threw it away" are different answers.
bool send_to_recycle_bin(const std::filesystem::path& path);

}  // namespace ws
