#include "crash_log.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dbghelp.h>

namespace {

std::mutex g_mutex;
std::deque<std::string> g_notes;
std::atomic<unsigned long long> g_beat{0};
std::atomic<bool> g_reporting{false};
HANDLE g_mainThread = nullptr;
DWORD g_mainId = 0;
std::wstring g_dir;

std::wstring EnvDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring(L".")) + L"\\Try2Create\\crash";
}

std::string Stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tmv);
    return buf;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

void InitSymbols() {
    static bool done = false;
    if (done) return;
    done = true;
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitializeW(GetCurrentProcess(), dir.c_str(), TRUE);
}

std::string Describe(DWORD64 addr) {
    char line[900];
    std::string module = "?";
    DWORD64 base = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t path[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH * 2);
        std::wstring p(path, n);
        const size_t slash = p.find_last_of(L"\\/");
        module = Narrow(slash == std::wstring::npos ? p : p.substr(slash + 1));
        base = reinterpret_cast<DWORD64>(mod);
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 500;
    DWORD64 disp = 0;
    std::string name;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) name = sym->Name;
    IMAGEHLP_LINE64 li = {};
    li.SizeOfStruct = sizeof(li);
    DWORD ldisp = 0;
    std::string where;
    if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &ldisp, &li) && li.FileName) {
        std::string f = li.FileName;
        const size_t slash = f.find_last_of("\\/");
        where = " (" + (slash == std::string::npos ? f : f.substr(slash + 1)) + ":" + std::to_string(li.LineNumber) + ")";
    }
    if (name.empty()) std::snprintf(line, sizeof(line), "%s+0x%llx", module.c_str(), static_cast<unsigned long long>(addr - base));
    else std::snprintf(line, sizeof(line), "%s!%s+0x%llx%s", module.c_str(), name.c_str(), static_cast<unsigned long long>(disp), where.c_str());
    return line;
}

std::string StackText(CONTEXT context, HANDLE thread) {
    std::string out;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 60; ++i) {
        char head[32];
        std::snprintf(head, sizeof(head), "  #%02d ", i);
        out += head + Describe(frame.AddrPC.Offset) + "\n";
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (frame.AddrPC.Offset == 0) break;
    }
    return out;
}

std::string Breadcrumbs() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    for (const std::string& n : g_notes) out += "  " + n + "\n";
    return out.empty() ? std::string("  (nothing recorded)\n") : out;
}

void WriteDump(const std::wstring& path, EXCEPTION_POINTERS* ep) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
    }
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithHandleData),
                      ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

// Writes <kind>_<time>.txt and .dmp. `ctx` is where the thread was (the crash, or the frozen main thread).
std::wstring Report(const char* kind, const std::string& headline, CONTEXT* ctx, HANDLE thread, EXCEPTION_POINTERS* ep) {
    InitSymbols();
    CreateDirectoryW((g_dir.substr(0, g_dir.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::string stamp = Stamp();
    const std::wstring base = g_dir + L"\\" + std::wstring(kind, kind + std::strlen(kind)) + L"_" + std::wstring(stamp.begin(), stamp.end());
    std::string text;
    text += std::string("Try2Create ") + kind + " report\n";
    text += "Time: " + stamp + "\n";
    text += headline + "\n\n";
    text += "Stack:\n" + (ctx ? StackText(*ctx, thread) : std::string("  (not available)\n")) + "\n";
    text += "What you did last (oldest first, ms since the program started):\n" + Breadcrumbs() + "\n";
    text += "A .dmp file with the same name sits next to this report: open it in Visual Studio (or WinDbg) with Try2Create.pdb for the full picture.\n";
    FILE* f = nullptr;
    if (_wfopen_s(&f, (base + L".txt").c_str(), L"wb") == 0 && f) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    WriteDump(base + L".dmp", ep);
    return base + L".txt";
}

std::string ExceptionHeadline(const EXCEPTION_RECORD* er) {
    InitSymbols();
    char buf[300];
    if (!er) return "Exception: (unknown)";
    std::snprintf(buf, sizeof(buf), "Exception code 0x%08lX at %s", er->ExceptionCode, Describe(reinterpret_cast<DWORD64>(er->ExceptionAddress)).c_str());
    std::string s = buf;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        std::snprintf(buf, sizeof(buf), "\nAccess violation: %s address 0x%llx", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                      static_cast<unsigned long long>(er->ExceptionInformation[1]));
        s += buf;
    } else if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        s += "\nStack overflow (something called itself without end)";
    }
    return s;
}

void Announce(const std::wstring& report) {
    const std::wstring text = L"Try2Create crashed.\n\nA report was saved here:\n" + report + L"\n\nIt lists where it happened and what you did last. Please send it along.";
    MessageBoxW(nullptr, text.c_str(), L"Try2Create", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (g_reporting.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    HANDLE thread = GetCurrentThread();
    const std::wstring report = Report("crash", ExceptionHeadline(ep ? ep->ExceptionRecord : nullptr), ep ? ep->ContextRecord : nullptr, thread, ep);
    Announce(report);
    return EXCEPTION_EXECUTE_HANDLER;
}

void ReportNow(const char* kind, const std::string& headline) {
    if (g_reporting.exchange(true)) return;
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    const std::wstring report = Report(kind, headline, &ctx, GetCurrentThread(), nullptr);
    Announce(report);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnTerminate() {
    std::string what = "terminate() was called";
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        what = std::string("An uncaught C++ exception: ") + ex.what();
    } catch (...) {
        what = "An uncaught C++ exception of an unknown type";
    }
    ReportNow("crash", what);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) { ReportNow("crash", "An invalid argument was passed to a C runtime function"); }
void OnPureCall() { ReportNow("crash", "A pure virtual function was called"); }
void OnAbort(int) { ReportNow("crash", "abort() was called"); }

// The program has not gone round its main loop for a while: write down where the main thread is stuck.
void Watchdog() {
    unsigned long long reportedFor = 0;
    for (;;) {
        Sleep(1000);
        const unsigned long long beat = g_beat.load();
        if (beat == 0 || g_reporting.load()) continue;
        const unsigned long long now = GetTickCount64();
        if (now - beat < 15000 || reportedFor == beat) continue;
        reportedFor = beat;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        bool ok = false;
        if (SuspendThread(g_mainThread) != static_cast<DWORD>(-1)) {
            ok = GetThreadContext(g_mainThread, &ctx) != 0;
            ResumeThread(g_mainThread);
        }
        char head[200];
        std::snprintf(head, sizeof(head), "The window did not answer for %llu seconds (the main thread is stuck, or busy with something very long).", (now - beat) / 1000);
        InitSymbols();
        CreateDirectoryW(g_dir.substr(0, g_dir.find_last_of(L'\\')).c_str(), nullptr);
        CreateDirectoryW(g_dir.c_str(), nullptr);
        const std::string stamp = Stamp();
        const std::wstring base = g_dir + L"\\hang_" + std::wstring(stamp.begin(), stamp.end());
        std::string text = std::string("Try2Create hang report\nTime: ") + stamp + "\n" + head + "\n\nWhere the main thread is:\n" +
                           (ok ? StackText(ctx, g_mainThread) : std::string("  (could not read it)\n")) + "\nWhat you did last (oldest first, ms since the program started):\n" +
                           Breadcrumbs() + "\nIt may recover by itself; if you had to end the program, this is where it was. A .dmp file sits next to this report.\n";
        FILE* f = nullptr;
        if (_wfopen_s(&f, (base + L".txt").c_str(), L"wb") == 0 && f) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
        WriteDump(base + L".dmp", nullptr);
    }
}

} // namespace

std::wstring CrashLogDir() { return g_dir.empty() ? EnvDir() : g_dir; }

void CrashNote(const std::string& what) {
    static const unsigned long long start = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_notes.push_back(std::to_string(GetTickCount64() - start) + " ms  " + what);
    while (g_notes.size() > 80) g_notes.pop_front();
}

void CrashHeartbeat() {
    g_beat = GetTickCount64();
    // test hooks: set MV_CRASH_TEST=crash (or =hang) to see a report without waiting for a real problem
    static const char* test = std::getenv("MV_CRASH_TEST");
    static int calls = 0;
    if (test && ++calls == 150) {
        if (std::strcmp(test, "crash") == 0) *reinterpret_cast<volatile int*>(0) = 1;
        else if (std::strcmp(test, "hang") == 0) Sleep(20000);
    }
}

void InstallCrashLogger() {
    g_dir = EnvDir();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_mainId = GetCurrentThreadId();
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    std::signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ULONG guarantee = 64 * 1024; // room to write the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    CrashNote("program started");
    std::thread(Watchdog).detach();
}
