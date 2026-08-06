#include "app/updater.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/log.hpp"
#include "core/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace ws {
namespace {

#if defined(_WIN32)

// A tiny reader for the two fields we need out of GitHub's reply. Not a JSON parser and not
// pretending to be one: it finds `"key":` and returns the string that follows.
//
// A parser would be the right tool if this read arbitrary documents. It reads one reply from
// one endpoint, and the two values it takes are checked before they are used for anything —
// the version has to look like a version, and the download URL has to start with the exact
// prefix a release asset of this repository has. A malformed or hostile reply fails those
// checks, which is the part that actually matters.
std::string field(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    usize at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    at = json.find('"', at);
    if (at == std::string::npos) return {};
    ++at;
    std::string value;
    while (at < json.size() && json[at] != '"') {
        if (json[at] == '\\' && at + 1 < json.size()) ++at;
        value += json[at++];
    }
    return value;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<usize>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed);
    return out;
}

struct Handles {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~Handles() {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

// One HTTPS GET, following redirects, with certificate checking left alone. `body` receives
// the reply; `to_file` receives it instead when it is not null, so a release asset does not
// have to be held in memory to be written to disk.
bool https_get(const std::wstring& host, const std::wstring& path, std::string* body,
               std::ofstream* to_file, std::atomic<f32>* progress, u64 expected_bytes) {
    Handles h;
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY means the machine's own proxy settings are used,
    // which is what a player behind a corporate or school network needs.
    h.session = WinHttpOpen(L"WorldShaper", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) return false;

    // Nothing here should be able to hang the thread for long.
    WinHttpSetTimeouts(h.session, 5000, 5000, 15000, 30000);

    h.connection = WinHttpConnect(h.session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!h.connection) return false;

    h.request = WinHttpOpenRequest(h.connection, L"GET", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!h.request) return false;

    // GitHub's API refuses requests without a user agent, and asks for an explicit accept.
    const wchar_t* headers = L"User-Agent: WorldShaper\r\nAccept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(h.request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(h.request, nullptr)) return false;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return false;

    std::vector<char> buffer(64 * 1024);
    u64 total = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(h.request, &available)) return false;
        if (available == 0) break;
        while (available > 0) {
            const DWORD want = (available < buffer.size()) ? available
                                                           : static_cast<DWORD>(buffer.size());
            DWORD read = 0;
            if (!WinHttpReadData(h.request, buffer.data(), want, &read) || read == 0) {
                return false;
            }
            if (body) body->append(buffer.data(), read);
            if (to_file) to_file->write(buffer.data(), static_cast<std::streamsize>(read));
            total += read;
            available -= read;
            if (progress && expected_bytes > 0) {
                progress->store(static_cast<f32>(static_cast<f64>(total) /
                                                 static_cast<f64>(expected_bytes)),
                                std::memory_order_relaxed);
            }
        }
    }
    return total > 0;
}

std::filesystem::path executable_path() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length));
}

// A version has to look like one before it is compared, so that a reply saying anything else
// is a failed check rather than an update to nowhere.
bool parse_version(const std::string& text, u32 out[3]) {
    out[0] = out[1] = out[2] = 0;
    usize at = 0;
    for (u32 part = 0; part < 3; ++part) {
        if (at >= text.size() || text[at] < '0' || text[at] > '9') return false;
        u32 value = 0;
        u32 digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 6) {
            value = value * 10 + static_cast<u32>(text[at] - '0');
            ++at;
            ++digits;
        }
        out[part] = value;
        if (part < 2) {
            if (at >= text.size() || text[at] != '.') return false;
            ++at;
        }
    }
    return at == text.size();
}

#endif  // _WIN32

}  // namespace

Updater::~Updater() { join(); }

void Updater::join() {
    if (worker_.joinable()) worker_.join();
}

void Updater::clean_up_previous() {
#if defined(_WIN32)
    std::error_code error;
    const std::filesystem::path previous = executable_path().replace_extension(".old.exe");
    if (std::filesystem::exists(previous, error)) {
        std::filesystem::remove(previous, error);
        if (!error) WS_LOG_INFO("update", "removed the previous version left by an update");
    }
#endif
}

void Updater::begin_check() {
#if defined(_WIN32)
    if (state() == UpdateState::Checking || state() == UpdateState::Downloading) return;
    join();
    state_.store(UpdateState::Checking, std::memory_order_release);

    worker_ = std::thread([this] {
        const std::wstring path = L"/repos/" + widen(kRepositoryOwner) + L"/" +
                                  widen(kRepositoryName) + L"/releases/latest";
        std::string body;
        if (!https_get(L"api.github.com", path, &body, nullptr, nullptr, 0)) {
            message_ = "could not reach GitHub";
            state_.store(UpdateState::Failed, std::memory_order_release);
            return;
        }

        const std::string tag = field(body, "tag_name");
        if (tag.size() < 2 || tag[0] != 'v') {
            message_ = "the latest release has no version on it";
            state_.store(UpdateState::Failed, std::memory_order_release);
            return;
        }
        const std::string version = tag.substr(1);
        u32 parts[3]{};
        if (!parse_version(version, parts)) {
            message_ = "the latest release's version does not look like one";
            state_.store(UpdateState::Failed, std::memory_order_release);
            return;
        }

        if (version_ordinal(parts[0], parts[1], parts[2]) <= kVersionOrdinal) {
            message_ = "this is the newest version";
            state_.store(UpdateState::UpToDate, std::memory_order_release);
            return;
        }

        // The download URL is checked against the shape a release asset of *this* repository
        // has, so a reply cannot point the download at somewhere else.
        const std::string url = field(body, "browser_download_url");
        const std::string expected = std::string("https://github.com/") + kRepositoryOwner +
                                     "/" + kRepositoryName + "/releases/download/";
        if (url.rfind(expected, 0) != 0) {
            message_ = "the release has no download that belongs to this project";
            state_.store(UpdateState::Failed, std::memory_order_release);
            return;
        }

        info_.available = true;
        info_.version = version;
        info_.tag = tag;
        info_.download_url = url;
        info_.notes = field(body, "name");
        const std::string size = field(body, "size");
        info_.size_bytes = size.empty() ? 0 : std::strtoull(size.c_str(), nullptr, 10);
        message_ = "version " + version + " is available";
        state_.store(UpdateState::Available, std::memory_order_release);
        WS_LOG_INFO("update", "{} is available (this build is {})", tag, kVersionTag);
    });
#else
    state_.store(UpdateState::UpToDate, std::memory_order_release);
#endif
}

void Updater::begin_download() {
#if defined(_WIN32)
    if (state() != UpdateState::Available) return;
    join();
    progress_.store(0.0f, std::memory_order_relaxed);
    state_.store(UpdateState::Downloading, std::memory_order_release);

    worker_ = std::thread([this] {
        // Split the URL that was already checked against the expected prefix.
        const std::string& url = info_.download_url;
        const usize host_at = url.find("://");
        const usize path_at = url.find('/', host_at + 3);
        const std::wstring host = widen(url.substr(host_at + 3, path_at - host_at - 3));
        const std::wstring path = widen(url.substr(path_at));

        const std::filesystem::path exe = executable_path();
        const std::filesystem::path incoming = std::filesystem::path(exe).replace_extension(
            ".new.exe");

        {
            std::ofstream file(incoming, std::ios::binary | std::ios::trunc);
            if (!file) {
                message_ = "could not write next to the game - is it in a protected folder?";
                state_.store(UpdateState::Failed, std::memory_order_release);
                return;
            }
            if (!https_get(host, path, nullptr, &file, &progress_, info_.size_bytes)) {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(incoming, ignored);
                message_ = "the download did not finish";
                state_.store(UpdateState::Failed, std::memory_order_release);
                return;
            }
        }

        // It has to at least be a Windows executable before it is put in place. This is a
        // sanity check on a truncated or wrong download, not a security check — that is what
        // HTTPS to a checked URL is for.
        {
            std::ifstream check(incoming, std::ios::binary);
            char magic[2]{};
            check.read(magic, 2);
            if (magic[0] != 'M' || magic[1] != 'Z') {
                check.close();
                std::error_code ignored;
                std::filesystem::remove(incoming, ignored);
                message_ = "what arrived was not a program";
                state_.store(UpdateState::Failed, std::memory_order_release);
                return;
            }
        }

        // Windows will not let a running executable be overwritten, but it will let it be
        // renamed. So: step aside, move the new one in, and sweep up on the next start.
        std::error_code error;
        const std::filesystem::path previous =
            std::filesystem::path(exe).replace_extension(".old.exe");
        std::filesystem::remove(previous, error);
        std::filesystem::rename(exe, previous, error);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(incoming, ignored);
            message_ = "could not move the running game aside";
            state_.store(UpdateState::Failed, std::memory_order_release);
            return;
        }
        std::filesystem::rename(incoming, exe, error);
        if (error) {
            // Put the old one back rather than leaving nothing behind.
            std::error_code ignored;
            std::filesystem::rename(previous, exe, ignored);
            message_ = "could not put the new version in place";
            state_.store(UpdateState::Failed, std::memory_order_release);
            return;
        }

        message_ = "version " + info_.version + " installed - restart to use it";
        state_.store(UpdateState::Installed, std::memory_order_release);
        WS_LOG_INFO("update", "installed {}; it takes effect on the next start", info_.tag);
    });
#endif
}

}  // namespace ws
