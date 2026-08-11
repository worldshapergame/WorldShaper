#include "platform/desktop.hpp"

#include <system_error>

#include "core/log.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// shellapi.h after windows.h, and both before anything of ours: the order is what the header
// expects and getting it wrong is a hundred errors inside the SDK rather than one here.
#include <shellapi.h>
#endif

namespace ws {

#if defined(_WIN32)

bool open_in_file_manager(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) return false;
    // `explorer` rather than a select-and-reveal, because what is being asked for here is *the
    // folder*, and a window with one file picked in it is an answer to a different question.
    const std::wstring wide = path.wstring();
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // The return is a legacy HINSTANCE used as an error code, and anything over 32 is success.
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool send_to_recycle_bin(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) return false;

    // SHFileOperation wants a list terminated by TWO nulls, and it wants an absolute path — a
    // relative one is resolved against the process's working directory, which is not where the
    // player thinks their files are.
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return false;
    std::wstring wide = absolute.wstring();
    wide.push_back(L'\0');
    wide.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = wide.c_str();
    // ALLOWUNDO is the whole point of this function: without it this is `remove_all` with extra
    // steps. NOCONFIRMATION because the interface has already been the confirmation, and
    // NOERRORUI because a modal box from the shell over a full-screen game is a box nobody can
    // reach — what a failure has to do is come back as `false` and be said in the interface.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                FOF_NOCONFIRMMKDIR;
    const int result = SHFileOperationW(&op);
    if (result != 0 || op.fAnyOperationsAborted) {
        WS_LOG_WARN("library", "the recycle bin refused '{}' (code {})", path.string(), result);
        return false;
    }
    return true;
}

#else

// Neither is worth a partial implementation. A "recycle" that deletes is worse than one that
// refuses, and a file manager that might be any of six is a guess. Both say no, the caller says so
// in one line, and the day this game has a second platform is the day this file grows a branch.
bool open_in_file_manager(const std::filesystem::path&) { return false; }
bool send_to_recycle_bin(const std::filesystem::path&) { return false; }

#endif

}  // namespace ws
