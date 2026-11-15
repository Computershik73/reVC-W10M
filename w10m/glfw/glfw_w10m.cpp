// GLFW's API (the part in include/GLFW/glfw3.h) for Windows 10 Mobile.
//
// The window is the shell's XAML SwapChainPanel: ANGLE presents OpenGL ES into it through EGL.
// Events come from the shell's queue (host_api.h), which the UI thread fills; everything here
// runs on the game thread, as GLFW's callbacks do on a desktop. Joystick 0 is the pad the shell
// keeps: the on-screen controls, or a real gamepad.
//
// ANGLE on Direct3D 11 gives OpenGL ES 3.0 from feature level 10_1 up and ES 2.0 on the 9_x
// levels most Windows phones stop at; librw asks for 3.1, 3.0 and 2.0 in turn.
//
// On an OpenGL ES 2.0 context the first frame is the last one the game thread draws itself:
// from the second on OpenGL is called by a drawing thread (w10m/glthread), which has the
// context, and what this file does with EGL goes there too.
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <windows.h>
#include <dxgi.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "glthread.h"
#include "host_api.h"

// ANGLE (the gecko-w10m sources butterscotch-w10m builds): the render thread cannot ask XAML
// for the panel's size, so the shell's figures are handed over.
extern "C" __declspec(dllimport) void angle_uwp_set_panel_size(int32_t width, int32_t height);

// src/skel/w10m/w10m.cpp: once a frame, on the game thread; what the player is doing
// (HOST_CONTEXT_*) comes back. The second: from now on a cutscene is skipped as it starts.
extern "C" int32_t reVC_W10M_Frame(void);
extern "C" void reVC_W10M_SkipCutscenes(void);

struct GLFWmonitor {
    GLFWvidmode mode;
};

struct GLFWwindow {
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0, height = 0;
    int esMajor = 0;            // the OpenGL ES version the context was asked for and given
    bool threadTried = false;   // the drawing thread has been started for this context, or not wanted
    bool shouldClose = false;
    bool entered = false;

    double cursorX = 1, cursorY = 1;
    bool buttons[GLFW_MOUSE_BUTTON_LAST + 1] = {};

    GLFWframebuffersizefun onFramebufferSize = nullptr;
    GLFWwindowfocusfun onFocus = nullptr;
    GLFWwindowiconifyfun onIconify = nullptr;
    GLFWkeyfun onKey = nullptr;
    GLFWscrollfun onScroll = nullptr;
    GLFWcursorposfun onCursorPos = nullptr;
    GLFWcursorenterfun onCursorEnter = nullptr;
};

namespace {

struct Hints {
    int clientApi = GLFW_OPENGL_API;
    int major = 1, minor = 0;
    int samples = 0;
};

// A touch on a menu is a mouse: it moves the pointer there and clicks. The menu works out what
// is under the pointer while it draws, so the press must not arrive in the frame the pointer
// moved in, nor the release in the frame of the press.
struct MouseStep {
    enum Kind { Move, Down, Up } kind;
    double x, y;
};

GLFWmonitor gMonitor;
GLFWmonitor* gMonitors[1] = {&gMonitor};
GLFWwindow* gWindow = nullptr;
Hints gHints;
GLFWerrorfun gOnError = nullptr;
GLFWjoystickfun gOnJoystick = nullptr;
EGLDisplay gDisplay = EGL_NO_DISPLAY;
EGLConfig gConfig = nullptr;
int gSwapInterval = -1;
HostPad gPad;
std::deque<MouseStep> gMouse;
// What the swaps themselves took on the drawing thread, for NotePerformance: where the GPU
// is behind, it is here that the thread waits for it.
std::atomic<LONGLONG> gShowTicks{0};
LONGLONG gScriptStart;  // when the clock of REVC_KEYS began: 0 until the menu is up

void Fail(const char* what) {
    char text[160];
    snprintf(text, sizeof(text), "egl: %s failed, error 0x%04x", what, (unsigned)eglGetError());
    host_log(text);
    if (gOnError) gOnError(0x00010008 /* GLFW_PLATFORM_ERROR */, text);
}

void RefreshMode() {
    int32_t w = 0, h = 0;
    if (!host_panel_size(&w, &h)) {
        // the shell starts the game once the panel is laid out; a size is a size all the same
        w = 1280;
        h = 720;
    }
    gMonitor.mode.width = w;
    gMonitor.mode.height = h;
    gMonitor.mode.redBits = gMonitor.mode.greenBits = gMonitor.mode.blueBits = 8;
    gMonitor.mode.refreshRate = 60;
}

// REVC_ANGLE_FL=9_3 (or 9_1, 10_0...) caps the Direct3D feature level, to try on one phone
// what another gets: 9_3 is OpenGL ES 2.0, as on most Lumias.
bool OpenDisplay() {
    if (gDisplay != EGL_NO_DISPLAY) return true;
    int major = 0, minor = 0;
    const char* cap = getenv("REVC_ANGLE_FL");
    if (cap && sscanf(cap, "%d_%d", &major, &minor) == 2) {
        auto getPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        const EGLint capped[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, major,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, minor,
            EGL_NONE,
        };
        host_logf("egl: capped at Direct3D feature level %d_%d (REVC_ANGLE_FL)", major, minor);
        gDisplay = getPlatformDisplay
                       ? getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY), capped)
                       : EGL_NO_DISPLAY;
    } else {
        gDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    EGLint eglMajor = 0, eglMinor = 0;
    if (gDisplay == EGL_NO_DISPLAY || !eglInitialize(gDisplay, &eglMajor, &eglMinor)) {
        Fail("eglInitialize");
        gDisplay = EGL_NO_DISPLAY;
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        Fail("eglBindAPI");
        return false;
    }

    // Depth and stencil for the game. No alpha if there is such a config: a swap chain with
    // alpha is blended over the XAML behind the panel, and what the game leaves in the back
    // buffer's alpha is not coverage. Without a panel (w10m/test/render) the surface is a pbuffer.
    const EGLint wanted[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SURFACE_TYPE, host_panel() ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(gDisplay, wanted, configs, 64, &count) || count < 1) {
        Fail("eglChooseConfig");
        return false;
    }
    gConfig = configs[0];
    for (EGLint i = 0; i < count; i++) {
        EGLint alpha = 8, red = 0;
        eglGetConfigAttrib(gDisplay, configs[i], EGL_ALPHA_SIZE, &alpha);
        eglGetConfigAttrib(gDisplay, configs[i], EGL_RED_SIZE, &red);
        if (alpha == 0 && red == 8) {
            gConfig = configs[i];
            break;
        }
    }
    EGLint alpha = 0, depth = 0, stencil = 0;
    eglGetConfigAttrib(gDisplay, gConfig, EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(gDisplay, gConfig, EGL_DEPTH_SIZE, &depth);
    eglGetConfigAttrib(gDisplay, gConfig, EGL_STENCIL_SIZE, &stencil);
    const char* vendor = eglQueryString(gDisplay, EGL_VENDOR);
    host_logf("egl: %d.%d, %s; config with alpha %d, depth %d, stencil %d", (int)eglMajor, (int)eglMinor,
              vendor ? vendor : "?", (int)alpha, (int)depth, (int)stencil);
    return true;
}

void SyncPanelSize(GLFWwindow* window, bool notify) {
    int32_t w = 0, h = 0;
    if (!host_panel_size(&w, &h) || (w == window->width && h == window->height)) return;
    // in its turn among the frames, where there is a drawing thread: ANGLE resizes at a swap
    glt_post([](void* size) { angle_uwp_set_panel_size((int32_t)((uintptr_t)size >> 16), (int32_t)((uintptr_t)size & 0xFFFF)); },
             reinterpret_cast<void*>(((uintptr_t)w << 16) | ((uintptr_t)h & 0xFFFF)));
    window->width = w;
    window->height = h;
    RefreshMode();
    if (notify) {
        host_logf("glfw: render target %dx%d", (int)w, (int)h);
        if (window->onFramebufferSize) window->onFramebufferSize(window, w, h);
    }
}

int KeyFromVirtualKey(int vk) {
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= 'A' && vk <= 'Z') return vk;
    if (vk >= VK_F1 && vk <= VK_F24) return GLFW_KEY_F1 + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return GLFW_KEY_KP_0 + (vk - VK_NUMPAD0);
    switch (vk) {
        case VK_SPACE: return GLFW_KEY_SPACE;
        case VK_OEM_7: return GLFW_KEY_APOSTROPHE;
        case VK_OEM_COMMA: return GLFW_KEY_COMMA;
        case VK_OEM_MINUS: return GLFW_KEY_MINUS;
        case VK_OEM_PERIOD: return GLFW_KEY_PERIOD;
        case VK_OEM_2: return GLFW_KEY_SLASH;
        case VK_OEM_1: return GLFW_KEY_SEMICOLON;
        case VK_OEM_PLUS: return GLFW_KEY_EQUAL;
        case VK_OEM_4: return GLFW_KEY_LEFT_BRACKET;
        case VK_OEM_5: return GLFW_KEY_BACKSLASH;
        case VK_OEM_6: return GLFW_KEY_RIGHT_BRACKET;
        case VK_OEM_3: return GLFW_KEY_GRAVE_ACCENT;
        case VK_ESCAPE: return GLFW_KEY_ESCAPE;
        case VK_RETURN: return GLFW_KEY_ENTER;
        case VK_TAB: return GLFW_KEY_TAB;
        case VK_BACK: return GLFW_KEY_BACKSPACE;
        case VK_INSERT: return GLFW_KEY_INSERT;
        case VK_DELETE: return GLFW_KEY_DELETE;
        case VK_RIGHT: return GLFW_KEY_RIGHT;
        case VK_LEFT: return GLFW_KEY_LEFT;
        case VK_DOWN: return GLFW_KEY_DOWN;
        case VK_UP: return GLFW_KEY_UP;
        case VK_PRIOR: return GLFW_KEY_PAGE_UP;
        case VK_NEXT: return GLFW_KEY_PAGE_DOWN;
        case VK_HOME: return GLFW_KEY_HOME;
        case VK_END: return GLFW_KEY_END;
        case VK_CAPITAL: return GLFW_KEY_CAPS_LOCK;
        case VK_SCROLL: return GLFW_KEY_SCROLL_LOCK;
        case VK_NUMLOCK: return GLFW_KEY_NUM_LOCK;
        case VK_SNAPSHOT: return GLFW_KEY_PRINT_SCREEN;
        case VK_PAUSE: return GLFW_KEY_PAUSE;
        case VK_DECIMAL: return GLFW_KEY_KP_DECIMAL;
        case VK_DIVIDE: return GLFW_KEY_KP_DIVIDE;
        case VK_MULTIPLY: return GLFW_KEY_KP_MULTIPLY;
        case VK_SUBTRACT: return GLFW_KEY_KP_SUBTRACT;
        case VK_ADD: return GLFW_KEY_KP_ADD;
        case VK_SHIFT:
        case VK_LSHIFT: return GLFW_KEY_LEFT_SHIFT;
        case VK_RSHIFT: return GLFW_KEY_RIGHT_SHIFT;
        case VK_CONTROL:
        case VK_LCONTROL: return GLFW_KEY_LEFT_CONTROL;
        case VK_RCONTROL: return GLFW_KEY_RIGHT_CONTROL;
        case VK_MENU:
        case VK_LMENU: return GLFW_KEY_LEFT_ALT;
        case VK_RMENU: return GLFW_KEY_RIGHT_ALT;
        case VK_LWIN: return GLFW_KEY_LEFT_SUPER;
        case VK_RWIN: return GLFW_KEY_RIGHT_SUPER;
        case VK_APPS: return GLFW_KEY_MENU;
        default: return GLFW_KEY_UNKNOWN;
    }
}

void MoveCursor(GLFWwindow* window, double x, double y) {
    // the game reads x == 0 as "there is no mouse"
    window->cursorX = x < 1 ? 1 : x;
    window->cursorY = y;
    if (window->onCursorPos) window->onCursorPos(window, window->cursorX, window->cursorY);
}

// As far as the frames allow: any number of moves, then one press or release, and a press
// not in the poll its move was in.
void StepMouse(GLFWwindow* window) {
    bool moved = false;
    while (!gMouse.empty()) {
        MouseStep step = gMouse.front();
        if (step.kind == MouseStep::Move) {
            gMouse.pop_front();
            MoveCursor(window, step.x, step.y);
            moved = true;
            continue;
        }
        if (step.kind == MouseStep::Down && moved) return;
        gMouse.pop_front();
        window->buttons[GLFW_MOUSE_BUTTON_LEFT] = step.kind == MouseStep::Down;
        return;
    }
}

// Frames and where their time goes, every 10 s: the log is the only window into the phone.
void NotePerformance(LONGLONG swapStart, LONGLONG swapEnd, GLFWwindow* window) {
    static LARGE_INTEGER frequency;
    static LONGLONG windowStart, lastEnd, swapTicks, worst;
    static int frames, slow;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    if (!windowStart) windowStart = swapEnd;
    if (lastEnd) {
        LONGLONG interval = swapEnd - lastEnd;
        if (interval > worst) worst = interval;
        if (interval * 20 > frequency.QuadPart) slow++;
    }
    swapTicks += swapEnd - swapStart;
    lastEnd = swapEnd;
    frames++;
    if (swapEnd - windowStart < frequency.QuadPart * 10) return;
    double seconds = (double)(swapEnd - windowStart) / (double)frequency.QuadPart;
    host_logf("perf: %.1f fps, %s %.1f ms a frame, %d frames over 50 ms (worst %d ms), target %dx%d",
              frames / seconds, glt_on() ? "held at the frame's end" : "swap",
              swapTicks * 1000.0 / (double)frequency.QuadPart / frames, slow,
              (int)(worst * 1000 / frequency.QuadPart), window->width, window->height);
    if (glt_on()) {
        // the two threads' shares: the frame takes the longer of them, not their sum
        GltStats stats;
        glt_stats(&stats);
        host_logf("perf: the drawing thread worked %.1f ms a frame on %.0f calls, %.1f ms of it in the swap; the game's "
                  "thread waited for %u calls with an answer (%.1f ms a frame) and found the queue full %u times",
                  stats.drawingMs / frames, (double)stats.calls / frames,
                  gShowTicks.exchange(0) * 1000.0 / (double)frequency.QuadPart / frames, (unsigned)stats.answers,
                  stats.answerWaitMs / frames, (unsigned)stats.timesFull);
    }
    windowStart = swapEnd;
    frames = slow = 0;
    swapTicks = worst = 0;
}

LONGLONG Ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double SecondsSince(LONGLONG start) {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    return (double)(Ticks() - start) / (double)frequency.QuadPart;
}

// ===[ For a run nobody is holding the phone for ]===
// The log is the only window into a phone, and the Device Portal has no screen to show and no
// finger to lend. Two settings (LocalState\env.NAME=value, see the shell) make up for that.

// The OpenGL ES calls this file makes itself, by name: it is linked against EGL alone.
struct GlCalls {
    void(__stdcall* readPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
    void(__stdcall* getIntegerv)(unsigned, int*) = nullptr;
    void(__stdcall* bindFramebuffer)(unsigned, unsigned) = nullptr;
};

// REVC_FRAMESHOT=5: every so many seconds, and for the first frame, the picture about to be
// shown is saved as LocalState\frame.bmp (640 wide at most), to be fetched with
// tools/phone.py file frame.bmp. The shell's own snapshots (REVC_SNAPSHOT) have its XAML only.
void SaveFrame(GLFWwindow* window, int frame) {
    static double every = -1;  // seconds; 0: off
    static bool keep;          // REVC_FRAMESHOT=1.5k: every one under a name of its own, frame_001.bmp...
    static int kept;
    static LONGLONG last;
    static GlCalls gl;
    if (every < 0) {
        const char* value = getenv("REVC_FRAMESHOT");
        every = value ? atof(value) : 0;
        keep = value && strchr(value, 'k');
        if (value && every < 0.2) every = keep ? 0.2 : 1;
        if (every) {
            gl.readPixels = reinterpret_cast<decltype(gl.readPixels)>(eglGetProcAddress("glReadPixels"));
            gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(eglGetProcAddress("glGetIntegerv"));
            gl.bindFramebuffer = reinterpret_cast<decltype(gl.bindFramebuffer)>(eglGetProcAddress("glBindFramebuffer"));
            if (!gl.readPixels || !gl.getIntegerv || !gl.bindFramebuffer || !host_local_folder()) every = 0;
            host_logf("glfw: REVC_FRAMESHOT, %s every %.1f s", keep ? "a frame_NNN.bmp" : "frame.bmp", every);
        }
    }
    if (!every || (last && SecondsSince(last) < every)) return;
    last = Ticks();
    int w = window->width, h = window->height;
    if (w <= 0 || h <= 0) return;
    std::vector<unsigned char> pixels((size_t)w * h * 4);
    struct Grab {
        GlCalls* gl;
        int w, h;
        unsigned char* pixels;
    } grab = {&gl, w, h, pixels.data()};
    // where the context is, and once everything drawn so far is in the picture
    glt_call(
        [](void* p) {
            Grab* g = static_cast<Grab*>(p);
            int bound = 0;
            g->gl->getIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &bound);
            if (bound) g->gl->bindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, 0);
            g->gl->readPixels(0, 0, g->w, g->h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, g->pixels);
            if (bound) g->gl->bindFramebuffer(0x8D40, (unsigned)bound);
        },
        &grab);
    // a BMP's rows go bottom to top, as OpenGL's do
    int step = keep ? (w + 319) / 320 : (w + 639) / 640;  // a series is of small pictures
    int outW = w / step, outH = h / step;
    int stride = (outW * 3 + 3) & ~3;
    std::vector<unsigned char> file(54 + (size_t)stride * outH);
    auto put32 = [&file](size_t at, unsigned value) {
        for (int i = 0; i < 4; i++) file[at + i] = (unsigned char)(value >> (8 * i));
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, (unsigned)file.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, (unsigned)outW);
    put32(22, (unsigned)outH);
    file[26] = 1;
    file[28] = 24;
    put32(34, (unsigned)(stride * outH));
    for (int y = 0; y < outH; y++) {
        const unsigned char* from = &pixels[(size_t)y * step * w * 4];
        unsigned char* to = &file[54 + (size_t)y * stride];
        for (int x = 0; x < outW; x++, from += 4 * step, to += 3) {
            to[0] = from[2];
            to[1] = from[1];
            to[2] = from[0];
        }
    }
    // under another name first: the Device Portal must not be handed half a file
    std::wstring folder = host_local_folder();
    wchar_t name[32] = L"\\frame.bmp";
    if (keep) swprintf(name, 32, L"\\frame_%03d.bmp", ++kept);
    std::wstring partial = folder + L"\\frame.tmp", whole = folder + name;
    FILE* out = _wfopen(partial.c_str(), L"wb");
    if (!out) return;
    bool written = fwrite(file.data(), 1, file.size(), out) == file.size();
    fclose(out);
    if (written && MoveFileExW(partial.c_str(), whole.c_str(), MOVEFILE_REPLACE_EXISTING))
        // with the script's clock, which is what the keys of REVC_KEYS are timed by
        host_logf("glfw: %ls is frame %d, %dx%d, at %.1f s of the script's clock", name + 1, frame, outW, outH,
                  gScriptStart ? SecondsSince(gScriptStart) : -1.0);
}

// REVC_KEYS=13@3,13@6,s@8: virtual key 13 (Enter) goes down 3 seconds after the game's menu
// first came up, and again at 6 (a new game, from the first menu); each comes up again in a
// later poll, as a finger's would. s: from then on a cutscene is skipped as it starts. The
// clock starts with the menu because what comes before it takes six seconds on one phone and
// eighty on another. 87@30-50: the key (W, the accelerator of the game's PC keys) is held
// from 30 to 50, which is how a run drives itself: 65 and 68 (A, D) steer, 83 (S) brakes.
struct ScriptedKey {
    int vk;  // 0: skip the cutscenes
    double at;
    double until;  // when it comes up again
    int state;  // 0: to come, 1: down, 2: done
};
std::vector<ScriptedKey> gScript;

void LoadScript() {
    gScript.clear();
    gScriptStart = 0;
    const char* text = getenv("REVC_KEYS");
    while (text && *text) {
        char* end = nullptr;
        long vk = 0;
        if (*text == 's') {
            end = const_cast<char*>(text) + 1;
        } else {
            vk = strtol(text, &end, 10);
            if (end == text || vk <= 0) break;
        }
        if (*end != '@') break;
        double at = strtod(end + 1, &end);
        double until = at + 0.15;
        if (*end == '-') until = strtod(end + 1, &end);
        gScript.push_back({(int)vk, at, until, 0});
        if (*end != ',') break;
        text = end + 1;
    }
    if (!gScript.empty()) host_logf("glfw: REVC_KEYS, %d steps once the menu is up", (int)gScript.size());
}

void StepScript(GLFWwindow* window, int32_t context) {
    if (gScript.empty()) return;
    if (!gScriptStart) {
        if (context != HOST_CONTEXT_MENU) return;
        gScriptStart = Ticks();
    }
    double now = SecondsSince(gScriptStart);
    for (ScriptedKey& key : gScript) {
        if (key.state == 2 || now < (key.state ? key.until : key.at)) continue;
        if (key.vk == 0) {
            host_log("glfw: scripted: cutscenes are skipped from here on");
            reVC_W10M_SkipCutscenes();
            key.state = 2;
            continue;
        }
        int code = KeyFromVirtualKey(key.vk);
        if (key.state == 0) host_logf("glfw: scripted key %d%s", key.vk, code == GLFW_KEY_UNKNOWN ? " (not a key GLFW has)" : "");
        if (code != GLFW_KEY_UNKNOWN && window->onKey) window->onKey(window, code, 0, key.state ? GLFW_RELEASE : GLFW_PRESS, 0);
        key.state++;
    }
}

// ===[ The frame's end ]===

// On the thread that has the context: the game's, or the drawing thread.
void Show(void* param) {
    GLFWwindow* window = static_cast<GLFWwindow*>(param);
    static int shown;
    LONGLONG from = Ticks();
    if (!eglSwapBuffers(gDisplay, window->surface)) {
        static int noted;
        if (noted++ < 8) Fail("eglSwapBuffers");
    }
    gShowTicks.fetch_add(Ticks() - from, std::memory_order_relaxed);
    // the first ones by name: what a phone does next has more than once depended on them
    if (++shown <= 3) host_logf("glfw: frame %d is shown (swap interval %d)", shown, gSwapInterval);
}

// After a context's first frame: librw has looked its functions up by then, and what it does
// once (its shaders, compiled or read back) is behind it. REVC_GL_THREAD=1 or 0 says yes or
// no; left alone, an OpenGL ES 2.0 context gets the thread, which is to say a GPU of Direct3D
// level 9, where the calls are what a frame's time goes into.
void StartDrawingThread(GLFWwindow* window) {
    if (window->threadTried) return;
    window->threadTried = true;
    const char* setting = getenv("REVC_GL_THREAD");
    if (!(setting ? atoi(setting) != 0 : window->esMajor < 3)) return;
    if (glt_start(host_log))
        host_log("glfw: from here on OpenGL ES is called on a drawing thread, the game's own goes on meanwhile "
                 "(REVC_GL_THREAD=0: as before)");
}

}  // namespace

extern "C" {

int glfwInit(void) {
    RefreshMode();
    memset(&gPad, 0, sizeof(gPad));
    gPad.axes[HOST_AXIS_LT] = gPad.axes[HOST_AXIS_RT] = -1.0f;
    return GLFW_TRUE;
}

void glfwTerminate(void) {
    if (gWindow) glfwDestroyWindow(gWindow);
    if (gDisplay != EGL_NO_DISPLAY) eglTerminate(gDisplay);
    gDisplay = EGL_NO_DISPLAY;
}

GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun callback) {
    GLFWerrorfun old = gOnError;
    gOnError = callback;
    return old;
}

/* ===[ The one monitor: the phone's screen, in the one mode the panel has ]=== */

GLFWmonitor** glfwGetMonitors(int* count) {
    if (count) *count = 1;
    return gMonitors;
}

GLFWmonitor* glfwGetPrimaryMonitor(void) { return &gMonitor; }

const char* glfwGetMonitorName(GLFWmonitor*) { return "Screen"; }

const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* monitor, int* count) {
    RefreshMode();
    if (count) *count = 1;
    return &monitor->mode;
}

const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* monitor) {
    RefreshMode();
    return &monitor->mode;
}

/* ===[ Window and context ]=== */

void glfwWindowHint(int hint, int value) {
    switch (hint) {
        case GLFW_CLIENT_API: gHints.clientApi = value; break;
        case GLFW_CONTEXT_VERSION_MAJOR: gHints.major = value; break;
        case GLFW_CONTEXT_VERSION_MINOR: gHints.minor = value; break;
        case GLFW_SAMPLES: gHints.samples = value; break;
        default: break;  // colour depth and refresh rate are the panel's
    }
}

GLFWwindow* glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    if (gHints.clientApi != GLFW_OPENGL_ES_API) return nullptr;  // ANGLE is OpenGL ES
    if (gWindow) {
        host_log("glfw: a second window was asked for");
        return nullptr;
    }
    if (!OpenDisplay()) return nullptr;

    GLFWwindow* window = new GLFWwindow();
    SyncPanelSize(window, false);  // ANGLE sizes the swap chain from it

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, gHints.major,
        EGL_CONTEXT_MINOR_VERSION, gHints.minor,
        EGL_NONE,
    };
    window->context = eglCreateContext(gDisplay, gConfig, EGL_NO_CONTEXT, contextAttribs);
    if (window->context == EGL_NO_CONTEXT) {
        // not an error: librw goes down its list of versions
        host_logf("egl: no OpenGL ES %d.%d context (0x%04x)", gHints.major, gHints.minor, (unsigned)eglGetError());
        delete window;
        return nullptr;
    }
    if (void* panel = host_panel()) {
        window->surface = eglCreateWindowSurface(gDisplay, gConfig, static_cast<EGLNativeWindowType>(panel), nullptr);
    } else {
        // no shell, no screen: the tests draw off-screen
        const EGLint size[] = {EGL_WIDTH, window->width, EGL_HEIGHT, window->height, EGL_NONE};
        window->surface = eglCreatePbufferSurface(gDisplay, gConfig, size);
        // From feature level 10_0 up ANGLE makes a pbuffer a shared texture behind a keyed mutex,
        // and Direct3D drops every draw into it until someone holds the mutex (found by
        // butterscotch-w10m's desktop check). Nobody else will here.
        auto queryPointer =
            reinterpret_cast<PFNEGLQUERYSURFACEPOINTERANGLEPROC>(eglGetProcAddress("eglQuerySurfacePointerANGLE"));
        void* mutex = nullptr;
        if (window->surface != EGL_NO_SURFACE && queryPointer &&
            queryPointer(gDisplay, window->surface, EGL_DXGI_KEYED_MUTEX_ANGLE, &mutex) && mutex)
            static_cast<IDXGIKeyedMutex*>(mutex)->AcquireSync(0, INFINITE);
    }
    if (window->surface == EGL_NO_SURFACE) {
        Fail("eglCreateWindowSurface");
        eglDestroyContext(gDisplay, window->context);
        delete window;
        return nullptr;
    }
    host_logf("glfw: OpenGL ES %d.%d context, render target %dx%d", gHints.major, gHints.minor, window->width,
              window->height);
    window->esMajor = gHints.major;
    gSwapInterval = -1;
    gMouse.clear();
    LoadScript();
    gWindow = window;
    return window;
}

void glfwDestroyWindow(GLFWwindow* window) {
    if (!window) return;
    glt_stop();  // the context comes back first, with everything asked of it done
    if (gDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (window->surface != EGL_NO_SURFACE) eglDestroySurface(gDisplay, window->surface);
        if (window->context != EGL_NO_CONTEXT) eglDestroyContext(gDisplay, window->context);
    }
    if (gWindow == window) gWindow = nullptr;
    delete window;
}

int glfwWindowShouldClose(GLFWwindow* window) { return window && window->shouldClose; }

void glfwSetWindowPos(GLFWwindow*, int, int) {}

void glfwSetWindowSize(GLFWwindow*, int, int) {}  // the panel fills the screen

void glfwGetWindowSize(GLFWwindow* window, int* width, int* height) {
    if (width) *width = window->width;
    if (height) *height = window->height;
}

void glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height) {
    if (width) *width = window->width;
    if (height) *height = window->height;
}

void glfwMakeContextCurrent(GLFWwindow* window) {
    if (glt_on()) {
        // whoever asks means this thread: the drawing thread gives the context back, and is
        // started again after the next frame
        glt_stop();
        if (gWindow) gWindow->threadTried = false;
    }
    if (!window) {
        if (gDisplay != EGL_NO_DISPLAY) eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return;
    }
    if (!eglMakeCurrent(gDisplay, window->surface, window->surface, window->context)) Fail("eglMakeCurrent");
}

void glfwSwapInterval(int interval) {
    if (interval == gSwapInterval || gDisplay == EGL_NO_DISPLAY) return;  // librw sets it before every swap
    gSwapInterval = interval;
    // it is the current context's surface that EGL sets it for
    glt_call([](void* value) { eglSwapInterval(gDisplay, (EGLint)(intptr_t)value); }, reinterpret_cast<void*>((intptr_t)interval));
}

void glfwSwapBuffers(GLFWwindow* window) {
    static int frames;
    frames++;
    SyncPanelSize(window, true);
    SaveFrame(window, frames);
    LONGLONG start = Ticks();
    if (glt_on()) {
        glt_frame(Show, window);  // in its turn; held here while the frame before is not shown yet
    } else {
        Show(window);
        StartDrawingThread(window);
    }
    NotePerformance(start, Ticks(), window);
}

GLFWglproc glfwGetProcAddress(const char* procname) {
    return reinterpret_cast<GLFWglproc>(eglGetProcAddress(procname));
}

/* ===[ Callbacks ]=== */

#define SET_CALLBACK(member)                 \
    auto old = window->member;               \
    window->member = callback;               \
    return old

GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* window, GLFWframebuffersizefun callback) {
    SET_CALLBACK(onFramebufferSize);
}
GLFWwindowfocusfun glfwSetWindowFocusCallback(GLFWwindow* window, GLFWwindowfocusfun callback) { SET_CALLBACK(onFocus); }
GLFWwindowiconifyfun glfwSetWindowIconifyCallback(GLFWwindow* window, GLFWwindowiconifyfun callback) {
    SET_CALLBACK(onIconify);
}
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* window, GLFWkeyfun callback) { SET_CALLBACK(onKey); }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow* window, GLFWscrollfun callback) { SET_CALLBACK(onScroll); }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow* window, GLFWcursorposfun callback) { SET_CALLBACK(onCursorPos); }
GLFWcursorenterfun glfwSetCursorEnterCallback(GLFWwindow* window, GLFWcursorenterfun callback) {
    SET_CALLBACK(onCursorEnter);
}

#undef SET_CALLBACK

GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun callback) {
    GLFWjoystickfun old = gOnJoystick;
    gOnJoystick = callback;
    return old;
}

/* ===[ Events ]=== */

void glfwPollEvents(void) {
    GLFWwindow* window = gWindow;
    if (!window) return;

    // the pointer is always "in the window": the game ignores the mouse otherwise
    if (!window->entered && window->onCursorEnter) {
        window->entered = true;
        window->onCursorEnter(window, GLFW_TRUE);
    }

    HostEvent e;
    while (host_poll_event(&e)) {
        switch (e.type) {
            case HOST_EV_KEY_DOWN:
            case HOST_EV_KEY_UP: {
                int key = KeyFromVirtualKey(e.code);
                if (key != GLFW_KEY_UNKNOWN && window->onKey)
                    window->onKey(window, key, 0, e.type == HOST_EV_KEY_DOWN ? GLFW_PRESS : GLFW_RELEASE, 0);
                break;
            }
            case HOST_EV_POINTER_DOWN:
                gMouse.push_back({MouseStep::Move, e.x, e.y});
                gMouse.push_back({MouseStep::Down, e.x, e.y});
                break;
            case HOST_EV_POINTER_MOVE:
                gMouse.push_back({MouseStep::Move, e.x, e.y});
                break;
            case HOST_EV_POINTER_UP:
                gMouse.push_back({MouseStep::Move, e.x, e.y});
                gMouse.push_back({MouseStep::Up, e.x, e.y});
                break;
            case HOST_EV_WHEEL:
                if (window->onScroll) window->onScroll(window, 0.0, e.y);
                break;
            case HOST_EV_FOCUS:
                if (window->onFocus) window->onFocus(window, e.code ? GLFW_TRUE : GLFW_FALSE);
                break;
            case HOST_EV_VISIBLE:
                if (window->onIconify) window->onIconify(window, e.code ? GLFW_FALSE : GLFW_TRUE);
                break;
            case HOST_EV_QUIT:
                window->shouldClose = true;
                break;
            default:
                break;
        }
    }
    StepMouse(window);
    SyncPanelSize(window, true);
    host_pad_state(&gPad);
    StepScript(window, reVC_W10M_Frame());
}

void glfwSetInputMode(GLFWwindow*, int, int) {}  // there is no cursor to hide or capture

int glfwGetMouseButton(GLFWwindow* window, int button) {
    if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST) return GLFW_RELEASE;
    return window->buttons[button] ? GLFW_PRESS : GLFW_RELEASE;
}

void glfwGetCursorPos(GLFWwindow* window, double* xpos, double* ypos) {
    if (xpos) *xpos = window->cursorX;
    if (ypos) *ypos = window->cursorY;
}

void glfwSetCursorPos(GLFWwindow* window, double xpos, double ypos) {
    window->cursorX = xpos < 1 ? 1 : xpos;
    window->cursorY = ypos;
}

/* ===[ Joystick 0: the shell's pad ]=== */

int glfwJoystickPresent(int jid) { return jid == GLFW_JOYSTICK_1 ? GLFW_TRUE : GLFW_FALSE; }

const float* glfwGetJoystickAxes(int jid, int* count) {
    if (jid != GLFW_JOYSTICK_1) {
        if (count) *count = 0;
        return nullptr;
    }
    if (count) *count = HOST_PAD_AXES;
    return gPad.axes;
}

// Sixteen buttons: the fifteen of a gamepad in GLFW's order and one spare. The game copies
// its whole button array from here and binds its defaults by this count.
const unsigned char* glfwGetJoystickButtons(int jid, int* count) {
    static unsigned char buttons[64];
    if (jid != GLFW_JOYSTICK_1) {
        if (count) *count = 0;
        return nullptr;
    }
    memset(buttons, 0, sizeof(buttons));
    memcpy(buttons, gPad.buttons, HOST_PAD_BUTTONS);
    if (count) *count = 16;
    return buttons;
}

const char* glfwGetJoystickName(int jid) { return jid == GLFW_JOYSTICK_1 ? "Windows 10 Mobile pad" : nullptr; }

int glfwJoystickIsGamepad(int jid) { return jid == GLFW_JOYSTICK_1 ? GLFW_TRUE : GLFW_FALSE; }

int glfwUpdateGamepadMappings(const char*) { return GLFW_TRUE; }  // the shell's pad is a gamepad already

int glfwGetGamepadState(int jid, GLFWgamepadstate* state) {
    if (jid != GLFW_JOYSTICK_1 || !state) return GLFW_FALSE;
    memcpy(state->buttons, gPad.buttons, sizeof(state->buttons));
    memcpy(state->axes, gPad.axes, sizeof(state->axes));
    return GLFW_TRUE;
}

}  // extern "C"
