// reVC for Windows 10 Mobile: the UWP shell.
//
// A code-only C++/WinRT XAML app (no markup compiler) whose window is one SwapChainPanel with
// the on-screen pad over it. ANGLE presents the game's OpenGL ES frames into that panel; the
// game itself (reVC_Main of src/skel/glfw/glfw.cpp, behind w10m/glfw) runs on a thread of its
// own and talks to this file only through host_api.h. Before there is a game to run, the same
// window shows the screen that asks for the game's files (GameFiles.h).
//
// The structure, and what the phones taught about it, comes from butterscotch-w10m.
#include "pch.h"

#include <fcntl.h>
#include <io.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>

#include <exception>

#include "host_api.h"
#include "Gamepad.h"
#include "GameFiles.h"
#include "TouchPad.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

extern "C" __declspec(dllimport) void angle_uwp_set_panel_scale(float x, float y);
extern "C" __declspec(dllimport) void angle_uwp_set_logger(void (*logger)(const char*));

namespace {

using revc::T;
using revc::Utf8;

// ===[ Log: LocalState\revc.log, readable through the Device Portal while the game runs ]===

std::mutex gLogLock, gLogFileLock;
std::wstring gLogPath;
std::string gLogPending;

// The file is opened, appended to and closed each time: the Device Portal refuses (404) a file
// another process holds open for writing, whatever its sharing flags, so this keeps the log
// readable while the game runs. Not once a line, though: the game names every file it opens.
void LogFlush() {
    std::lock_guard<std::mutex> file(gLogFileLock);
    std::string text;
    {
        std::lock_guard<std::mutex> lock(gLogLock);
        if (gLogPath.empty()) return;
        text.swap(gLogPending);
    }
    if (text.empty()) return;
    CREATEFILE2_EXTENDED_PARAMETERS p{};
    p.dwSize = sizeof(p);
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE h = ::CreateFile2(gLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, &p);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    ::CloseHandle(h);
}

void LogOpen(const std::wstring& folder) {
    std::wstring path = folder + L"\\revc.log";
    std::wstring previous = folder + L"\\revc.prev.log";
    ::MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    {
        std::lock_guard<std::mutex> lock(gLogLock);
        gLogPath = path;
    }
    std::thread([] {
        for (;;) {
            ::Sleep(250);
            LogFlush();
        }
    }).detach();
}

// Every line starts with the seconds since the app's first: what followed what, and how soon.
void LogWrite(const char* text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
    std::lock_guard<std::mutex> lock(gLogLock);
    if (gLogPending.size() > (4u << 20)) return;  // nobody is taking it away: the storage is full
    static LARGE_INTEGER frequency, start;
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        ::QueryPerformanceFrequency(&frequency);
        start = now;
    }
    char stamp[24];
    int stampLength = snprintf(stamp, sizeof(stamp), "%8.3f ",
                               static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart));
    if (stampLength > 0) gLogPending.append(stamp, static_cast<size_t>(stampLength));
    gLogPending.append(text, length);
    gLogPending.append("\r\n", 2);
}

void Log(std::string const& text) { LogWrite(text.c_str()); }

// The game names every file it opens, every sound it measures and every texture it converts:
// thousands of lines a start, and ten thousand the first time. Those stay out of the log
// (REVC_VERBOSE=1 lets them in), but the last of them are kept for the moment something ends
// the app: what the game was doing then is among them.
std::atomic<bool> gVerbose{false};
char gQuiet[16][200];
std::atomic<unsigned> gQuietCount{0};

void QuietNote() {
    unsigned count = gQuietCount.load();
    for (unsigned i = count > 8 ? count - 8 : 0; i < count; i++) {
        char text[240];
        snprintf(text, sizeof(text), "game (before that): %s", gQuiet[i % 16]);
        LogWrite(text);
    }
}

void GameLine(const char* text, size_t length) {
    if (!length) return;
    std::string line(text, length);
    bool chatter = line.compare(0, 7, "[DEV]: ") == 0 || line.find("CFileMgr::") != std::string::npos ||
                   line.find(".txd took ") != std::string::npos || line.find(" was draw last") != std::string::npos ||
                   line.find(" appears more than once in ") != std::string::npos;
    if (chatter && !gVerbose.load()) {
        char* slot = gQuiet[gQuietCount.load() % 16];
        strncpy(slot, line.c_str(), sizeof(gQuiet[0]) - 1);
        slot[sizeof(gQuiet[0]) - 1] = 0;
        gQuietCount.fetch_add(1);
        return;
    }
    LogWrite(("game: " + line).c_str());
}

// The game's own messages do not all end their line: one "[DBG]: " after another
void GameOutput(std::string const& line) {
    size_t start = 0;
    for (;;) {
        size_t next = std::string::npos;
        for (const char* mark : {"[DBG]: ", "[DEV]: "}) {
            size_t at = line.find(mark, start + 1);
            if (at < next) next = at;
        }
        GameLine(line.data() + start, (next == std::string::npos ? line.size() : next) - start);
        if (next == std::string::npos) break;
        start = next;
    }
}

// What the game, librw, OpenAL Soft and mpg123 print (they share one C runtime) goes to the
// log too: stdout and stderr become a pipe that a thread reads by the line. A window app's
// streams have no file behind them, and so nothing to redirect, until a file has been opened as
// each: a placeholder, one for either. (freopen_s opens a file for it alone to write: with one
// placeholder for both, stderr was left closed, and the C runtime ended the app at the first
// line OpenAL Soft wrote to it.)
void CaptureOutput(std::wstring const& folder) {
    int ends[2];
    if (_pipe(ends, 1 << 16, _O_BINARY) != 0) {
        LogWrite("shell: no pipe for what the game prints");
        return;
    }
    FILE* const streams[] = {stdout, stderr};
    const wchar_t* const names[] = {L"\\stdout.tmp", L"\\stderr.tmp"};
    for (int i = 0; i < 2; i++) {
        std::wstring placeholder = folder + names[i];
        FILE* opened = nullptr;
        if (_wfreopen_s(&opened, placeholder.c_str(), L"w", streams[i]) == 0 && _dup2(ends[1], _fileno(streams[i])) == 0)
            setvbuf(streams[i], nullptr, _IONBF, 0);
        else
            LogWrite(i ? "shell: stderr could not be captured" : "shell: stdout could not be captured");
        ::DeleteFileW(placeholder.c_str());  // the pipe has taken its place
    }
    _close(ends[1]);
    int reader = ends[0];
    std::thread([reader] {
        std::string line;
        char buffer[1024];
        for (;;) {
            int n = _read(reader, buffer, sizeof(buffer));
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (buffer[i] == '\n' || line.size() >= 1000) {
                    GameOutput(line);
                    line.clear();
                    if (buffer[i] == '\n') continue;
                }
                if (buffer[i] != '\r') line.push_back(buffer[i]);
            }
        }
    }).detach();
}

// Settings for a test run. The app cannot open a file the Device Portal uploads into LocalState,
// it only sees its name: so a setting is an empty file called "env.NAME=value" (as in renpy-w10m).
// REVC_ANGLE_FL=9_3 and ALSOFT_LOGLEVEL=3 are the ones of use.
void ReadEnvironment(std::wstring const& folder) {
    WIN32_FIND_DATAW item;
    HANDLE find = ::FindFirstFileExW((folder + L"\\env.*").c_str(), FindExInfoBasic, &item, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = item.cFileName + 4;
        size_t equals = name.find(L'=');
        if (equals == std::wstring::npos || equals == 0) continue;
        _wputenv_s(name.substr(0, equals).c_str(), name.substr(equals + 1).c_str());
        LogWrite(("shell: environment " + Utf8(name)).c_str());
    } while (::FindNextFileW(find, &item));
    ::FindClose(find);
}

// ===[ State shared with the game thread ]===

std::mutex gEventLock;
std::deque<HostEvent> gEvents;
std::atomic<int32_t> gPanelW{0}, gPanelH{0};    // the render target
std::atomic<int32_t> gNativeW{0}, gNativeH{0};  // the panel in physical pixels
std::atomic<bool> gFocus{true};
std::atomic<bool> gStarted{false};
std::atomic<int32_t> gContext{HOST_CONTEXT_LOADING};
std::atomic<int32_t> gPadStyle{HOST_PAD_STYLE_PLAYSTATION};
std::atomic<int32_t> gRequests{0};
void* gPanelAbi = nullptr;  // ABI IInspectable* of the SwapChainPanel
std::wstring gLocalFolder;  // LocalState
std::atomic<DWORD> gUiThread{0}, gGameThread{0};
revc::TouchState gTouch;
revc::Gamepad* gGamepad = nullptr;
std::function<void(std::string)> gFatal;  // set by the App: shows it and ends the app

void Push(int32_t type, int32_t code, float x = 0, float y = 0) {
    std::lock_guard<std::mutex> lock(gEventLock);
    gEvents.push_back(HostEvent{type, code, x, y});
}

// ===[ Faults ]===
// The phone writes no crash dumps for a sideloaded app: what ends the app goes to the log, with
// module and offset to look up in the linker map (builds\maps). Nothing here allocates: the
// thread that faulted may be the one holding the heap.

const char* ThreadName(DWORD id) {
    return id == gUiThread.load() ? "the UI thread" : id == gGameThread.load() ? "the game's thread" : "another thread";
}

// The module an address is in and its offset there; false (and "?") where there is none
bool ModuleOf(uintptr_t address, char* name, size_t size, uintptr_t* offset) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "";
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) ||
        !module || !::GetModuleFileNameA(module, path, MAX_PATH)) {
        snprintf(name, size, "?");
        *offset = address;
        return false;
    }
    const char* base = strrchr(path, '\\');
    snprintf(name, size, "%s", base ? base + 1 : path);
    *offset = address - reinterpret_cast<uintptr_t>(module);
    return true;
}

bool Readable(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION region{};
    if (address < 0x10000 || !::VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return address + size <= reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
}

// Return addresses on a stack: every word of it that points into a module's code (odd, for
// Thumb). A backtrace without unwind data: what was called on the way here, and some of what
// was called before and has returned.
void StackNote(uintptr_t sp) {
    MEMORY_BASIC_INFORMATION region{};
    if (!sp || !::VirtualQuery(reinterpret_cast<void*>(sp), &region, sizeof(region)) || region.State != MEM_COMMIT) return;
    uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    uintptr_t codeFrom = 0, codeTo = 0;  // the last region found to be code
    char line[1024];
    size_t used = 0;
    int found = 0;
    for (uintptr_t at = (sp + 3) & ~uintptr_t(3); at + sizeof(uintptr_t) <= end && at < sp + 24576 && found < 60;
         at += sizeof(uintptr_t)) {
        uintptr_t value = *reinterpret_cast<const uintptr_t*>(at);
        if (!(value & 1) || value < 0x10000) continue;
        if (value < codeFrom || value >= codeTo) {
            MEMORY_BASIC_INFORMATION code{};
            if (!::VirtualQuery(reinterpret_cast<void*>(value), &code, sizeof(code)) || code.State != MEM_COMMIT ||
                code.Type != MEM_IMAGE ||
                !(code.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                continue;
            codeFrom = reinterpret_cast<uintptr_t>(code.BaseAddress);
            codeTo = codeFrom + code.RegionSize;
        }
        char module[MAX_PATH];
        uintptr_t offset = 0;
        if (!ModuleOf(value, module, sizeof(module), &offset)) continue;
        int n = snprintf(line + used, sizeof(line) - used, " %s+0x%lX", module, static_cast<unsigned long>(offset));
        if (n > 0) used += static_cast<size_t>(n);
        if (++found % 6 == 0 || used > sizeof(line) - MAX_PATH - 32) {
            host_logf("crash: stack%s", line);
            used = 0;
        }
    }
    if (used) host_logf("crash: stack%s", line);
}

void FaultNote(EXCEPTION_POINTERS* info, bool full) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    char module[MAX_PATH];
    uintptr_t offset = 0;
    ModuleOf(reinterpret_cast<uintptr_t>(record->ExceptionAddress), module, sizeof(module), &offset);
    char access[64] = "";
    if (record->NumberParameters >= 2 &&
        (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR))
        snprintf(access, sizeof(access), ", %s address %p",
                 record->ExceptionInformation[0] == 1 ? "writing" : record->ExceptionInformation[0] == 8 ? "executing" : "reading",
                 reinterpret_cast<void*>(record->ExceptionInformation[1]));
    DWORD thread = ::GetCurrentThreadId();
    host_logf("crash: exception 0x%08lX at %s+0x%lX (thread %lu, %s)%s", record->ExceptionCode, module,
              static_cast<unsigned long>(offset), thread, ThreadName(thread), access);
#ifdef _M_ARM
    if (full) {
        const CONTEXT* c = info->ContextRecord;
        host_logf("crash: r0 %08lX r1 %08lX r2 %08lX r3 %08lX r4 %08lX r5 %08lX r6 %08lX r7 %08lX", c->R0, c->R1, c->R2,
                  c->R3, c->R4, c->R5, c->R6, c->R7);
        ModuleOf(c->Lr, module, sizeof(module), &offset);
        host_logf("crash: r8 %08lX r9 %08lX r10 %08lX r11 %08lX r12 %08lX sp %08lX pc %08lX lr %08lX (%s+0x%lX)", c->R8,
                  c->R9, c->R10, c->R11, c->R12, c->Sp, c->Pc, c->Lr, module, static_cast<unsigned long>(offset));
        StackNote(c->Sp);
        QuietNote();
    }
#else
    (void)full;
#endif
}

// A C++ exception, by the name of its type: thrown and caught all the time (C++/WinRT reports
// errors with them), but one nobody catches ends the app without another word. On ARM the
// exception's tables hold offsets from the image's base, which is its fourth parameter.
void CppExceptionNote(const EXCEPTION_RECORD* record) {
    static std::atomic<int> noted{0};
    if (record->NumberParameters < 3 || noted.fetch_add(1) >= 24) return;
    uintptr_t base = record->NumberParameters >= 4 ? record->ExceptionInformation[3] : 0;
    uintptr_t throwInfo = record->ExceptionInformation[2];
    const char* name = "?";
    if (Readable(throwInfo, 16)) {
        uintptr_t types = base + reinterpret_cast<const uint32_t*>(throwInfo)[3];  // the catchable types
        if (Readable(types, 8) && reinterpret_cast<const int32_t*>(types)[0] > 0) {
            uintptr_t type = base + reinterpret_cast<const uint32_t*>(types)[1];  // the first: the thrown type
            if (Readable(type, 8)) {
                uintptr_t descriptor = base + reinterpret_cast<const uint32_t*>(type)[1];
                if (Readable(descriptor, 2 * sizeof(void*) + 64)) name = reinterpret_cast<const char*>(descriptor + 2 * sizeof(void*));
            }
        }
    }
    DWORD thread = ::GetCurrentThreadId();
    host_logf("note: a C++ exception of type %.80s is thrown (thread %lu, %s)", name, thread, ThreadName(thread));
}

LONG CALLBACK CrashNote(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363) {
        CppExceptionNote(info->ExceptionRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case STATUS_STACK_BUFFER_OVERRUN:
        case STATUS_HEAP_CORRUPTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_BREAKPOINT:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
    // First chance: something may still handle it. In full a few times, then a line each.
    static std::atomic<int> noted{0};
    int count = noted.fetch_add(1);
    if (count < 40) {
        FaultNote(info, count < 4);
        LogFlush();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI LastChance(EXCEPTION_POINTERS* info) {
    host_logf("crash: nothing handled exception 0x%08lX: the app ends here", info->ExceptionRecord->ExceptionCode);
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// What ends a process without an exception a handler would see: the C runtime's own ends (a
// fail-fast does not pass through vectored handlers). On a phone such an end shows up as
// something else: a moment later the UI thread faults in CoreUIComponents.dll, a store through
// a null r8 with nothing of the app on its stack. That is the phone's navigation client taking
// the window from a process that is already being ended (husk-w10m found the same behind a
// stack overrun), and it was this port's first crash: the cause was an invalid parameter two
// hundred milliseconds earlier, on the game's thread. So these are noted where they happen.
void HereNote(const char* what) {
    DWORD thread = ::GetCurrentThreadId();
    host_logf("crash: %s (thread %lu, %s)", what, thread, ThreadName(thread));
#ifdef _M_ARM
    CONTEXT context{};
    ::RtlCaptureContext(&context);
    StackNote(context.Sp);
#endif
    QuietNote();
    LogFlush();
}

// The C runtime ends the app when a function of it is handed something invalid (a closed
// stream, a bad format). With a handler that returns, the function fails instead and the game
// goes on.
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    static std::atomic<int> noted{0};
    if (noted.fetch_add(1) < 6) HereNote("the C runtime was handed an invalid parameter; the call fails and the game goes on");
}

void PureCall() {
    HereNote("a pure virtual function was called");
    ::TerminateProcess(::GetCurrentProcess(), 3);
}

void Aborted(int) { HereNote("abort() was called"); }

// set_terminate is a thread's own: the UI thread's and the game's call this
void WatchThisThread() {
    std::set_terminate([] {
        HereNote("std::terminate: an exception nobody caught");
        ::TerminateProcess(::GetCurrentProcess(), 3);
    });
}

}  // namespace

// ===[ host_api.h ]===

extern "C" void* host_panel(void) { return gPanelAbi; }

extern "C" bool host_panel_size(int32_t* w, int32_t* h) {
    *w = gPanelW.load();
    *h = gPanelH.load();
    return *w > 0 && *h > 0;
}

extern "C" bool host_panel_native_size(int32_t* w, int32_t* h) {
    *w = gNativeW.load();
    *h = gNativeH.load();
    return *w > 0 && *h > 0;
}

extern "C" bool host_poll_event(HostEvent* e) {
    std::lock_guard<std::mutex> lock(gEventLock);
    if (gEvents.empty()) return false;
    *e = gEvents.front();
    gEvents.pop_front();
    return true;
}

extern "C" bool host_has_focus(void) { return gFocus.load(); }

extern "C" void host_pad_state(HostPad* pad) {
    if (gGamepad && gGamepad->Read(pad)) return;
    std::lock_guard<std::mutex> lock(gTouch.lock);
    *pad = gTouch.pad;
}

extern "C" void host_set_context(int32_t context) { gContext.store(context); }

extern "C" void host_set_pad_style(int32_t style) { gPadStyle.store(style); }

extern "C" int32_t host_take_requests(void) { return gRequests.exchange(0); }

extern "C" const wchar_t* host_local_folder(void) { return gLocalFolder.empty() ? nullptr : gLocalFolder.c_str(); }

extern "C" void host_log(const char* text) { LogWrite(text); }

extern "C" void host_logf(const char* format, ...) {
    char text[1024];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    LogWrite(text);
}

extern "C" void host_fatal(const char* text) {
    LogWrite((std::string("fatal: ") + text).c_str());
    LogFlush();
    if (gFatal) gFatal(text);
    else ::TerminateProcess(::GetCurrentProcess(), 3);
    for (;;) ::Sleep(1000);  // the game thread goes no further; the UI thread ends the app
}

namespace revc {

// A code-only XAML app must still implement IXamlMetadataProvider: the framework asks the
// Application for it during start-up. No custom XAML types exist, so everything is empty.
struct App : ApplicationT<App, winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> {
    SwapChainPanel panel_{nullptr};
    std::unique_ptr<TouchPad> touch_;
    std::unique_ptr<GameFiles> files_;
    std::unique_ptr<Gamepad> gamepad_;
    // Window content: a Grid, which XAML sizes to the window in every orientation, holding the
    // files screen (stretched by it) over a Canvas (it does not clip a child larger than itself)
    // with the stage: the game panel plus the pad, turned by hand when the OS keeps the window
    // portrait
    Grid top_{nullptr};
    Canvas root_{nullptr};
    Grid stage_{nullptr};
    winrt::Windows::Devices::Sensors::SimpleOrientationSensor sensor_{nullptr};
    winrt::Windows::System::Display::DisplayRequest awake_{nullptr};
    DispatcherTimer tick_{nullptr};
    double angle_ = 90;  // clockwise turn of the stage in a portrait window
    bool rotated_ = false;
    double stageW_ = 0, stageH_ = 0;  // DIPs, set by Layout
    float scaleX_ = 1, scaleY_ = 1;    // physical pixels per DIP of the panel
    double renderScale_ = 1;           // render-target pixels per physical pixel, fixed at the start
    int32_t resolution_ = 0;           // the setting, per cent of the panel; 0: AutoLines() at most
    bool level9_ = false;              // a GPU that stops at Direct3D level 9
    std::wstring local_, starting_;
    bool gameReady_ = false;
    bool panelLoaded_ = false;
    bool reachedMenu_ = false;
    uint32_t memoryTicks_ = 0;
    uint64_t memoryNoted_ = 0;
    bool snapshots_ = false;
    int32_t preview_ = -1;
    DispatcherTimer snapshotTimer_{nullptr};

    App() {
        gUiThread = ::GetCurrentThreadId();
        std::wstring version;
        try {
            local_ = std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
            gLocalFolder = local_;
            LogOpen(local_);
            std::string family = "?", os = "?";
            try {
                auto info = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
                family = Utf8(std::wstring(info.DeviceFamily()));
                uint64_t ver = std::stoull(std::wstring(info.DeviceFamilyVersion()));
                os = std::to_string(ver >> 48) + "." + std::to_string((ver >> 32) & 0xFFFF) + "." +
                     std::to_string((ver >> 16) & 0xFFFF) + "." + std::to_string(ver & 0xFFFF);
            } catch (...) {
            }
            auto v = Package::Current().Id().Version();
            version = std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." + std::to_wstring(v.Build) +
                      L"." + std::to_wstring(v.Revision);
            Log("shell: reVC for Windows 10 Mobile " + Utf8(version) + " on " + family + " " + os);
            CaptureOutput(local_);
            _putenv_s("ALSOFT_LOGLEVEL", "2");  // OpenAL Soft's warnings and errors, into the log
            ReadEnvironment(local_);
            gVerbose = _wgetenv(L"REVC_VERBOSE") != nullptr;
            // For looking at the shell from the PC, which has no other view of the phone's screen:
            // REVC_SNAPSHOT=1 saves the XAML over the panel (the files screen, the pad; not the
            // game's picture) as LocalState\snapshot.png whenever it changes, and
            // REVC_PAD_PREVIEW=menu|foot|vehicle|cutscene shows that pad without a game, with
            // Xbox's names on it when the word ends in -xbox (vehicle-xbox).
            snapshots_ = _wgetenv(L"REVC_SNAPSHOT") != nullptr;
            if (const wchar_t* preview = _wgetenv(L"REVC_PAD_PREVIEW")) {
                std::wstring name(preview);
                size_t dash = name.find(L'-');
                if (dash != std::wstring::npos) {
                    if (name.substr(dash + 1) == L"xbox") gPadStyle = HOST_PAD_STYLE_XBOX;
                    name.resize(dash);
                }
                preview_ = name == L"menu"       ? HOST_CONTEXT_MENU
                           : name == L"foot"     ? HOST_CONTEXT_ON_FOOT
                           : name == L"vehicle"  ? HOST_CONTEXT_VEHICLE
                           : name == L"cutscene" ? HOST_CONTEXT_CUTSCENE
                                                 : -1;
            }
            starting_ = local_ + L"\\starting";
            auto settings = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
            if (auto value = settings.TryLookup(L"Resolution")) resolution_ = unbox_value_or<int32_t>(value, 0);
            level9_ = Level9Gpu();
            // for this run only, and not kept: REVC_RESOLUTION=50 (0: as the app chooses itself)
            if (const wchar_t* forced = _wgetenv(L"REVC_RESOLUTION")) resolution_ = _wtoi(forced);
        } catch (...) {
        }
        version_ = version;
        RequestedTheme(ApplicationTheme::Dark);  // the files screen's buttons and dialogs
        // ANGLE's own notes, trouble only (it reports every texture and program otherwise)
        angle_uwp_set_logger([](const char* text) {
            if (!text) return;
            static const char* const keep[] = {"fail", "FAIL", "error", "lost", "reset", "resized", "swap chain",
                                               "removed", "hung", "logging is on", "composition scale"};
            if (strstr(text, "executable")) return;  // "executable ... reset": one for every shader program
            for (const char* k : keep) {
                if (strstr(text, k)) {
                    LogWrite(text);  // its notes say "angle:" themselves
                    return;
                }
            }
        });
        UnhandledException([](auto const&, UnhandledExceptionEventArgs const& e) {
            Log("shell: unhandled exception: " + Utf8(std::wstring(e.Message())));
        });
        Suspending([](auto const&, auto const&) {
            LogWrite("shell: suspending");
            LogFlush();  // a suspended app may be ended without another word
        });
        Resuming([](auto const&, auto const&) { LogWrite("shell: resuming"); });
    }

    void OnLaunched(LaunchActivatedEventArgs const&) {
        if (!Window::Current().Content()) {
            top_ = Grid();
            top_.Background(Brush(0xFF000000));
            root_ = Canvas();
            top_.Children().Append(root_);
            stage_ = Grid();
            stage_.RenderTransformOrigin({0.5f, 0.5f});
            root_.Children().Append(stage_);
            panel_ = SwapChainPanel();
            stage_.Children().Append(panel_);
            Window::Current().Content(top_);
            gPanelAbi = winrt::get_abi(panel_);

            touch_ = std::make_unique<TouchPad>(
                stage_, gTouch,
                // a touch on a menu, in the render target's pixels
                [this](int32_t type, float x, float y) {
                    Push(type, 0, static_cast<float>(x * scaleX_ * renderScale_),
                         static_cast<float>(y * scaleY_ * renderScale_));
                },
                [](int32_t request) { gRequests.fetch_or(request); }, [this] { NextResolution(); });
            touch_->ResolutionLabel(ResolutionLabel());

            gamepad_ = std::make_unique<Gamepad>([this](bool connected) {
                Log(connected ? "shell: a gamepad is connected" : "shell: the gamepad is gone");
            });
            gGamepad = gamepad_.get();

            gFatal = [this](std::string text) {
                top_.Dispatcher().RunAsync(CoreDispatcherPriority::High, [this, text] { Fatal(text); });
            };

            std::wstring package = std::wstring(Package::Current().InstalledLocation().Path());
            files_ = std::make_unique<GameFiles>(top_, local_, package, version_, [this] { Play(); }, Log);

            HookPanel();
            HookWindow();
            HookOrientation();

            if (preview_ >= 0) {
                // the pad alone, as it is in that part of the game (REVC_PAD_PREVIEW)
                files_->Close();
                touch_->Context(preview_);
                touch_->Style(gPadStyle.load());
                Presentation();
                // The marker of a start that never got to the game's menu: ask before trying again
            } else if (files_->Present() && !FileExists(starting_) && files_->Overlay()) {
                Play();
            } else {
                files_->Open(FileExists(starting_) && files_->Present()
                                 ? T(L"The game did not get as far as its menu last time. Try again, or replace the "
                                     L"game's files if they are incomplete or modified. revc.log in the app's "
                                     L"folder says what happened.",
                                     L"В прошлый раз игра не дошла до своего меню. Попробуйте ещё раз или замените "
                                     L"файлы игры, если они неполные или изменены. Что произошло, записано в "
                                     L"revc.log в папке приложения.")
                                 : L"");
                Presentation();
            }

            tick_ = DispatcherTimer();
            tick_.Interval(std::chrono::milliseconds(100));
            tick_.Tick([this](auto const&, auto const&) { Tick(); });
            tick_.Start();
        }
        Window::Current().Activate();
    }

    void Play() {
        files_->Close();
        ::SetCurrentDirectoryW(files_->Folder().c_str());
        Log("shell: playing from " + Utf8(files_->Folder()));
        // gone again once the game's menu is up (Tick)
        HANDLE marker = ::CreateFile2(starting_.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (marker != INVALID_HANDLE_VALUE) ::CloseHandle(marker);
        gameReady_ = true;
        try {
            awake_ = winrt::Windows::System::Display::DisplayRequest();
            awake_.RequestActive();  // a gamepad does not touch the screen: it would dim and lock
        } catch (...) {
        }
        Presentation();  // landscape, full screen: the game starts on the layout that follows
        Layout();
    }

    // Once there is a game and the panel is in the window with a size: ANGLE gives it a swap
    // chain of that size
    void StartGame() {
        if (!gameReady_ || !panelLoaded_ || gPanelW.load() <= 0 || gPanelH.load() <= 0 || gStarted.exchange(true)) return;
        // Its own stack size: the game's main thread on a PC has the executable's megabyte, and
        // the Android port's has eight; four here, reserved, not committed.
        uintptr_t thread = _beginthreadex(
            nullptr, 4u << 20,
            [](void*) -> unsigned {
                static char name[] = "reVC.exe";
                char* argv[] = {name, nullptr};
                gGameThread = ::GetCurrentThreadId();
                WatchThisThread();
                LogWrite("shell: starting the game");
                int result = reVC_Main(1, argv);
                Log("shell: the game returned " + std::to_string(result) + ", closing");
                LogFlush();
                winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
                return 0;
            },
            nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread) ::CloseHandle(reinterpret_cast<HANDLE>(thread));
        else LogWrite("shell: the game's thread could not be started");
    }

    // Ten times a second: what the game is doing decides which controls are up
    void Tick() {
        if (!gameReady_ || !touch_) return;
        int32_t context = gStarted.load() ? gContext.load() : HOST_CONTEXT_LOADING;
        if (context == HOST_CONTEXT_MENU && !reachedMenu_) {
            reachedMenu_ = true;
            ::DeleteFileW(starting_.c_str());
            LogWrite("shell: the game's menu is up");
        }
        touch_->Enabled(!gamepad_ || !gamepad_->Connected());
        if (context != touch_->Context()) SnapshotSoon();
        touch_->Context(context);
        touch_->Style(gPadStyle.load());
        MemoryNote();
    }

    // What the app holds of what the phone lets it hold, whenever that has moved by 32 MB: a
    // phone with 1 GB ends an app that goes over its limit, without a word in the log
    void MemoryNote() {
        if (++memoryTicks_ % 20 != 0) return;  // every two seconds
        try {
            using winrt::Windows::System::MemoryManager;
            uint64_t used = MemoryManager::AppMemoryUsage() >> 20, limit = MemoryManager::AppMemoryUsageLimit() >> 20;
            if (used + 32 > memoryNoted_ && memoryNoted_ + 32 > used) return;
            memoryNoted_ = used;
            Log("shell: memory " + std::to_string(used) + " MB, the limit is " + std::to_string(limit) + " MB");
        } catch (...) {
        }
    }

    // ===[ Snapshots (REVC_SNAPSHOT) ]===
    // RenderTargetBitmap draws XAML, and a SwapChainPanel's swap chain is not XAML: the game's
    // picture is black in these. They are for the shell's own layout.

    void SnapshotSoon() {
        if (!snapshots_) return;
        if (!snapshotTimer_) {
            snapshotTimer_ = DispatcherTimer();
            snapshotTimer_.Interval(std::chrono::milliseconds(1500));
            snapshotTimer_.Tick([this](auto const&, auto const&) {
                snapshotTimer_.Stop();
                Snapshot();
            });
        }
        snapshotTimer_.Stop();
        snapshotTimer_.Start();
    }

    winrt::fire_and_forget Snapshot() {
        auto lifetime = get_strong();
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Storage;
        try {
            winrt::Windows::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
            co_await bitmap.RenderAsync(top_);
            auto pixels = co_await bitmap.GetPixelsAsync();
            auto file = co_await ApplicationData::Current().LocalFolder().CreateFileAsync(
                L"snapshot.png", CreationCollisionOption::ReplaceExisting);
            auto stream = co_await file.OpenAsync(FileAccessMode::ReadWrite);
            auto encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream);
            encoder.SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, bitmap.PixelWidth(),
                                 bitmap.PixelHeight(), 96, 96,
                                 winrt::array_view<uint8_t const>(pixels.data(), pixels.data() + pixels.Length()));
            co_await encoder.FlushAsync();
            stream.Close();
            Log("shell: snapshot.png " + std::to_string(bitmap.PixelWidth()) + "x" + std::to_string(bitmap.PixelHeight()));
        } catch (winrt::hresult_error const& e) {
            Log("shell: snapshot failed " + std::to_string(e.code().value));
        }
    }

    void Fatal(std::string const& text) {
        using namespace winrt::Windows::UI::Popups;
        try {
            MessageDialog dialog(winrt::hstring(Wide(text.c_str())), L"reVC");
            dialog.ShowAsync().Completed([](auto const&, auto const&) { ::TerminateProcess(::GetCurrentProcess(), 3); });
        } catch (...) {
            ::TerminateProcess(::GetCurrentProcess(), 3);
        }
    }

    // ===[ Resolution ]===
    // The game draws into a swap chain of the panel's size times this, and the compositor
    // stretches it over the panel: a 1440x2560 screen is more than these GPUs fill at speed.

    static constexpr int32_t kResolutions[] = {0, 100, 75, 60, 50};

    // Left to itself the game is drawn at 720 lines at most, and at 432 on a GPU that stops
    // at Direct3D level 9: a Lumia 650 with the game's drawing cut down to what its processor
    // gets through is held back by the pixels next, 12 frames a second at 720 lines where
    // 432 give 17 (and 360 no more than that).
    int32_t AutoLines() const { return level9_ ? 432 : 720; }

    double RenderScale(double nativeW, double nativeH) const {
        if (resolution_ > 0) return resolution_ / 100.0;
        double lines = (std::min)(nativeW, nativeH);
        return lines > AutoLines() ? AutoLines() / lines : 1.0;
    }

    std::wstring ResolutionLabel() const {
        return resolution_ > 0 ? L"RES " + std::to_wstring(resolution_) + L"%" : L"RES " + std::to_wstring(AutoLines()) + L"p";
    }

    // ANGLE gives OpenGL ES 3.0 from Direct3D level 10_1 up and 2.0 below; asked of Direct3D
    // itself, because the shell sizes the game's picture before the game has a context.
    static bool Level9Gpu() {
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                   D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                                   D3D_FEATURE_LEVEL_9_1};
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        winrt::com_ptr<ID3D11Device> device;
        HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                                         D3D11_SDK_VERSION, device.put(), &got, nullptr);
        if (hr == E_INVALIDARG)  // a runtime that does not know 11_1
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, ARRAYSIZE(levels) - 1,
                                     D3D11_SDK_VERSION, device.put(), &got, nullptr);
        if (FAILED(hr)) {
            Log("shell: Direct3D would not say what level the GPU has");
            return false;
        }
        char text[80];
        snprintf(text, sizeof(text), "shell: Direct3D feature level %d_%d", (got >> 12) & 15, (got >> 8) & 15);
        LogWrite(text);
        return got < D3D_FEATURE_LEVEL_10_1;
    }

    void NextResolution() {
        size_t i = 0;
        while (i < std::size(kResolutions) && kResolutions[i] != resolution_) i++;
        resolution_ = kResolutions[(i + 1) % std::size(kResolutions)];
        try {
            winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values().Insert(
                L"Resolution", winrt::box_value(resolution_));
        } catch (...) {
        }
        touch_->ResolutionLabel(ResolutionLabel());
        Log("shell: resolution setting " + std::to_string(resolution_));
        // ANGLE scales a swap chain when it makes it: the one that is up keeps its size
        try {
            winrt::Windows::UI::Popups::MessageDialog dialog(winrt::hstring(
                std::wstring(resolution_ > 0
                                 ? T(L"The game will be drawn at ", L"Игра будет рисоваться в ") + std::to_wstring(resolution_) +
                                       T(L"% of the screen's resolution", L"% разрешения экрана")
                                 : T(L"The game will be drawn at ", L"Игра будет рисоваться не более чем в ") +
                                       std::to_wstring(AutoLines()) + T(L" lines at most", L" строк")) +
                T(L" the next time the app starts. Fewer pixels are faster.",
                  L" при следующем запуске приложения. Меньше пикселей — быстрее.")));
            dialog.ShowAsync();
        } catch (...) {
        }
    }

    // ===[ Window ]===

    // The game: landscape, full screen (status and navigation bars hidden until swiped in). The
    // files screen: any orientation, with the system bars shown.
    void Presentation() {
        using namespace winrt::Windows::Graphics::Display;
        using namespace winrt::Windows::UI::ViewManagement;
        bool game = gameReady_ || preview_ >= 0;
        try {
            DisplayInformation::AutoRotationPreferences(
                game ? DisplayOrientations::Landscape | DisplayOrientations::LandscapeFlipped : DisplayOrientations::None);
        } catch (...) {
        }
        try {
            auto view = ApplicationView::GetForCurrentView();
            if (game) view.TryEnterFullScreenMode();
            else if (view.IsFullScreenMode()) view.ExitFullScreenMode();
        } catch (...) {
        }
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
                auto bar = StatusBar::GetForCurrentView();
                if (game) {
                    bar.HideAsync();
                } else {
                    bar.BackgroundColor(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x14, 0x09, 0x1C));
                    bar.BackgroundOpacity(1);
                    bar.ForegroundColor(winrt::Windows::UI::Colors::White());
                    bar.ShowAsync();
                }
            }
        } catch (...) {
        }
    }

    // Where the status bar and the navigation bar cover the window, in DIPs (zero when the window
    // is already laid out inside them)
    winrt::Windows::UI::Xaml::Thickness Insets() {
        try {
            auto b = Window::Current().Bounds();
            auto v = winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().VisibleBounds();
            auto clamp = [](double d) { return d > 0 ? d : 0.0; };
            return {clamp(v.X - b.X), clamp(v.Y - b.Y), clamp((b.X + b.Width) - (v.X + v.Width)),
                    clamp((b.Y + b.Height) - (v.Y + v.Height))};
        } catch (...) {
            return {0, 0, 0, 0};
        }
    }

    // AutoRotationPreferences and the manifest's rotation list are requests the phone may ignore
    // (rotation lock, or launched while held upright): the window then stays portrait. The stage
    // is laid out landscape and turned 90 degrees, towards the way the phone is actually held.
    void HookOrientation() {
        // A turn of the phone reaches the window's bounds and VisibleBounds before XAML has laid
        // the root out again: lay out on each, and once more when the dispatcher is idle, by
        // which time XAML has finished its pass
        root_.SizeChanged([this](auto const&, auto const&) { Layout(); });
        Window::Current().SizeChanged([this](auto const&, auto const&) {
            Layout();
            LayoutWhenIdle();
        });
        try {
            winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().VisibleBoundsChanged(
                [this](auto const&, auto const&) {
                    Layout();
                    LayoutWhenIdle();
                });
        } catch (...) {
        }
        try {
            using namespace winrt::Windows::Devices::Sensors;
            sensor_ = SimpleOrientationSensor::GetDefault();
            if (!sensor_) return;
            Steer(sensor_.GetCurrentOrientation());
            sensor_.OrientationChanged([this](auto const&, SimpleOrientationSensorOrientationChangedEventArgs const& a) {
                auto orientation = a.Orientation();
                root_.Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [this, orientation] {
                    if (Steer(orientation)) Layout();
                });
            });
        } catch (...) {
        }
    }

    // Upright, flat or face down keep the last landscape side
    bool Steer(winrt::Windows::Devices::Sensors::SimpleOrientation o) {
        using winrt::Windows::Devices::Sensors::SimpleOrientation;
        double angle = o == SimpleOrientation::Rotated90DegreesCounterclockwise    ? 90
                       : o == SimpleOrientation::Rotated270DegreesCounterclockwise ? -90
                                                                                   : angle_;
        bool changed = angle != angle_;
        angle_ = angle;
        return changed;
    }

    void LayoutWhenIdle() {
        root_.Dispatcher().RunAsync(CoreDispatcherPriority::Low, [this] { Layout(); });
    }

    void Layout() {
        // The window's bounds, not the root's ActualWidth/Height, which can still hold the old
        // orientation when a turn is reported (every child here is sized explicitly)
        auto bounds = Window::Current().Bounds();
        double w = bounds.Width, h = bounds.Height;
        if (w <= 0 || h <= 0) {
            w = root_.ActualWidth();
            h = root_.ActualHeight();
        }
        if (w <= 0 || h <= 0) return;
        bool portrait = h > w;
        if (portrait) {
            stage_.Width(h);
            stage_.Height(w);
            Canvas::SetLeft(stage_, (w - h) / 2);
            Canvas::SetTop(stage_, (h - w) / 2);
            winrt::Windows::UI::Xaml::Media::RotateTransform turn;
            turn.Angle(angle_);
            stage_.RenderTransform(turn);
        } else {
            stage_.Width(w);
            stage_.Height(h);
            Canvas::SetLeft(stage_, 0);
            Canvas::SetTop(stage_, 0);
            stage_.RenderTransform(nullptr);
        }
        bool changed = stageW_ != (portrait ? h : w) || stageH_ != (portrait ? w : h) || rotated_ != portrait;
        stageW_ = portrait ? h : w;
        stageH_ = portrait ? w : h;
        rotated_ = portrait;
        if (changed)
            Log("shell: window " + std::to_string(static_cast<int>(w)) + "x" + std::to_string(static_cast<int>(h)) +
                " DIP, " + (portrait ? "portrait, stage turned " + std::to_string(static_cast<int>(angle_)) : "landscape"));
        if (files_ && files_->Visible()) files_->Place(w, Insets());
        if (touch_) touch_->Layout(stageW_, stageH_);
        UpdatePanelSize();  // the scale changes with the turn even when the size does not
        SnapshotSoon();
    }

    void UpdatePanelSize() {
        // The display's own ratio of pixels to DIPs, not the panel's composition scale: that one
        // is still 1 until the panel has been loaded (the game may be started before), and a
        // quarter turn of the stage may come back folded into it. Nothing here scales the panel.
        float sx = panel_.CompositionScaleX(), sy = panel_.CompositionScaleY();
        try {
            float raw = static_cast<float>(
                winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
            if (raw > 0) sx = sy = raw;
        } catch (...) {
        }
        // from the size Layout gave the stage: the panel's own lags a layout pass behind it
        if (stageW_ <= 0 || stageH_ <= 0 || sx <= 0 || sy <= 0) return;
        double nativeW = stageW_ * sx, nativeH = stageH_ * sy;
        // the render scale is settled once, before the game makes its swap chain
        if (!gStarted.load()) renderScale_ = RenderScale(nativeW, nativeH);
        int32_t w = static_cast<int32_t>(nativeW * renderScale_ + 0.5);
        int32_t h = static_cast<int32_t>(nativeH * renderScale_ + 0.5);
        bool changed = w != gPanelW.load() || h != gPanelH.load() || sx != scaleX_ || sy != scaleY_;
        // ANGLE divides the swap chain's size by this to fill the panel: render pixels per DIP
        angle_uwp_set_panel_scale(static_cast<float>(sx * renderScale_), static_cast<float>(sy * renderScale_));
        scaleX_ = sx;
        scaleY_ = sy;
        gNativeW = static_cast<int32_t>(nativeW + 0.5);
        gNativeH = static_cast<int32_t>(nativeH + 0.5);
        gPanelW = w;
        gPanelH = h;
        if (changed)
            Log("shell: panel " + std::to_string(gNativeW.load()) + "x" + std::to_string(gNativeH.load()) +
                " px, scale " + std::to_string(sx) + ", render target " + std::to_string(w) + "x" + std::to_string(h));
        StartGame();
    }

    void HookPanel() {
        using winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs;
        panel_.Loaded([this](auto const&, auto const&) {
            panelLoaded_ = true;
            Layout();
        });
        panel_.SizeChanged([this](auto const&, auto const&) { UpdatePanelSize(); });
        panel_.CompositionScaleChanged([this](auto const&, auto const&) { UpdatePanelSize(); });
        panel_.PointerPressed([this](auto const&, PointerRoutedEventArgs const& a) {
            touch_->PanelPressed(static_cast<int32_t>(a.Pointer().PointerId()), a.GetCurrentPoint(panel_).Position());
            panel_.CapturePointer(a.Pointer());
            a.Handled(true);
        });
        panel_.PointerMoved([this](auto const&, PointerRoutedEventArgs const& a) {
            touch_->PanelMoved(static_cast<int32_t>(a.Pointer().PointerId()), a.GetCurrentPoint(panel_).Position());
        });
        auto released = [this](auto const&, PointerRoutedEventArgs const& a) {
            touch_->PanelReleased(static_cast<int32_t>(a.Pointer().PointerId()), a.GetCurrentPoint(panel_).Position());
            a.Handled(true);
        };
        panel_.PointerReleased(released);
        panel_.PointerCanceled(released);
        panel_.PointerCaptureLost(released);
        panel_.PointerWheelChanged([this](auto const&, PointerRoutedEventArgs const& a) {
            int delta = a.GetCurrentPoint(panel_).Properties().MouseWheelDelta();
            if (delta) Push(HOST_EV_WHEEL, 0, 0, static_cast<float>(delta) / 120.0f);
        });
    }

    void HookWindow() {
        auto window = CoreWindow::GetForCurrentThread();
        // A keyboard (Bluetooth, USB, Continuum): the game's own keyboard controls
        window.KeyDown([this](auto const&, KeyEventArgs const& a) {
            if (!gameReady_) return;  // the files screen takes keys itself
            if (!a.KeyStatus().WasKeyDown) Push(HOST_EV_KEY_DOWN, static_cast<int32_t>(a.VirtualKey()));
            a.Handled(true);
        });
        window.KeyUp([this](auto const&, KeyEventArgs const& a) {
            if (!gameReady_) return;
            Push(HOST_EV_KEY_UP, static_cast<int32_t>(a.VirtualKey()));
            a.Handled(true);
        });
        window.Activated([](auto const&, WindowActivatedEventArgs const& a) {
            bool focus = a.WindowActivationState() != CoreWindowActivationState::Deactivated;
            gFocus = focus;
            Push(HOST_EV_FOCUS, focus ? 1 : 0);
        });
        window.VisibilityChanged([this](auto const&, VisibilityChangedEventArgs const& a) {
            bool visible = a.Visible();
            Log(visible ? "shell: visible" : "shell: hidden");
            Push(HOST_EV_VISIBLE, visible ? 1 : 0);
            // the phone suspends a hidden app within seconds: the player comes back to a pause
            if (!visible && gStarted.load()) gRequests.fetch_or(HOST_REQUEST_PAUSE);
        });
        // The hardware/software Back key is Escape (pause, and back in the menus) instead of
        // leaving the game; on the files screen it closes the app, as usual
        try {
            SystemNavigationManager::GetForCurrentView().BackRequested(
                [this](auto const&, BackRequestedEventArgs const& a) {
                    if (!gameReady_) return;
                    a.Handled(true);
                    Push(HOST_EV_KEY_DOWN, VK_ESCAPE);
                    Push(HOST_EV_KEY_UP, VK_ESCAPE);
                });
        } catch (...) {
        }
    }

    winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const&) {
        return nullptr;
    }
    winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(hstring const&) { return nullptr; }
    com_array<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() { return {}; }

    std::wstring version_;
};

}  // namespace revc

// Desktop-partition API (kernel32 exports it to app-container processes too)
extern "C" __declspec(dllimport) PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    ::AddVectoredExceptionHandler(1, CrashNote);
    ::SetUnhandledExceptionFilter(LastChance);
    _set_invalid_parameter_handler(InvalidParameter);
    _set_purecall_handler(PureCall);
    signal(SIGABRT, Aborted);
    // The entry thread must be MTA; Application::Start makes the ASTA UI thread itself
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Application::Start([](auto&&) {
        WatchThisThread();
        winrt::make<revc::App>();
    });
    return 0;
}
