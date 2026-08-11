#include "core/crash.hpp"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>

#include "core/log.hpp"
#include "core/version.hpp"

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN and NOMINMAX come from the build's own compile definitions.
#include <windows.h>
// These two have to follow windows.h; they use types it defines.
#include <dbghelp.h>
#include <psapi.h>
#endif

namespace ws {
namespace {

std::string g_dir;
// Where reports go. A folder of its own under `g_dir`, because two files per crash in the root
// buried the six things a player has reason to open under a hundred and fifty they do not.
std::string g_report_dir;
std::string g_log_path;
std::atomic<bool> g_dialog{true};

// One report per process. Two threads faulting at once would otherwise interleave into a
// file that describes neither, and DbgHelp is not safe to call from two threads anyway.
std::atomic<bool> g_reporting{false};

// Breadcrumbs. Read without a lock from the handler, for the same reason the log ring is:
// the faulting thread may be the one holding the lock.
constexpr int kMaxContext = 16;
struct ContextEntry {
    char key[48];
    char value[208];
};
ContextEntry g_context[kMaxContext];
std::atomic<int> g_context_count{0};
std::mutex g_context_mutex;

// Assembled in a fixed buffer. A crash handler that allocates can fail inside the failure.
constexpr usize kReportCapacity = 256 * 1024;
char g_report[kReportCapacity];
usize g_report_used = 0;

void emit(const char* fmt, ...) {
    if (g_report_used + 1 >= kReportCapacity) return;
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(g_report + g_report_used, kReportCapacity - g_report_used,
                                 fmt, args);
    va_end(args);
    if (n > 0) {
        const usize room = kReportCapacity - g_report_used - 1;
        g_report_used += (static_cast<usize>(n) < room) ? static_cast<usize>(n) : room;
    }
}

void emit_context_lines() {
    const int count = g_context_count.load(std::memory_order_acquire);
    if (count <= 0) return;
    emit("\nContext\n");
    for (int i = 0; i < count && i < kMaxContext; ++i) {
        emit("  %-20s %s\n", g_context[i].key, g_context[i].value);
    }
}

void emit_recent_log() {
    emit("\nLast log lines\n");
    // Reuse the tail of the report buffer as scratch, then append. Nothing else is running.
    static char tail[128 * 1024];
    log_recent(tail, sizeof(tail));
    emit("%s", tail);
}

#if defined(_WIN32)

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
        case EXCEPTION_FLT_DENORMAL_OPERAND:  return "float denormal operand";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "float divide by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
        case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW:          return "integer overflow";
        case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
        case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case 0xE06D7363:                      return "unhandled C++ exception";
        default:                              return "unknown";
    }
}

void emit_fault(EXCEPTION_RECORD* record) {
    // Code zero is the synthetic record used by crash_write_report; there is no fault to
    // describe there, only a stack worth capturing.
    if (!record || record->ExceptionCode == 0) return;
    emit("Fault      %s (0x%08lX)\n", exception_name(record->ExceptionCode),
         static_cast<unsigned long>(record->ExceptionCode));
    emit("At         0x%016llX\n",
         static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(record->ExceptionAddress)));
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        // Which way, and where. "Read of 0x0" says null pointer without any further work;
        // a huge address says a bad index rather than a missing object.
        const ULONG_PTR kind = record->ExceptionInformation[0];
        const char* verb = kind == 0 ? "read of" : (kind == 1 ? "write to" : "execute at");
        emit("Access     %s 0x%016llX\n", verb,
             static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
}

void emit_stack(CONTEXT* context) {
    emit("\nCall stack\n");
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE)) {
        emit("  (symbols unavailable: SymInitialize failed, %lu)\n", GetLastError());
    }

    // StackWalk64 can modify the context it walks, so give it a copy; the minidump written
    // afterwards needs the original untouched.
    CONTEXT walk = *context;
    STACKFRAME64 frame{};
#if defined(_M_X64)
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = walk.Rip;
    frame.AddrFrame.Offset = walk.Rbp;
    frame.AddrStack.Offset = walk.Rsp;
#elif defined(_M_ARM64)
    const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = walk.Pc;
    frame.AddrFrame.Offset = walk.Fp;
    frame.AddrStack.Offset = walk.Sp;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = walk.Eip;
    frame.AddrFrame.Offset = walk.Ebp;
    frame.AddrStack.Offset = walk.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);

    for (int depth = 0; depth < 64; ++depth) {
        if (!StackWalk64(machine, process, thread, &frame, &walk, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) break;

        // Module plus offset first, and always. A player's machine has no .pdb, so the
        // symbol lookup below will fail there; module+offset still pins the crash down
        // once matched against the build's own symbols.
        char module_name[MAX_PATH] = "?";
        DWORD64 module_base = 0;
        IMAGEHLP_MODULE64 module_info{};
        module_info.SizeOfStruct = sizeof(module_info);
        if (SymGetModuleInfo64(process, pc, &module_info)) {
            std::snprintf(module_name, sizeof(module_name), "%s", module_info.ModuleName);
            module_base = module_info.BaseOfImage;
        }
        emit("  %2d  %-24s +0x%-8llX", depth, module_name,
             static_cast<unsigned long long>(module_base ? pc - module_base : pc));

        std::memset(symbol, 0, sizeof(SYMBOL_INFO));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, pc, &displacement, symbol)) {
            emit("  %s +0x%llX", symbol->Name,
                 static_cast<unsigned long long>(displacement));
        }
        DWORD line_displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(process, pc, &line_displacement, &line)) {
            emit("\n      %s:%lu", line.FileName, line.LineNumber);
        }
        emit("\n");
    }
    SymCleanup(process);
}

void emit_system() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        emit("Memory     %llu MB free of %llu MB\n",
             static_cast<unsigned long long>(memory.ullAvailPhys >> 20),
             static_cast<unsigned long long>(memory.ullTotalPhys >> 20));
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    // Pulled by name so there is no psapi link dependency for a diagnostic nicety.
    if (HMODULE psapi = GetModuleHandleA("kernel32.dll")) {
        using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
        auto fn = reinterpret_cast<GetProcessMemoryInfoFn>(
            reinterpret_cast<void*>(GetProcAddress(psapi, "K32GetProcessMemoryInfo")));
        if (fn && fn(GetCurrentProcess(),
                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            emit("Process    %llu MB working set\n",
                 static_cast<unsigned long long>(counters.WorkingSetSize >> 20));
        }
    }
}

void write_file(const std::string& path, const char* data, usize size) {
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(file);
}

void write_minidump(const std::string& path, EXCEPTION_POINTERS* pointers) {
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (!dbghelp) return;
    using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                      PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION,
                                      PMINIDUMP_CALLBACK_INFORMATION);
    auto write_dump = reinterpret_cast<WriteDumpFn>(
        reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (!write_dump) return;

    const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = pointers;
    info.ClientPointers = FALSE;

    // Enough to inspect locals and the objects they point at, without dumping the whole
    // address space: a voxel game's heap is measured in gigabytes.
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithDataSegs |
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
               pointers ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

std::string timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    // Milliseconds and the process id, because two crashes in the same second must not
    // overwrite each other -- and the second report is often the more interesting one.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02u-%02u%02u%02u.%03u-%lu", now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                  now.wMilliseconds, GetCurrentProcessId());
    return buffer;
}

std::string build_report(EXCEPTION_POINTERS* pointers, const char* reason) {
    g_report_used = 0;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    emit("WorldShaper %s crashed\n", kVersion);
    emit("===============================================================\n");
    emit("When       %04u-%02u-%02u %02u:%02u:%02u\n", now.wYear, now.wMonth, now.wDay,
         now.wHour, now.wMinute, now.wSecond);
    emit("Reason     %s\n", reason);
    emit("Thread     %lu\n", GetCurrentThreadId());
    if (pointers) emit_fault(pointers->ExceptionRecord);
    emit_system();
    emit_context_lines();

    if (pointers && pointers->ContextRecord) {
        emit_stack(pointers->ContextRecord);
    } else {
        CONTEXT here{};
        RtlCaptureContext(&here);
        emit_stack(&here);
    }
    emit_recent_log();

    const std::string stamp = timestamp();
    const std::string& into = g_report_dir.empty() ? g_dir : g_report_dir;
    const std::string report_path = into + "crash-" + stamp + ".log";
    write_file(report_path, g_report, g_report_used);
    write_minidump(into + "crash-" + stamp + ".dmp", pointers);
    return report_path;
}

void announce(const std::string& path) {
    // Straight to stderr as well, because a developer running from a terminal should not
    // have to dismiss a dialog to see what happened.
    std::fprintf(stderr, "\n[FATAL] crash report written to %s\n", path.c_str());
    std::fflush(stderr);
    if (!g_dialog.load(std::memory_order_relaxed)) return;
    const std::string text =
        "WorldShaper has crashed.\n\nA report was written to:\n" + path +
        "\n\nSending that file, and the .dmp beside it, is what makes this fixable.";
    MessageBoxA(nullptr, text.c_str(), "WorldShaper crashed", MB_OK | MB_ICONERROR);
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* pointers) {
    if (g_reporting.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    announce(build_report(pointers, "unhandled exception"));
    return EXCEPTION_EXECUTE_HANDLER;
}

void report_and_die(const char* reason) {
    if (g_reporting.exchange(true)) return;
    announce(build_report(nullptr, reason));
    // Past the handlers, straight out. Returning would re-enter whatever was already broken.
    TerminateProcess(GetCurrentProcess(), 3);
}

void on_signal(int signal_number) {
    // WS_CHECK ends in std::abort(), which lands here. The failed condition is already the
    // last line of the log ring, so the report explains itself.
    report_and_die(signal_number == SIGABRT ? "abort (failed check or assertion)"
                                            : "fatal signal");
}

void on_terminate() { report_and_die("std::terminate (uncaught exception)"); }

void on_purecall() { report_and_die("pure virtual call"); }

void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                          uintptr_t) {
    // The CRT's default for this is to kill the process without a word.
    report_and_die("invalid CRT parameter");
}

std::string local_app_data_dir() {
    char base[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::string dir = std::string(base) + "\\WorldShaper\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

#endif  // _WIN32

}  // namespace

const std::string& crash_log_dir() { return g_dir; }
const std::string& crash_log_path() { return g_log_path; }

void crash_set_dialog(bool enabled) { g_dialog.store(enabled, std::memory_order_relaxed); }

void crash_set_context(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    const int count = g_context_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        if (std::string_view(g_context[i].key) == key) {
            std::snprintf(g_context[i].value, sizeof(g_context[i].value), "%.*s",
                          static_cast<int>(value.size()), value.data());
            return;
        }
    }
    if (count >= kMaxContext) return;
    std::snprintf(g_context[count].key, sizeof(g_context[count].key), "%.*s",
                  static_cast<int>(key.size()), key.data());
    std::snprintf(g_context[count].value, sizeof(g_context[count].value), "%.*s",
                  static_cast<int>(value.size()), value.data());
    g_context_count.store(count + 1, std::memory_order_release);
}

#if defined(_WIN32)

// Everything the old layout left in the root, moved once, on the first run that finds it. A folder
// that only tidies what it makes from now on leaves the mess that made it necessary.
static void sweep_old_reports_into(const std::string& from, const std::string& into) {
    WIN32_FIND_DATAA found{};
    const std::string pattern = from + "crash-*";
    HANDLE handle = FindFirstFileA(pattern.c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return;
    unsigned moved = 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const std::string was = from + found.cFileName;
        const std::string now = into + found.cFileName;
        if (MoveFileExA(was.c_str(), now.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) ++moved;
    } while (FindNextFileA(handle, &found) != 0);
    FindClose(handle);
    if (moved > 0) {
        std::fprintf(stderr, "[info ] crash    moved %u old reports into crashes\\\n", moved);
    }
}

void crash_install(std::string_view log_name) {
    g_dir = local_app_data_dir();
    if (g_dir.empty()) return;
    // Reports go in a folder of their own, and the folder is swept of what was there before it
    // existed.
    //
    // A crash writes two files and a session that crashes repeatedly writes two more each time, so
    // the root filled with them — a hundred and fifty `crash-*.dmp` and `.log` beside the six
    // things a player actually has reason to open. The shelves are already folders and the log is
    // one file; this was the only thing in there with no home.
    g_report_dir = g_dir + "crashes\\";
    CreateDirectoryA(g_report_dir.c_str(), nullptr);
    sweep_old_reports_into(g_dir, g_report_dir);
    g_log_path = g_dir + std::string(log_name);
    // Keep one generation back. A player who crashes and relaunches before saying anything
    // would otherwise have already overwritten the log that explains it.
    MoveFileExA(g_log_path.c_str(), (g_log_path + ".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
    log_open_file(g_log_path.c_str());

    SetUnhandledExceptionFilter(on_exception);
    std::set_terminate(on_terminate);
    _set_purecall_handler(on_purecall);
    _set_invalid_parameter_handler(on_invalid_parameter);
    // SIGABRT only. The CRT will happily translate an access violation into SIGSEGV if a
    // handler is installed, and that path throws away the exception record: no "read of
    // 0x0", and a stack that starts inside the signal machinery instead of at the fault.
    // Left alone, those faults reach the filter above with everything intact.
    std::signal(SIGABRT, on_signal);
    // Without this the CRT shows its own "abnormal program termination" box first and the
    // handler above never runs.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // A stack overflow arrives with almost no stack left. Reserving a slice up front is
    // what lets the handler run at all rather than faulting again inside itself.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);

    WS_LOG_INFO("crash", "log at {}", g_log_path);
}

std::string crash_write_report(const char* reason) {
    const bool already = g_reporting.exchange(true);
    CONTEXT here{};
    RtlCaptureContext(&here);
    EXCEPTION_RECORD record{};
    EXCEPTION_POINTERS pointers{&record, &here};
    const std::string path = build_report(&pointers, reason);
    if (!already) g_reporting.store(false, std::memory_order_relaxed);
    return path;
}

#else

void crash_install(std::string_view) {}
std::string crash_write_report(const char*) { return {}; }

#endif

}  // namespace ws
