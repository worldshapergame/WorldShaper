#include "ui/library.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>

#include "core/crash.hpp"
#include "core/log.hpp"

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
        Kind{"clips", ".wsclip", "clips", Icon::Clip, true, "#"},
        Kind{"materials", ".wsmat", "materials", Icon::Material, true, "#"},
        Kind{"characters", ".wsclip", "characters", Icon::Character, true, "#"},
        Kind{"mods", ".wsmod", "mods", Icon::Mod, false, "#"},
        Kind{"scripts", ".wslua", "scripts", Icon::Script, true, "--"},
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
    std::filesystem::create_directories(trash(), error);
}

void Library::open(const Kind& kind) {
    kind_ = kind;
    here_ = root_ / kind.folder;
    listed_ = false;
    refresh(true);
}

bool Library::at_top() const { return here_ == root_ / kind_.folder; }

std::string Library::breadcrumb() const {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(here_, root_ / kind_.folder, error);
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
    here_ = here_.parent_path();
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

void Library::list() {
    entries_.clear();
    std::error_code error;
    if (!std::filesystem::exists(here_, error)) return;

    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(here_, error)) {
        std::error_code each;
        Entry entry;
        entry.path = item.path();
        entry.name = item.path().filename().string();
        entry.folder = item.is_directory(each);
        if (entry.folder) {
            entry.shown = entry.name;
        } else {
            // Only this shelf's own kind is shown. A `.txt` a player left in their clips folder is
            // not a clip, and listing it would make *open* mean nothing.
            if (lowered(item.path().extension().string()) != lowered(kind_.extension)) continue;
            entry.shown = item.path().stem().string();
            entry.bytes = static_cast<u64>(item.file_size(each));
            entry.author = read_author(item.path());
        }
        entry.modified = seconds_of(item.last_write_time(each));
        entries_.push_back(std::move(entry));
    }

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
        const std::filesystem::path target =
            free_name(entry.path.parent_path(), entry.shown, extension);
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

std::string Library::erase(const std::vector<Entry>& entries) {
    std::error_code error;
    std::filesystem::create_directories(trash(), error);
    for (const Entry& entry : entries) {
        const std::string extension = entry.folder ? std::string() : entry.path.extension().string();
        const std::filesystem::path target = free_name(trash(), entry.shown, extension);
        std::filesystem::rename(entry.path, target, error);
        if (error) {
            // Across volumes a rename fails and a copy-then-remove is the only way. A trash that
            // gives up on a file stored somewhere else is a trash a player cannot trust.
            error.clear();
            std::filesystem::copy(entry.path, target,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  error);
            if (!error) std::filesystem::remove_all(entry.path, error);
            if (error) return "could not move " + entry.shown + " to the trash";
        }
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
