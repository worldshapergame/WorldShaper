#include "ui/library.hpp"

#include "platform/window.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/crash.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"
#include "platform/desktop.hpp"

namespace ws::ui {
namespace {

// The one marker, for every format. See the header for why it is one and not one per kind.
constexpr const char* kAuthorMarker = "WSauthor:";
// How far into a file it may be. Generous enough for a clip that opens with a paragraph about
// itself, small enough that listing four hundred files is four hundred short reads.
constexpr usize kAuthorWindow = 4096;

i64 seconds_of(const std::filesystem::file_time_type& when) {
    // Portable enough for a sort and for a date on a row. It is not used for anything that has to
    // agree between machines, which is the only reason a clock conversion is allowed here at all.
    const auto since = when.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(since).count();
}

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Whether a name is one the file system will take, and one a player can find again. Deliberately
// strict: a file whose name contains a separator is a file that lands somewhere else.
bool nameable(const std::string& name) {
    if (name.empty() || name.size() > 120) return false;
    if (name == "." || name == "..") return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) return false;
        if (std::strchr("\\/:*?\"<>|", c) != nullptr) return false;
    }
    // Trailing dots and spaces are accepted by some file systems and then silently trimmed by
    // others, which turns one file into two on a copy between them.
    return name.back() != ' ' && name.back() != '.';
}

}  // namespace

std::filesystem::path default_root() {
    const std::string& dir = crash_log_dir();
    if (!dir.empty()) return std::filesystem::path(dir);
    // No platform folder — a portable install, or a test. Beside the working directory, which is
    // somewhere a player can find and a test can throw away.
    return std::filesystem::current_path() / "WorldShaper";
}

const std::vector<Kind>& shipped_kinds() {
    static const std::vector<Kind> kinds = {
        // A `.wsworld` is a clip script today, so its author tag is written as a comment and the
        // file still parses. The single-file container with append-only journaling is Stage 15's
        // own and is not built yet; when it lands, `text` here becomes false and the tag becomes a
        // block — which is exactly the one line this arrangement was shaped to make it.
        Kind{"worlds", ".wsworld", "worlds", Icon::World, true, "#"},
        // Characters are in HERE, and there is no characters shelf.
        //
        // There was one, holding `.wsclip` files — the same format, on a different shelf — and the
        // reason it was a different shelf was that a character is a different KIND of thing. It is
        // not. A character is a clip you can wear as well as one you can stamp into the world, and
        // which of those you do with it is a decision made when you use it, not a fact about the
        // file. So a shelf per use was a shelf that had to be guessed at on the way in: a player
        // who saved a figure to `clips\` could not wear it, and one who saved it to `characters\`
        // could not place it, and neither of those was a rule anybody wrote down — it was the
        // consequence of two folders holding one format.
        //
        // Two shelves for one thing also costs twice: a duplicate is in one of them, a search finds
        // half of what is there, and every operation in the library has two places to look.
        // `shipped` is the folder beside the executable the built-in ones are read from, in place,
        // for ever (D494). They are not copied to the player's shelf and never were meant to be:
        // a copy is a second thing to keep in step, it is a thing a player can delete and then not
        // have, and every one of them came back on the next launch anyway looking like the delete
        // had failed. The facility and the twenty-two pieces it is assembled out of are here.
        Kind{"clips", ".wsclip", "clips", Icon::Clip, true, "#", ".clip", "clips"},
        // And the voxel types the game ships with, listed beside the player's own and never copied
        // anywhere (D494). The shelf is the tool's palette now (D806), so a shelf with nothing on
        // it is a game a player cannot build in — Q and E always had SOMETHING to step through
        // because it came out of the open world, and a library has to be given a start.
        Kind{"materials", ".wsmat", "materials", Icon::Material, true, "#", "", "materials"},
        // There is no scripts shelf (D492). It held `.wslua`, which is Lua, which is what a mod is
        // written in — and `mods\` was already specified as *Lua and native packages*. So it was
        // two shelves for one format, exactly as characters and clips were, and the thing that
        // told them apart was how finished the file was rather than what kind of thing it is. The
        // shelf lists both spellings and anything on the old one moves here once.
        Kind{"mods", ".wsmod", "mods", Icon::Mod, true, "--", ".wslua"},
    };
    return kinds;
}

std::string read_author(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string head(kAuthorWindow, '\0');
    file.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<usize>(file.gcount()));

    const usize at = head.find(kAuthorMarker);
    if (at == std::string::npos) return {};
    usize start = at + std::strlen(kAuthorMarker);
    while (start < head.size() && (head[start] == ' ' || head[start] == '\t')) ++start;
    usize end = start;
    while (end < head.size() && head[end] != '\n' && head[end] != '\r' && head[end] != '\0') ++end;
    std::string author = head.substr(start, end - start);
    while (!author.empty() && (author.back() == ' ' || author.back() == '\t')) author.pop_back();
    return author;
}

bool write_author(const std::filesystem::path& path, const Kind& kind,
                  const std::string& author) {
    if (author.empty()) return false;
    // Read, prepend, write. A file this small is not worth an in-place edit, and a rewrite is the
    // only way to put a line at the front of a text file anyway.
    std::string body;
    {
        std::ifstream file(path, std::ios::binary);
        if (file) {
            body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
    }
    if (body.find(kAuthorMarker) != std::string::npos) return true;   // already stamped

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (kind.text) {
        out << kind.comment << " " << kAuthorMarker << " " << author << "\n";
    } else {
        out.write(kAuthorMarker, static_cast<std::streamsize>(std::strlen(kAuthorMarker)));
        out << " " << author << "\n";
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return out.good();
}

std::string cache_file_for(const std::filesystem::path& root, const std::string& world_path,
                           const char* suffix) {
    std::error_code error;
    const std::filesystem::path full =
        std::filesystem::absolute(std::filesystem::path(world_path), error);
    const std::string text = error ? world_path : full.lexically_normal().string();
    u64 hash = 0xCBF29CE484222325ull;
    for (char c : text) hash = hash_combine(hash, static_cast<u64>(static_cast<u8>(c)));
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "-%016llx", static_cast<unsigned long long>(hash));
    const std::filesystem::path into = root / "cache";
    std::filesystem::create_directories(into, error);
    return (into / (std::filesystem::path(world_path).stem().string() + stamp + suffix)).string();
}

std::vector<std::string> where_includes_live(const std::filesystem::path& root) {
    std::vector<std::string> folders;
    folders.push_back((std::filesystem::path(Window::base_path()) / "clips").string());
    folders.push_back((root / "clips").string());
    folders.push_back((root / "materials").string());
    return folders;
}

bool is_author_line(std::string_view line) {
    return line.find(kAuthorMarker) != std::string_view::npos;
}

std::string without_author(const std::string& text) {
    if (text.find(kAuthorMarker) == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    usize at = 0;
    while (at < text.size()) {
        usize end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string_view line(text.data() + at, end - at);
        if (line.find(kAuthorMarker) == std::string_view::npos) {
            out.append(line);
            if (end < text.size()) out.push_back('\n');
        }
        at = end + 1;
    }
    return out;
}

Library::Library(std::filesystem::path root) : root_(std::move(root)) {
    kind_ = shipped_kinds().front();
    here_ = root_ / kind_.folder;
}

void Library::ensure_folders() const {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    for (const Kind& kind : shipped_kinds()) {
        std::filesystem::create_directories(root_ / kind.folder, error);
    }
    // Where a built world is kept. Not beside the world any more (D493): a `.wsworld` is one file
    // and the shelf shows one thing, and what a build cost belongs with the other things this
    // machine worked out rather than in the folder a player keeps their work in.
    std::filesystem::create_directories(root_ / "cache", error);

    // Shelves that stopped existing hand what was on them to the shelf that took over.
    //
    // A shelf going away must not take a player's files off the screen with it, and both of these
    // were the same mistake: two folders holding one format, told apart by what somebody intended
    // to do with the file rather than by what the file is. Characters are clips you can wear
    // (D479); loose Lua is a mod that is not finished (D492). Each move happens once, because
    // afterwards the folder is not there to find, and the folder is only removed if it emptied —
    // a directory that would not move is one still holding something, and removing it would be
    // removing the thing it holds.
    const struct { const char* from; const char* into; const char* what; } kGone[]{
        {"characters", "clips", "characters"},
        {"scripts", "mods", "scripts"},
    };
    for (const auto& move : kGone) {
        const std::filesystem::path was = root_ / move.from;
        error.clear();
        if (!std::filesystem::is_directory(was, error)) {
            error.clear();
            continue;
        }
        u32 moved = 0;
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(was, error)) {
            std::error_code each;
            const bool folder = item.is_directory(each);
            const std::filesystem::path target =
                free_name(root_ / move.into, item.path().stem().string(),
                          folder ? std::string() : item.path().extension().string());
            std::filesystem::rename(item.path(), target, each);
            if (!each) ++moved;
        }
        error.clear();
        std::filesystem::remove(was, error);   // only ever succeeds when it is empty
        error.clear();
        if (moved > 0) {
            WS_LOG_INFO("library", "moved {} {} onto the {} shelf", moved, move.what, move.into);
        }
    }
}

void Library::set_shipped_root(std::filesystem::path where) {
    game_ = std::move(where);
    shipped_ = kind_.shipped.empty() ? std::filesystem::path() : game_ / kind_.shipped;
    listed_ = false;
    refresh(true);
}

void Library::open(const Kind& kind) {
    kind_ = kind;
    here_ = root_ / kind.folder;
    shipped_ = (kind_.shipped.empty() || game_.empty()) ? std::filesystem::path()
                                                        : game_ / kind_.shipped;
    listed_ = false;
    refresh(true);
    // Where the shelf is and what was on it, once per shelf opened.
    //
    // Nothing anywhere said this, and it is the first question every report about the library
    // turned out to need: *is the game looking where I am looking, and did it see what I see*. The
    // path matters as much as the count — `%LOCALAPPDATA%` is `AppData\Local` and `%APPDATA%` is
    // `AppData\Roaming`, and a player checking the wrong one of those finds nothing and is right
    // to conclude the game has lost their files.
    WS_LOG_INFO("library", "shelf '{}' at '{}': {} things", kind_.folder, here_.string(),
                entries_.size());
}

// A shelf has one root, or two once the game ships some of its own — and *up* has to stop at
// whichever one you are under.
//
// It stopped at the player's only, so entering a built-in folder and pressing up walked out of the
// game's clips folder, into the folder the executable is in, and from there to the root of the
// disk: every press showed a listing of somewhere the library has no business being. `at_top` was
// the whole of the bug, because `up` is written in terms of it.
const std::filesystem::path& Library::top_of_here() const {
    static const std::filesystem::path kNone;
    const std::filesystem::path mine = root_ / kind_.folder;
    if (within(here_, mine)) return top_mine_ = mine;
    if (!shipped_.empty() && within(here_, shipped_)) return top_mine_ = shipped_;
    return top_mine_ = mine;
}

bool Library::is_shipped(const std::filesystem::path& path) const {
    if (game_.empty()) return false;
    // The player's own root wins, and that is not a nicety: a portable install puts
    // `WorldShaper\` beside the working directory, which on a development build is the folder the
    // executable is in — so without this every file a player made would read as one the game
    // shipped, and the editor would refuse to save any of them.
    if (within(path, root_)) return false;
    return within(path, game_);
}

bool Library::within(const std::filesystem::path& what, const std::filesystem::path& root) {
    std::error_code error;
    const std::string inside = what.lexically_normal().generic_string();
    const std::string outer = root.lexically_normal().generic_string();
    if (inside.size() < outer.size()) return false;
    if (inside.compare(0, outer.size(), outer) != 0) return false;
    return inside.size() == outer.size() || inside[outer.size()] == '/';
}

bool Library::at_top() const { return here_ == top_of_here(); }

std::string Library::breadcrumb() const {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(here_, top_of_here(), error);
    if (error || relative.empty() || relative == ".") return kind_.label;
    return kind_.label + " / " + relative.generic_string();
}

void Library::enter(const Entry& entry) {
    if (!entry.folder) return;
    here_ = entry.path;
    listed_ = false;
    refresh(true);
}

void Library::up() {
    if (at_top()) return;
    const std::filesystem::path top = top_of_here();
    const std::filesystem::path parent = here_.parent_path();
    // Belt as well as braces: `at_top` above already stops it, and this makes leaving the root
    // impossible rather than merely not asked for. A library that can be walked out of is a file
    // manager over the whole disk, which is not what any of this is for.
    here_ = within(parent, top) ? parent : top;
    listed_ = false;
    refresh(true);
}

void Library::set_sort(Sort by, bool descending) {
    sort_ = by;
    descending_ = descending;
    list();
}

void Library::refresh(bool force) {
    std::error_code error;
    const auto when = std::filesystem::last_write_time(here_, error);
    if (!force && listed_ && !error && when == stamp_) return;
    if (!error) stamp_ = when;
    list();
    listed_ = true;
}

void Library::set_filter(std::string text) {
    std::string want = lowered(text);
    if (want == filter_) return;
    filter_ = std::move(want);
    list();
}

bool Library::worth_showing(const std::filesystem::path& folder, u32 depth) const {
    // Deep enough to find a piece inside a folder of pieces, shallow enough that a shelf listing
    // never walks somebody's whole disk. A folder nested deeper than this holding the only clip in
    // the tree is shown as empty rather than hidden, which is the harmless way to be wrong.
    if (depth > 3) return true;
    std::error_code error;
    bool anything = false;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(folder, error)) {
        std::error_code each;
        anything = true;
        if (item.is_directory(each)) {
            if (worth_showing(item.path(), depth + 1)) return true;
            continue;
        }
        const std::string suffix = lowered(item.path().extension().string());
        if (suffix == lowered(kind_.extension)) return true;
        if (!kind_.also.empty() && suffix == lowered(kind_.also)) return true;
    }
    // Empty is shown; full of things this shelf is not about is not.
    return !anything;
}

void Library::list() {
    entries_.clear();
    std::error_code error;

    // One folder or two: the player's, and — only at the top of a shelf that has any — the one the
    // game shipped. They are listed together on purpose. A player looking for the facility should
    // find it where clips are, not in a second place with a different name, and the built-in ones
    // are the examples the rest of the shelf is learned from.
    const auto read_folder = [&](const std::filesystem::path& from, bool shipped) {
        std::error_code walk;
        if (!std::filesystem::exists(from, walk) || walk) return;
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(from, walk)) {
            std::error_code each;
            Entry entry;
            entry.path = item.path();
            entry.name = item.path().filename().string();
            entry.folder = item.is_directory(each);
            entry.shipped = shipped;
            if (entry.folder) {
                // A folder with nothing of this kind anywhere in it is not a folder this shelf has
                // anything to say about. `worlds\facility\` carries a BRIEF.md and a `requests\`
                // full of notes; those are the *authoring* of the thing rather than the thing, and
                // a library that lists them is a library that has stopped being about clips. An
                // EMPTY folder is still shown, because that is one somebody just made.
                if (!worth_showing(item.path(), 0)) continue;
                entry.shown = entry.name;
            } else {
                // Only this shelf's own kinds are shown. A `.md` beside a clip is not a clip, a
                // `.txt` a player left in their clips folder is not one either, and listing either
                // would make *open* mean nothing.
                const std::string suffix = lowered(item.path().extension().string());
                const bool mine = suffix == lowered(kind_.extension);
                const bool also = !kind_.also.empty() && suffix == lowered(kind_.also);
                if (!mine && !also) continue;
                entry.shown = item.path().stem().string();
                entry.bytes = static_cast<u64>(item.file_size(each));
                entry.author = read_author(item.path());
            }
            // And the search, over the name a player reads rather than the one on disk.
            if (!filter_.empty() && lowered(entry.shown).find(filter_) == std::string::npos) {
                continue;
            }
            entry.modified = seconds_of(item.last_write_time(each));
            entries_.push_back(std::move(entry));
        }
    };

    read_folder(here_, false);
    if (!shipped_.empty() && at_top()) read_folder(shipped_, true);

    // Folders first, always, whatever the sort is. A folder is a place and a file is a thing, and
    // mixing them by size puts the way out of a folder somewhere in the middle of it.
    //
    // Written as a three-way compare rather than a chain of `<`. A comparator that is not a strict
    // weak ordering is undefined behaviour in std::sort and shows up as a crash inside the
    // standard library on a listing of a particular length — which is a fault nobody attributes to
    // the tie-break rule they wrote.
    const bool down = descending_;
    const Sort by = sort_;
    const auto compare_text = [](const std::string& a, const std::string& b) {
        const std::string x = lowered(a);
        const std::string y = lowered(b);
        return (x < y) ? -1 : (y < x) ? 1 : 0;
    };
    const auto compare = [&](const Entry& a, const Entry& b) {
        switch (by) {
            case Sort::Date:
                return (a.modified < b.modified) ? -1 : (b.modified < a.modified) ? 1 : 0;
            case Sort::Size:
                return (a.bytes < b.bytes) ? -1 : (b.bytes < a.bytes) ? 1 : 0;
            case Sort::Author:
                return compare_text(a.author, b.author);
            case Sort::Name:
            case Sort::Count:
            default:
                return compare_text(a.shown, b.shown);
        }
    };
    std::sort(entries_.begin(), entries_.end(), [&](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        int primary = compare(a, b);
        if (down) primary = -primary;
        if (primary != 0) return primary < 0;
        if (by == Sort::Name) return false;
        // A tie on date, author or size is broken by name, so a listing has one order rather than
        // whatever the file system happened to hand back this time.
        return compare_text(a.shown, b.shown) < 0;
    });
}

std::filesystem::path Library::free_name(const std::filesystem::path& into,
                                         const std::string& stem,
                                         const std::string& extension) const {
    std::filesystem::path candidate = into / (stem + extension);
    std::error_code error;
    u32 nth = 2;
    while (std::filesystem::exists(candidate, error)) {
        candidate = into / (stem + " " + std::to_string(nth) + extension);
        ++nth;
        if (nth > 9999) break;   // somebody has four thousand copies; stop rather than spin
    }
    return candidate;
}

std::string Library::make_folder(const std::string& name) {
    if (!nameable(name)) return "that is not a name a folder can have";
    std::error_code error;
    const std::filesystem::path made = here_ / name;
    if (std::filesystem::exists(made, error)) return "there is already something called that";
    if (!std::filesystem::create_directory(made, error)) return "could not make that folder";
    refresh(true);
    return {};
}

std::string Library::rename(const Entry& entry, const std::string& to) {
    if (entry.shipped) return entry.shown + " came with the game -- duplicate it to make it yours";
    if (!nameable(to)) return "that is not a name a file can have";
    const std::string extension = entry.folder ? std::string() : kind_.extension;
    const std::filesystem::path target = entry.path.parent_path() / (to + extension);
    if (target == entry.path) return {};
    std::error_code error;
    if (std::filesystem::exists(target, error)) return "there is already something called that";
    std::filesystem::rename(entry.path, target, error);
    if (error) return "could not rename it";
    refresh(true);
    return {};
}

std::string Library::duplicate(const std::vector<Entry>& entries) {
    std::error_code error;
    for (const Entry& entry : entries) {
        const std::string extension = entry.folder ? std::string() : kind_.extension;
        // A copy of a built-in one lands on the PLAYER's shelf, never back in the game's own
        // folder. That is what makes *duplicate* the way to edit something the game shipped: the
        // original stays exactly as it shipped and the copy is yours, with your name on it, in the
        // place your work lives. Writing beside the executable would also fail outright on any
        // install a player does not own.
        const std::filesystem::path into =
            entry.shipped ? (root_ / kind_.folder) : entry.path.parent_path();
        const std::filesystem::path target = free_name(into, entry.shown, extension);
        if (entry.folder) {
            std::filesystem::copy(entry.path, target,
                                  std::filesystem::copy_options::recursive, error);
        } else {
            std::filesystem::copy_file(entry.path, target, error);
        }
        // A copy keeps the author it was made by, because the tag is IN the file and a copy is a
        // copy of the file. That is the whole of D447's mechanism, and it is worth noticing that
        // it took no code here at all.
        if (error) return "could not copy " + entry.shown;
    }
    refresh(true);
    return {};
}

std::string Library::built_from(const std::string& folder) const {
    if (folder.empty()) return {};
    std::error_code error;
    const std::string mark = folder + "/";
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(here_, error)) {
        if (!item.is_regular_file(error)) continue;
        if (item.path().extension() != kind_.extension) continue;
        std::ifstream in(item.path(), std::ios::binary);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            // The same reading `expand_includes` does, and deliberately no more than that: an
            // include is a quoted name on a line that starts with the word, so finding one does not
            // need the parser and cannot disagree with it about what a comment is.
            const usize at = line.find_first_not_of(" \t");
            if (at == std::string::npos || line.compare(at, 7, "include") != 0) continue;
            const usize quote = line.find('"', at + 7);
            if (quote == std::string::npos) continue;
            const usize close = line.find('"', quote + 1);
            if (close == std::string::npos) continue;
            std::string named = line.substr(quote + 1, close - quote - 1);
            for (char& c : named) {
                if (c == '\\') c = '/';
            }
            if (named.rfind(mark, 0) == 0) return item.path().stem().string();
        }
    }
    return {};
}

std::string Library::erase(const std::vector<Entry>& entries) {
    std::error_code error;

    // Nothing is moved until every check has passed, because half a delete on a multiple selection
    // is the state nobody can undo by hand.
    for (const Entry& entry : entries) {
        if (entry.shipped) {
            return entry.shown + " came with the game, so it cannot be deleted";
        }
    }
    //
    // A folder that a world on this shelf is BUILT OUT OF is refused, and the refusal names the
    // world. That is the whole of the failure this exists for: the pieces of a building look like
    // an ordinary folder sitting next to an ordinary file, deleting them leaves a world that opens
    // as an empty sky, and nothing anywhere said the two were connected. Unless the world is in the
    // same selection — deleting a world and its own parts together is exactly right.
    for (const Entry& entry : entries) {
        if (!entry.folder) continue;
        const std::string needed = built_from(entry.name);
        if (needed.empty()) continue;
        bool going_too = false;
        for (const Entry& other : entries) {
            if (!other.folder && other.path.stem().string() == needed) going_too = true;
        }
        if (!going_too) {
            return needed + " is built out of " + entry.shown + ", so it would open empty";
        }
    }

    for (const Entry& entry : entries) {
        // What the file cannot be opened without goes with it: the folder of pieces it is
        // assembled out of, and the world already built from it. Worked out BEFORE anything moves,
        // because `built_from` reads the shelf and the file it would read is the one about to
        // leave it.
        std::filesystem::path pieces;
        if (!entry.folder) {
            const std::filesystem::path beside = entry.path.parent_path() / entry.path.stem();
            std::error_code look;
            if (std::filesystem::is_directory(beside, look) && !look &&
                built_from(beside.filename().string()) == entry.path.stem().string()) {
                pieces = beside;
            }
        }

        // To the system's own recycle bin (D491), not to a folder of ours.
        //
        // A delete a player cannot undo is not what they pressed, and that was the whole argument
        // for `trash\`. The argument was right and the answer was wrong: every player already has
        // an undoable delete, it is where every other file on their machine goes, they know how to
        // open it and how to empty it, and it survives the game not running. A second one inside
        // the game was a second place to look, a second thing to explain, and a shelf that filled
        // with things a player thought they had thrown away.
        if (!send_to_recycle_bin(entry.path)) {
            // Never a fall-back to deleting outright. "Could not put it where you can get it back"
            // and "threw it away" are different answers and only one of them was asked for.
            return "could not put " + entry.shown + " in the recycle bin";
        }
        if (entry.folder) continue;

        for (const char* sidecar : {".world", ".load"}) {
            const std::filesystem::path from = entry.path.string() + sidecar;
            std::error_code beside;
            if (std::filesystem::exists(from, beside) && !beside) send_to_recycle_bin(from);
        }
        // **And what this machine worked out about it, which does not live beside it** (D493).
        //
        // The cache is named for the world's PATH, so leaving it behind does not merely waste the
        // disk: the next world to take that name finds it, and because both were made by pressing
        // *new* they start from the same text, so its key matches and the new world opens as the
        // old one. Reported as a deleted world coming back. Deleted outright rather than sent to
        // the bin — it is not the player's, it is derived, and it is worthless anywhere else.
        for (const char* suffix : {".world", ".load"}) {
            std::error_code derived;
            std::filesystem::remove(cache_file_for(root_, entry.path.string(), suffix), derived);
        }
        if (!pieces.empty()) send_to_recycle_bin(pieces);
    }
    refresh(true);
    return {};
}

std::string Library::move(const std::vector<Entry>& entries, const std::filesystem::path& into) {
    std::error_code error;
    if (!std::filesystem::is_directory(into, error)) return "that is not a folder";
    for (const Entry& entry : entries) {
        // Moving a folder into itself is the one move that destroys what it moves.
        if (entry.folder) {
            const std::filesystem::path from = std::filesystem::weakly_canonical(entry.path, error);
            const std::filesystem::path to = std::filesystem::weakly_canonical(into, error);
            const std::string from_text = from.generic_string();
            const std::string to_text = to.generic_string();
            if (to_text.rfind(from_text, 0) == 0) return "a folder cannot go inside itself";
        }
        const std::string extension = entry.folder ? std::string() : entry.path.extension().string();
        const std::filesystem::path target = free_name(into, entry.shown, extension);
        std::filesystem::rename(entry.path, target, error);
        if (error) return "could not move " + entry.shown;
    }
    refresh(true);
    return {};
}

std::string Library::create(const std::string& name, const std::string& author,
                            const std::string& contents) {
    if (!nameable(name)) return "that is not a name a file can have";
    std::error_code error;
    const std::filesystem::path target = here_ / (name + kind_.extension);
    if (std::filesystem::exists(target, error)) return "there is already something called that";
    {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) return "could not make that file";
        file << contents;
    }
    if (!write_author(target, kind_, author)) {
        WS_LOG_WARN("library", "could not stamp {} with its author", target.string());
    }
    refresh(true);
    return {};
}

}  // namespace ws::ui
