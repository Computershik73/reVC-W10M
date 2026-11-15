// The shell's side of host_api.h for a PC without a shell: the game runs in the current
// directory, draws into a pbuffer (there is no panel) and sees a pad nobody touches. What it
// prints and logs goes to stdout, a line at a time.
//
//   game_test [--frames N] [--press FRAME:BUTTON ...] [--key FRAME:VK ...]
//
// After N polls of the event queue (one a frame; 0: never) the shell "closes the window", and
// the game is expected to shut down by itself. --press holds a pad button (host_api.h's index)
// for a few frames from that frame on: enough to walk the game's menus. --key presses a key
// (a Windows virtual-key code, decimal) the same way: 123 is F12, the game's screenshot key.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "host_api.h"

namespace {

struct Press {
    long frame;
    int button;
};

long gFrames, gQuitAfter = 600;
int gContext = -1;
std::vector<Press> gPresses, gKeys;
LARGE_INTEGER gStart, gFrequency;

double Seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - gStart.QuadPart) / double(gFrequency.QuadPart);
}

void Line(const char* tag, const char* text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
    std::printf("[%7.3f] %s%.*s\n", Seconds(), tag, static_cast<int>(length), text);
}

}  // namespace

extern "C" void* host_panel(void) { return nullptr; }

extern "C" bool host_panel_size(int32_t* w, int32_t* h) {
    *w = 1280;
    *h = 720;
    return true;
}

extern "C" bool host_panel_native_size(int32_t* w, int32_t* h) { return host_panel_size(w, h); }

// Polled until it is empty, once a frame: the end of each poll is the frame count
extern "C" bool host_poll_event(HostEvent* event) {
    static bool closing;
    if (!closing && gQuitAfter > 0 && gFrames >= gQuitAfter) {
        closing = true;
        Line("test: ", "closing the window");
        event->type = HOST_EV_QUIT;
        event->code = 0;
        event->x = event->y = 0;
        return true;
    }
    // a key goes down at the start of its frame and up three frames later
    static long keysDone = -1;
    if (keysDone < gFrames) {
        static size_t next;
        for (; next < gKeys.size() * 2; next++) {
            const Press& key = gKeys[next / 2];
            long due = key.frame + (next % 2 ? 3 : 0);
            if (due > gFrames) break;
            event->type = next % 2 ? HOST_EV_KEY_UP : HOST_EV_KEY_DOWN;
            event->code = key.button;
            event->x = event->y = 0;
            next++;
            return true;
        }
        keysDone = gFrames;
    }
    gFrames++;
    return false;
}

extern "C" bool host_has_focus(void) { return true; }

extern "C" void host_pad_state(HostPad* pad) {
    memset(pad, 0, sizeof(*pad));
    pad->axes[HOST_AXIS_LT] = pad->axes[HOST_AXIS_RT] = -1.0f;
    for (const Press& press : gPresses)
        if (gFrames >= press.frame && gFrames < press.frame + 4) pad->buttons[press.button] = 1;
}

extern "C" void host_set_context(int32_t context) {
    static const char* const names[] = {"a menu", "on foot", "in a vehicle", "a cutscene", "loading"};
    if (context == gContext) return;
    gContext = context;
    char text[96];
    snprintf(text, sizeof(text), "the game is in: %s (frame %ld)", context >= 0 && context < 5 ? names[context] : "?", gFrames);
    Line("test: ", text);
}

extern "C" int32_t host_take_requests(void) { return 0; }

extern "C" const wchar_t* host_local_folder(void) { return nullptr; }

extern "C" void host_set_pad_style(int32_t) {}

extern "C" void host_log(const char* text) { Line("", text); }

extern "C" void host_logf(const char* format, ...) {
    char text[1024];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    Line("", text);
}

extern "C" void host_fatal(const char* text) {
    Line("FATAL: ", text);
    fflush(stdout);
    ExitProcess(3);
}

static LONG CALLBACK CrashNote(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != STATUS_STACK_BUFFER_OVERRUN)
        return EXCEPTION_CONTINUE_SEARCH;
    void* address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module))
        GetModuleFileNameA(module, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    char text[320];
    snprintf(text, sizeof(text), "exception 0x%08lX at %s+0x%llX, data address %p (frame %ld)", code, base ? base + 1 : name,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)),
             info->ExceptionRecord->NumberParameters > 1 ? reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1])
                                                         : nullptr,
             gFrames);
    Line("CRASH: ", text);
    fflush(stdout);
    ExitProcess(4);
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    QueryPerformanceFrequency(&gFrequency);
    QueryPerformanceCounter(&gStart);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            gQuitAfter = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            Press press{};
            if (sscanf(argv[++i], "%ld:%d", &press.frame, &press.button) == 2 && press.button >= 0 && press.button < HOST_PAD_BUTTONS)
                gPresses.push_back(press);
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            Press key{};
            if (sscanf(argv[++i], "%ld:%d", &key.frame, &key.button) == 2) gKeys.push_back(key);
        }
    }
    AddVectoredExceptionHandler(1, CrashNote);
    static char name[] = "reVC.exe";
    char* args[] = {name, nullptr};
    Line("test: ", "starting the game");
    int result = reVC_Main(1, args);
    char text[64];
    snprintf(text, sizeof(text), "the game returned %d after %ld frames", result, gFrames);
    Line("test: ", text);
    return 0;
}
