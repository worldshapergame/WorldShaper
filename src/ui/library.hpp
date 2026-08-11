#pragma once
// A library is a file manager over the real folder that kind of thing lives in.
//
// Not a metaphor for one (D445). The folders are folders on disk and the files are files, under
// `%LOCALAPPDATA%\WorldShaper\`:
//
//     worlds\      .wsworld     one file per world
//     clips\       .wsclip      creations, procedural or not
//     materials\   .wsmat       pattern generators
//     mods\        .wsmod       Lua and native packages
//     characters\  .wsclip      the same format; a different shelf
//     scripts\     .wslua
//     trash\                    where a delete goes
//
// A player can drag a clip in from Explorer and it appears; a player can back up their work by
// copying a folder. The rule that falls out of that, and the reason this class holds so little:
//
//   **Nothing in the game may keep state about a file that the file does not carry**, because the
//   file can move, be renamed or be deleted without the game watching. There is no index, no
//   database and no sidecar. A listing is a directory read, and it is re-read when the directory's
//   own timestamp says it changed.
//
// # The author tag (D447)
//
// Every file the game creates carries the name of whoever made it, and it travels with every copy.
// A clip you downloaded says who made it, for ever, in your library and in anyone's you pass it to.
// The interface cannot edit that field — not because it cannot be forged from outside, but because
// a name the interface lets you overwrite is a name that means nothing.
//
// It is one ASCII marker, `WSauthor:`, followed by the name and a newline, written as whatever
// counts as a comment for that kind (`#` for a clip, `--` for Lua) or as a plain tag block for a
// binary container. One marker for every format, because a per-format author field is a per-format
// place to forget one — and a reader that scans the first few kilobytes for a fixed string works on
// a file the game has never seen before, which is exactly the file that came from somebody else.
//
// # The trash (D446)
//
// A delete goes to `trash\` rather than away, because a delete a player cannot undo is not what
// they pressed, and this is a library of work somebody made. The trash is a shelf like any other:
// it can be opened, and emptying it is a second, separate decision.

#include <filesystem>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "ui/draw.hpp"

namespace ws::ui {

// One shelf. The set is open — a mod that adds a kind of thing needs somewhere to put it — so this
// is a value in a list rather than an enumeration anybody switches on.
struct Kind {
    std::string folder;      // its directory under the root
    std::string extension;   // what one of its files is called, dot included
    std::string label;       // what the tab says
    Icon icon = Icon::Clip;
    // Whether a file of this kind is text, which decides how its author tag is written. A binary
    // container gets the tag as a plain block; a text file gets it as a comment, so the file still
    // opens in Notepad and still parses.
    bool text = true;
    std::string comment = "#";
};

// What a listing holds. Everything here is read from the file system or from the file itself;
// nothing is remembered between listings.
struct Entry {
    std::string name;    // on disk, extension included
    std::string shown;   // what a player reads: the stem, or the folder's name
    bool folder = false;
    u64 bytes = 0;
    i64 modified = 0;    // seconds since the epoch, as the file system reports it
    std::string author;  // empty when the file carries no tag, which is what a foreign file is
    std::filesystem::path path;
};

enum class Sort : u32 { Name, Date, Author, Size, Count };

// The root every shelf hangs under. `%LOCALAPPDATA%\WorldShaper\` where the platform has one, and
// a `WorldShaper` folder beside the working directory where it does not — which is what a test
// gets, and what a portable install would get.
std::filesystem::path default_root();

// The six shelves the game ships with, in the order they are offered.
const std::vector<Kind>& shipped_kinds();

// Reads the author out of a file, by scanning its first few kilobytes for the marker. Returns
// empty for a file that has none, which is not an error: it is a file from before the tag existed,
// or one somebody made with a text editor.
std::string read_author(const std::filesystem::path& path);

// Writes the marker into a file being created. Called by whatever creates the file, once, at
// creation — never afterwards, and never from the interface.
bool write_author(const std::filesystem::path& path, const Kind& kind, const std::string& author);

// The same text with every author line taken out of it.
//
// This exists for exactly one caller and it is worth saying why. A built world is cached under a
// key hashed from the source that produced it, so that editing a clip throws the cache away — and
// **who made a file is not part of what the file builds**. Without this, stamping a copy with its
// author changes the text, changes the key, and throws away a cache that is still perfectly good:
// every world put on the shelf would rebuild from cold the first time it was opened, which is
// minutes for a large one and looks exactly like the loading path not working at all.
std::string without_author(const std::string& text);

class Library {
public:
    explicit Library(std::filesystem::path root = default_root());

    const std::filesystem::path& root() const { return root_; }

    // Makes sure every shelf exists. Called once at start-up; a shelf that a player deleted comes
    // back rather than making the library an error message.
    void ensure_folders() const;

    // --- where you are ------------------------------------------------------------------------
    void open(const Kind& kind);
    const Kind& kind() const { return kind_; }
    // The folder being looked at, and where it sits relative to the shelf's own root.
    const std::filesystem::path& here() const { return here_; }
    std::string breadcrumb() const;
    bool at_top() const;
    void enter(const Entry& folder);
    void up();

    // Re-reads the directory if it has changed since the last listing, and unconditionally when
    // `force` is set. A player who drags a file in from Explorer expects to see it; a player
    // scrolling a list of four hundred does not expect the disk to be read sixty times a second.
    void refresh(bool force = false);
    const std::vector<Entry>& entries() const { return entries_; }

    void set_sort(Sort by, bool descending);
    Sort sort() const { return sort_; }
    bool descending() const { return descending_; }

    // --- what a library does, all of it on a multiple selection ------------------------------
    //
    // Each returns an empty string on success and one line saying what happened on failure. One
    // line, because that is what the interface has room to say and because a file system error a
    // player cannot act on is worse than none.
    std::string make_folder(const std::string& name);
    std::string rename(const Entry& entry, const std::string& to);
    std::string duplicate(const std::vector<Entry>& entries);
    std::string erase(const std::vector<Entry>& entries);
    std::string move(const std::vector<Entry>& entries, const std::filesystem::path& into);
    // A new, empty file of this shelf's kind, carrying its author tag. This is what the editor
    // tab's *new* does, and it is why the editor asks for a file before it opens (D455).
    std::string create(const std::string& name, const std::string& author,
                       const std::string& contents = {});

    // Where a delete goes. Its own shelf, so it can be opened and emptied.
    std::filesystem::path trash() const { return root_ / "trash"; }

private:
    void list();
    // A name that does not already exist in `into`, by appending " 2", " 3", and so on. Used by
    // duplicate and by the trash, where two files of the same name from two folders would
    // otherwise silently become one.
    std::filesystem::path free_name(const std::filesystem::path& into, const std::string& stem,
                                    const std::string& extension) const;

    std::filesystem::path root_;
    Kind kind_;
    std::filesystem::path here_;
    std::vector<Entry> entries_;
    Sort sort_ = Sort::Name;
    bool descending_ = false;
    std::filesystem::file_time_type stamp_{};
    bool listed_ = false;
};

}  // namespace ws::ui
