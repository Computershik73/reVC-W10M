// A trial for Windows Phone 8.1: would what this port is made of run there?
//
// The game is built with today's compiler (MSVC v143 for 32-bit ARM) against a C runtime that
// Windows 10 has and Windows Phone 8.1 does not. This app is built the same way, with that C
// runtime linked into it (/MT) and against the 8.1 kit's own import libraries, so that it loads
// there if anything does (w10m/wp81/stubs.cpp has what the kit lacks). It then tries, on the
// phone, what the port does:
//
//   1. the phone: its model, the memory it gives an app, its processors and screen
//   2. the C and C++ runtime: formatting, exceptions (C++ and structured), threads and their
//      locks, the heap, files in LocalState, time, mathematics
//   3. the shell's way of showing a picture: a code-only C++/WinRT XAML app (as w10m/app is)
//      with a SwapChainPanel and a swap chain made for composition, presented from a thread
//      of its own
//   4. w10m/bench/d3dbench.cpp, as it runs on Windows 10 Mobile with REVC_BENCH=1: what this
//      Direct3D has, what a draw costs with one thing changed before it (a matrix in a constant
//      buffer, a matrix from an instance's vertices, new vertices written further along a
//      buffer...), and whether calls can be recorded on one thread for another
//   5. the same with REVC_BENCH=fill: what a layer of pixels and a grid of triangles cost
//
// What it finds goes to the screen (a summary at the top, everything below it), to
// LocalState\trial.log, and through the Share button to whatever the phone shares text with.
#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>
#undef GetCurrentTime  // winbase.h's macro, a name in XAML's animations

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "host_api.h"

#ifndef TRIAL_VERSION
#define TRIAL_VERSION "0"
#endif

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

namespace {

// ===[ The log ]===

const auto gStart = std::chrono::steady_clock::now();
std::mutex gLogLock;
std::string gLogText;         // everything so far: for the file, until it is open, and for sharing
std::wstring gLogPath;        // LocalState\trial.log, once it is known
std::atomic<int> gProblems{0};
bool gRussian = false;

const char* T(const char* english, const char* russian) { return gRussian ? russian : english; }

CoreDispatcher gDispatcher{nullptr};
Controls::TextBlock gSummaryBlock{nullptr}, gLogBlock{nullptr};
Controls::ScrollViewer gScroll{nullptr};
Controls::SwapChainPanel gPanel{nullptr};
std::wstring gScreenLog;  // the UI thread's copy of the log

void Show(std::string const& line);
void ShowSummary();

// The file is opened for each line and closed again: it can be taken off the phone while the
// app runs (ISETool cannot read a file that is held open), and a crash loses nothing.
void AppendToFile(std::string const& line) {
    if (gLogPath.empty()) return;
    if (FILE* f = _wfsopen(gLogPath.c_str(), L"ab", _SH_DENYNO)) {
        fputs(line.c_str(), f);
        fputc('\n', f);
        fclose(f);
    }
}

void Log(std::string const& text) {
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%7.3f ", t);
    std::string line = stamp + text;
    {
        std::lock_guard<std::mutex> lock(gLogLock);
        gLogText += line + "\n";
        AppendToFile(line);
    }
    OutputDebugStringA((line + "\n").c_str());
    Show(text);
}

void Logf(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Log(text);
}

void Problem(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gProblems++;
    Log(std::string("PROBLEM: ") + text);
}

void OpenLogFile(std::wstring const& folder) {
    std::lock_guard<std::mutex> lock(gLogLock);
    if (!gLogPath.empty()) return;
    gLogPath = folder + L"\\trial.log";
    if (FILE* f = _wfsopen(gLogPath.c_str(), L"wb", _SH_DENYNO)) {
        fputs(gLogText.c_str(), f);
        fclose(f);
    }
}

// ===[ What the summary shows ]===

struct Summary {
    std::mutex lock;
    std::string phone, memory, crt, panel, level, deferred, done;
    int step = 0;
    // microseconds a draw, with what the GPU had left to do (the second number of the bench's
    // lines), from its first run: a device as Direct3D makes it
    double plain = -1, matrix = -1, instance = -1, stream = -1, recorded = -1;
    double vsync = -1, free = -1;
} gSummary;

// The bench's lines that the summary takes a number from
void TakeFromBench(const char* text) {
    struct Row {
        const char* prefix;
        double Summary::*value;
    };
    static const Row rows[] = {
        // the bench's second row of it: its first draws pay for what the GPU still had to do
        // (153 microseconds on an HTC One M8 against 5.4 a moment later), and the Lumia 650's
        // 19 beside it is its second row too
        {"d3d bench: nothing changed, again: ", &Summary::plain},
        {"d3d bench: a mat4 for the vertex shader, in a buffer of its own: ", &Summary::matrix},
        {"d3d bench: a mat4 for the vertex shader from a second vertex buffer, one for an instance, further along for "
         "each draw: ",
         &Summary::instance},
        {"d3d bench: new vertices and indices further along each time, nothing overwritten: ", &Summary::stream},
    };
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(gSummary.lock);
        for (auto const& row : rows) {
            size_t n = strlen(row.prefix);
            if (strncmp(text, row.prefix, n) == 0 && gSummary.*row.value < 0) {
                double call = 0, total = 0;
                if (sscanf(text + n, "%lf us a draw, %lf", &call, &total) == 2) {
                    gSummary.*row.value = total;
                    changed = true;
                }
            }
        }
        const char* level = "d3d bench: Direct3D 11 at feature level ";
        if (gSummary.level.empty() && strncmp(text, level, strlen(level)) == 0) {
            std::string rest = text + strlen(level);
            gSummary.level = rest.substr(0, rest.find(','));
            changed = true;
        }
        const char* deferred = "d3d bench: a deferred context: ";
        if (gSummary.deferred.empty() && strncmp(text, deferred, strlen(deferred)) == 0) {
            gSummary.deferred = strncmp(text + strlen(deferred), "yes", 3) == 0 ? T("yes", "да") : T("NO", "НЕТ");
            changed = true;
        }
        const char* recorded = "d3d bench: recorded, nothing changed: ";
        if (gSummary.recorded < 0 && strncmp(text, recorded, strlen(recorded)) == 0) {
            double record = 0;
            if (sscanf(text + strlen(recorded), "%lf", &record) == 1) {
                gSummary.recorded = record;
                changed = true;
            }
        }
    }
    if (changed) ShowSummary();
}

std::string Number(double value) {
    if (value < 0) return "-";
    char text[32];
    snprintf(text, sizeof(text), value < 10 ? "%.1f" : "%.0f", value);
    return text;
}

std::wstring SummaryText() {
    std::lock_guard<std::mutex> lock(gSummary.lock);
    static const char* stepsEn[] = {"starting", "the phone", "the C/C++ runtime", "the picture in the panel",
                                    "the cost of a draw", "the cost of pixels", "done"};
    static const char* stepsRu[] = {"запуск", "телефон", "среда C/C++", "картинка в панели",
                                    "цена вызова отрисовки", "цена пикселей", "готово"};
    std::string s;
    s += T("reVC: a trial for Windows Phone 8.1, build ", "reVC: проверка для Windows Phone 8.1, сборка ");
    s += TRIAL_VERSION;
    s += "\n";
    if (gSummary.done.empty()) {
        char step[160];
        snprintf(step, sizeof(step), T("Step %d of 5: %s. Wait for \"done\".\n", "Шаг %d из 5: %s. Дождитесь «готово».\n"),
                 gSummary.step, gRussian ? stepsRu[gSummary.step] : stepsEn[gSummary.step]);
        s += step;
    } else {
        s += gSummary.done + "\n";
    }
    if (!gSummary.phone.empty()) s += T("Phone: ", "Телефон: ") + gSummary.phone + "\n";
    if (!gSummary.memory.empty()) s += T("Memory for an app: ", "Память для приложения: ") + gSummary.memory + "\n";
    if (!gSummary.crt.empty()) s += T("C/C++ runtime of 2022: ", "Среда C/C++ 2022 года: ") + gSummary.crt + "\n";
    if (!gSummary.panel.empty()) s += T("XAML and the swap chain: ", "XAML и цепочка кадров: ") + gSummary.panel + "\n";
    if (gSummary.vsync >= 0)
        s += T("Frames a second, empty: ", "Кадров в секунду, пустых: ") + Number(gSummary.vsync) +
             T(" with vsync, ", " с синхронизацией, ") + Number(gSummary.free) + T(" without\n", " без неё\n");
    if (!gSummary.level.empty()) {
        s += T("Direct3D 11, level ", "Direct3D 11, уровень ") + gSummary.level;
        if (!gSummary.deferred.empty()) s += T("; deferred context: ", "; отложенный контекст: ") + gSummary.deferred;
        s += "\n";
    }
    if (gSummary.plain >= 0 || gSummary.matrix >= 0) {
        s += T("A draw, microseconds (Lumia 650 on Windows 10 Mobile):\n",
               "Вызов отрисовки, мкс (Lumia 650 на Windows 10 Mobile):\n");
        s += T("  nothing changed: ", "  ничего не меняя: ") + Number(gSummary.plain) + " (19)\n";
        s += T("  a matrix in a constant buffer: ", "  матрица в буфере констант: ") + Number(gSummary.matrix) + " (87)\n";
        s += T("  a matrix with the vertices: ", "  матрица вместе с вершинами: ") + Number(gSummary.instance) + " (32)\n";
        s += T("  vertices further along a buffer: ", "  вершины дальше по буферу: ") + Number(gSummary.stream) + "\n";
        if (gSummary.recorded >= 0)
            s += T("  recorded for another thread: ", "  запись для другого потока: ") + Number(gSummary.recorded) + "\n";
    }
    int problems = gProblems.load();
    if (problems) {
        char line[96];
        snprintf(line, sizeof(line), T("Problems: %d (PROBLEM in the log below)\n", "Проблем: %d (PROBLEM в журнале ниже)\n"),
                 problems);
        s += line;
    }
    return std::wstring(winrt::to_hstring(s));
}

void SetStep(int step) {
    {
        std::lock_guard<std::mutex> lock(gSummary.lock);
        gSummary.step = step;
    }
    ShowSummary();
}

template <class F> void OnUi(F&& f) {
    if (!gDispatcher) return;
    try {
        gDispatcher.RunAsync(CoreDispatcherPriority::Normal, std::forward<F>(f));
    } catch (...) {
    }
}

void ShowSummary() {
    OnUi([] {
        if (gSummaryBlock) gSummaryBlock.Text(SummaryText());
    });
}

void Show(std::string const& line) {
    std::wstring text(winrt::to_hstring(line));
    OnUi([text] {
        if (!gLogBlock) return;
        gScreenLog += text;
        gScreenLog += L"\n";
        gLogBlock.Text(gScreenLog);
    });
}

// ===[ 1. The phone ]===

std::string Narrow(winrt::hstring const& s) { return winrt::to_string(s); }

void ThePhone(double rawPixels, double widthDips, double heightDips) {
    std::string phone;
    try {
        winrt::Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation info;
        phone = Narrow(info.SystemManufacturer()) + " " + Narrow(info.SystemProductName());
        Logf("phone: %s, %s; operating system: %s", phone.c_str(), Narrow(info.FriendlyName()).c_str(),
             Narrow(info.OperatingSystem()).c_str());
        try {
            Logf("phone: firmware %s, hardware %s", Narrow(info.SystemFirmwareVersion()).c_str(),
                 Narrow(info.SystemHardwareVersion()).c_str());
        } catch (winrt::hresult_error const& e) {
            Logf("phone: no firmware version (0x%08X)", (unsigned)e.code());
        }
    } catch (winrt::hresult_error const& e) {
        Problem("the phone's model could not be read, 0x%08X", (unsigned)e.code());
    }

    std::string memory;
    try {
        using winrt::Windows::System::MemoryManager;
        uint64_t limit = MemoryManager::AppMemoryUsageLimit() >> 20, used = MemoryManager::AppMemoryUsage() >> 20;
        char text[96];
        snprintf(text, sizeof(text), T("%u MB (%u in use)", "%u МБ (занято %u)"), (unsigned)limit, (unsigned)used);
        memory = text;
        Logf("memory: an app may have %u MB, this one has %u", (unsigned)limit, (unsigned)used);
    } catch (winrt::hresult_error const& e) {
        Problem("MemoryManager is not there, 0x%08X", (unsigned)e.code());
    }
    MEMORYSTATUSEX status = {sizeof(status)};
    if (GlobalMemoryStatusEx(&status))
        Logf("memory: %u MB in the phone, %u MB of address space", (unsigned)(status.ullTotalPhys >> 20),
             (unsigned)(status.ullTotalVirtual >> 20));

    SYSTEM_INFO system = {};
    GetNativeSystemInfo(&system);
    Logf("processors: %u, architecture %u", (unsigned)system.dwNumberOfProcessors, (unsigned)system.wProcessorArchitecture);
    Logf("screen: %.0f x %.0f view pixels, %.2f physical pixels to one: %.0f x %.0f", widthDips, heightDips, rawPixels,
         widthDips * rawPixels, heightDips * rawPixels);

    std::lock_guard<std::mutex> lock(gSummary.lock);
    gSummary.phone = phone;
    gSummary.memory = memory;
}

// ===[ 2. The C and C++ runtime ]===

thread_local int tPerThread = 0;

int StructuredException() {
    __try {
        RaiseException(0xE0000001, 0, 0, nullptr);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int ReadNothing() {
    __try {
        volatile int* nowhere = nullptr;
        return *nowhere;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

void TheRuntime(std::wstring const& folder) {
    int before = gProblems.load();

    char text[128];
    snprintf(text, sizeof(text), "%.3f %d %s %e %x", 3.14159, 42, "ok", 0.00125, 255u);
    if (strcmp(text, "3.142 42 ok 1.250000e-03 ff") != 0) Problem("snprintf gave \"%s\"", text);
    double parsed = strtod("1e-3", nullptr);
    if (fabs(parsed - 0.001) > 1e-12) Problem("strtod gave %g", parsed);
    double root = sqrt(2.0);
    float sine = sinf(1.0f);
    if (fabs(root - 1.41421356) > 1e-6 || fabsf(sine - 0.841471f) > 1e-5f) Problem("sqrt %f, sinf %f", root, sine);

    try {
        throw std::runtime_error("thrown");
    } catch (std::exception const& e) {
        if (strcmp(e.what(), "thrown") != 0) Problem("a C++ exception came back as \"%s\"", e.what());
    } catch (...) {
        Problem("a C++ exception was caught as something else");
    }
    try {
        std::vector<int> few(3);
        (void)few.at(7);
        Problem("std::vector::at did not throw");
    } catch (std::out_of_range const&) {
    }
    if (StructuredException() != 1) Problem("a structured exception was not caught");
    if (ReadNothing() != -1) Problem("reading address 0 was not caught");

    // threads, a lock, a condition variable, a value of each thread's own
    {
        std::mutex lock;
        long counted = 0;
        std::vector<std::thread> threads;
        std::atomic<int> distinct{0};
        for (int t = 0; t < 4; t++)
            threads.emplace_back([&, t] {
                tPerThread = t + 1;
                for (int i = 0; i < 20000; i++) {
                    std::lock_guard<std::mutex> hold(lock);
                    counted++;
                }
                if (tPerThread == t + 1) distinct++;
            });
        for (auto& thread : threads) thread.join();
        if (counted != 80000 || distinct != 4) Problem("4 threads counted %ld, kept their own value %d of 4", counted, (int)distinct);

        std::condition_variable wake;
        bool asked = false, answered = false;
        std::thread other([&] {
            std::unique_lock<std::mutex> hold(lock);
            wake.wait(hold, [&] { return asked; });
            answered = true;
            wake.notify_all();
        });
        {
            std::unique_lock<std::mutex> hold(lock);
            asked = true;
            wake.notify_all();
            if (!wake.wait_for(hold, std::chrono::seconds(5), [&] { return answered; }))
                Problem("a condition variable did not wake the other thread");
        }
        other.join();
    }

    // the heap: 64 MB in blocks of 1 MB, written, freed
    {
        std::vector<void*> blocks;
        for (int i = 0; i < 64; i++) {
            void* block = malloc(1 << 20);
            if (!block) {
                Problem("malloc gave up after %d MB", i);
                break;
            }
            memset(block, i, 1 << 20);
            blocks.push_back(block);
        }
        for (void* block : blocks) free(block);
        std::unique_ptr<char[]> big(new char[24 << 20]);
        big[(24 << 20) - 1] = 1;
    }

    // a file in LocalState, written and read back
    if (!folder.empty()) {
        std::wstring path = folder + L"\\crt_test.bin";
        std::vector<unsigned char> out(1 << 20), in(1 << 20);
        for (size_t i = 0; i < out.size(); i++) out[i] = (unsigned char)(i * 7);
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            Problem("a file could not be made in LocalState (errno %d)", errno);
        } else {
            size_t written = fwrite(out.data(), 1, out.size(), f);
            fclose(f);
            f = _wfopen(path.c_str(), L"rb");
            size_t read = f ? fread(in.data(), 1, in.size(), f) : 0;
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fclose(f);
                if (size != (long)out.size()) Problem("ftell gave %ld", size);
            }
            if (written != out.size() || read != in.size() || in != out) Problem("the file came back different (%u, %u)", (unsigned)written, (unsigned)read);
            if (_wremove(path.c_str()) != 0) Problem("the file could not be removed (errno %d)", errno);
        }
    }

    // time
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_s(&local, &now);
    char when[64] = "";
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    const char* path = getenv("PATH");
    _putenv_s("REVC_TRIAL", "1");
    const char* set = getenv("REVC_TRIAL");
    if (!set || strcmp(set, "1") != 0) Problem("_putenv_s and getenv");

    int problems = gProblems.load() - before;
    Logf("crt: formatting, exceptions (C++ and structured), threads, a condition variable, 88 MB of heap, "
         "a file in LocalState, time (%s), getenv (PATH %s): %d problems",
         when, path ? "set" : "not set", problems);
    std::lock_guard<std::mutex> lock(gSummary.lock);
    gSummary.crt = problems ? std::string(T("PROBLEMS: ", "ПРОБЛЕМЫ: ")) + std::to_string(problems) : T("works", "работает");
}

// ===[ 3. A swap chain in the SwapChainPanel ]===

template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Picture {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* chain = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    ~Picture() {
        Release(view);
        Release(chain);
        Release(context);
        Release(device);
    }
} gPicture;

bool SetPanelChain(IDXGISwapChain1* chain) {
    auto done = std::make_shared<std::promise<HRESULT>>();
    auto result = done->get_future();
    winrt::com_ptr<IDXGISwapChain1> keep;
    keep.copy_from(chain);
    OnUi([done, keep] {
        HRESULT hr = E_FAIL;
        try {
            hr = gPanel.as<ISwapChainPanelNative>()->SetSwapChain(keep.get());
        } catch (winrt::hresult_error const& e) {
            hr = e.code();
        }
        done->set_value(hr);
    });
    if (result.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        Problem("the UI thread did not set the swap chain in 10 s");
        return false;
    }
    HRESULT hr = result.get();
    if (FAILED(hr)) Problem("the panel did not take the swap chain, 0x%08X", (unsigned)hr);
    return SUCCEEDED(hr);
}

bool Present(UINT interval, float r, float g, float b) {
    float colour[4] = {r, g, b, 1};
    gPicture.context->OMSetRenderTargets(1, &gPicture.view, nullptr);
    gPicture.context->ClearRenderTargetView(gPicture.view, colour);
    HRESULT hr = gPicture.chain->Present(interval, 0);
    if (FAILED(hr)) {
        Problem("Present gave 0x%08X", (unsigned)hr);
        return false;
    }
    return true;
}

double Frames(UINT interval, int count) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < count; i++) {
        float phase = (float)i / 30.0f;
        if (!Present(interval, 0.5f + 0.5f * sinf(phase), 0.5f + 0.5f * sinf(phase + 2.1f), 0.5f + 0.5f * sinf(phase + 4.2f)))
            return -1;
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return count / seconds;
}

void ThePicture(int width, int height) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                               D3D_FEATURE_LEVEL_9_1};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   7, D3D11_SDK_VERSION, &gPicture.device, &level, &gPicture.context);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
                               6, D3D11_SDK_VERSION, &gPicture.device, &level, &gPicture.context);
    if (FAILED(hr)) {
        Problem("no Direct3D 11 device for the panel, 0x%08X", (unsigned)hr);
        return;
    }
    Logf("picture: a device at level %d_%d", (int)(level >> 12), (int)((level >> 8) & 0xF));

    IDXGIDevice1* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(gPicture.device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgi)) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        Logf("picture: the GPU: %s, %u MB of its own, %u MB shared", Narrow(winrt::hstring(desc.Description)).c_str(),
             (unsigned)(desc.DedicatedVideoMemory >> 20), (unsigned)(desc.SharedSystemMemory >> 20));
        adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    }
    if (!factory) {
        Problem("no IDXGIFactory2");
        Release(adapter);
        Release(dxgi);
        return;
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = level < D3D_FEATURE_LEVEL_10_0 ? 3 : 2;  // as w10m/rwd3d11/device.cpp has it
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForComposition(gPicture.device, &sd, nullptr, &gPicture.chain);
    if (FAILED(hr) && sd.BufferCount == 3) {
        Logf("picture: no swap chain of 3 buffers (0x%08X), trying 2", (unsigned)hr);
        sd.BufferCount = 2;
        hr = factory->CreateSwapChainForComposition(gPicture.device, &sd, nullptr, &gPicture.chain);
    }
    Release(factory);
    Release(adapter);
    if (FAILED(hr)) {
        Release(dxgi);
        Problem("CreateSwapChainForComposition gave 0x%08X", (unsigned)hr);
        return;
    }
    Logf("picture: a swap chain of %d x %d, %u buffers, for composition", width, height, (unsigned)sd.BufferCount);
    ID3D11Texture2D* buffer = nullptr;
    hr = gPicture.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&buffer);
    if (SUCCEEDED(hr)) hr = gPicture.device->CreateRenderTargetView(buffer, nullptr, &gPicture.view);
    Release(buffer);
    if (FAILED(hr)) {
        Release(dxgi);
        Problem("no view of the swap chain's buffer, 0x%08X", (unsigned)hr);
        return;
    }
    if (!SetPanelChain(gPicture.chain)) {
        Release(dxgi);
        return;
    }

    double vsync = Frames(1, 120);
    double free = Frames(0, 120);
    if (dxgi) dxgi->SetMaximumFrameLatency(1);
    double latency1 = Frames(0, 120);
    Release(dxgi);
    Logf("picture: frames a second, nothing but a cleared picture: %.1f with vsync, %.1f without, %.1f without and one "
         "frame queued",
         vsync, free, latency1);
    std::lock_guard<std::mutex> lock(gSummary.lock);
    gSummary.vsync = vsync;
    gSummary.free = free;
    gSummary.panel = vsync > 0 ? T("works", "работает") : T("PROBLEM", "ПРОБЛЕМА");
}

// ===[ The order of it all, on a thread of its own ]===

void Trial(std::wstring folder, double rawPixels, double widthDips, double heightDips, int panelWidth, int panelHeight) {
    Logf("trial %s: Windows Phone 8.1 trial of reVC's port, built with MSVC %d", TRIAL_VERSION, _MSC_FULL_VER);
    SetStep(1);
    ThePhone(rawPixels, widthDips, heightDips);
    SetStep(2);
    TheRuntime(folder);
    SetStep(3);
    ThePicture(panelWidth, panelHeight);
    if (gPicture.chain) Present(1, 0.1f, 0.1f, 0.3f);
    SetStep(4);
    _putenv_s("REVC_BENCH", "1");
    W10M_Direct3DBenchmark();
    SetStep(5);
    _putenv_s("REVC_BENCH", "fill");
    W10M_Direct3DBenchmark();
    try {
        uint64_t used = winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20;
        Logf("memory: this app has %u MB at the end", (unsigned)used);
    } catch (...) {
    }
    int problems = gProblems.load();
    if (gPicture.chain) {
        if (problems)
            Present(1, 0.5f, 0.05f, 0.05f);
        else
            Present(1, 0.05f, 0.4f, 0.1f);
    }
    {
        std::lock_guard<std::mutex> lock(gSummary.lock);
        gSummary.step = 6;
        char done[160];
        snprintf(done, sizeof(done),
                 problems ? T("Done: %d problems. Share sends the whole log.", "Готово: проблем %d. «Отправить» отдаёт весь журнал.")
                          : T("Done, no problems. Share sends the whole log.", "Готово, проблем нет. «Отправить» отдаёт весь журнал."),
                 problems);
        gSummary.done = done;
    }
    Logf("trial: done, %d problems", problems);
    ShowSummary();
}

// ===[ Crashes: what and where, for w10m/work/wp81's map ]===

LONG WINAPI LastChance(EXCEPTION_POINTERS* info) {
    HMODULE self = GetModuleHandleW(nullptr);
    uintptr_t at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    char text[160];
    snprintf(text, sizeof(text), "CRASH: exception 0x%08X at reVC81Trial.exe+0x%X", (unsigned)info->ExceptionRecord->ExceptionCode,
             (unsigned)(at - (uintptr_t)self));
    // not waiting for the lock: the crash may be of the thread that holds it
    if (gLogLock.try_lock()) {
        AppendToFile(text);
        gLogLock.unlock();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ===[ The app: a code-only XAML application, as w10m/app/App.cpp is ]===

Media::SolidColorBrush Brush(uint8_t r, uint8_t g, uint8_t b) {
    return Media::SolidColorBrush(winrt::Windows::UI::Color{255, r, g, b});
}

struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    // the framework asks the Application for this during start-up; there are no types of ours
    Markup::IXamlType GetXamlType(Interop::TypeName const&) { return nullptr; }
    Markup::IXamlType GetXamlType(winrt::hstring const&) { return nullptr; }
    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return {}; }

    void OnLaunched(winrt::Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
        auto window = Window::Current();
        if (window.Content()) {
            window.Activate();
            return;
        }
        try {
            auto first = winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages().GetAt(0);
            gRussian = first == L"ru" || (first.size() > 3 && wcsncmp(first.c_str(), L"ru-", 3) == 0);
        } catch (...) {
        }
        gDispatcher = window.Dispatcher();
        // the log's file first: whatever goes wrong after this is in it
        std::wstring folder;
        try {
            folder = std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
            OpenLogFile(folder);
        } catch (winrt::hresult_error const& e) {
            Problem("no LocalState, 0x%08X", (unsigned)e.code());
        }
        Log("xaml: launched");
        try {
            Build(window, folder);
        } catch (winrt::hresult_error const& e) {
            Problem("the window could not be made, 0x%08X: %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
            window.Activate();
        } catch (std::exception const& e) {
            Problem("the window could not be made: %s", e.what());
            window.Activate();
        }
    }

    void Build(Window const& window, std::wstring const& folder) {
        auto bounds = window.Bounds();
        double rawPixels = 1;
        try {
            auto display = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            try {
                rawPixels = display.RawPixelsPerViewPixel();
            } catch (...) {
                rawPixels = (double)(int)display.ResolutionScale() / 100.0;
            }
        } catch (...) {
        }
        if (rawPixels <= 0) rawPixels = 1;

        Controls::Grid top;
        top.Background(Brush(0, 0, 0));
        auto rows = top.RowDefinitions();
        double panelDips = std::floor(bounds.Height / 5);
        for (GridLength height : {GridLength{1, GridUnitType::Auto}, GridLength{panelDips, GridUnitType::Pixel},
                                  GridLength{1, GridUnitType::Star}, GridLength{1, GridUnitType::Auto}}) {
            Controls::RowDefinition row;
            row.Height(height);
            rows.Append(row);
        }
        gSummaryBlock = Controls::TextBlock();
        gSummaryBlock.FontSize(16);
        gSummaryBlock.TextWrapping(TextWrapping::Wrap);
        gSummaryBlock.Foreground(Brush(255, 255, 255));
        gSummaryBlock.Margin(Thickness{12, 30, 12, 8});
        gSummaryBlock.Text(SummaryText());
        top.Children().Append(gSummaryBlock);

        gPanel = Controls::SwapChainPanel();
        Controls::Grid::SetRow(gPanel, 1);
        top.Children().Append(gPanel);

        gLogBlock = Controls::TextBlock();
        gLogBlock.FontSize(12);
        gLogBlock.TextWrapping(TextWrapping::Wrap);
        gLogBlock.Foreground(Brush(190, 190, 190));
        gLogBlock.Margin(Thickness{12, 8, 12, 8});
        gScroll = Controls::ScrollViewer();
        gScroll.Content(gLogBlock);
        Controls::Grid::SetRow(gScroll, 2);
        top.Children().Append(gScroll);

        Controls::Button share;
        share.Content(winrt::box_value(winrt::to_hstring(std::string(T("Share the log", "Отправить журнал")))));
        share.HorizontalAlignment(HorizontalAlignment::Stretch);
        share.Margin(Thickness{12, 0, 12, 12});
        share.Foreground(Brush(255, 255, 255));
        share.BorderBrush(Brush(255, 255, 255));
        share.Click([](auto&&, auto&&) {
            try {
                winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI();
            } catch (winrt::hresult_error const& e) {
                Problem("the share screen could not be shown, 0x%08X", (unsigned)e.code());
            }
        });
        Controls::Grid::SetRow(share, 3);
        top.Children().Append(share);

        window.Content(top);
        window.Activate();

        try {
            using namespace winrt::Windows::ApplicationModel::DataTransfer;
            DataTransferManager::GetForCurrentView().DataRequested([](DataTransferManager const&, DataRequestedEventArgs const& args) {
                std::string text;
                {
                    std::lock_guard<std::mutex> lock(gLogLock);
                    text = gLogText;
                }
                auto data = args.Request().Data();
                data.Properties().Title(winrt::to_hstring(std::string("reVC: Windows Phone 8.1 trial ") + TRIAL_VERSION));
                data.SetText(winrt::hstring(SummaryText() + L"\n" + std::wstring(winrt::to_hstring(text))));
            });
        } catch (winrt::hresult_error const& e) {
            Problem("no share for this app, 0x%08X", (unsigned)e.code());
        }
        try {
            winrt::Windows::UI::ViewManagement::StatusBar::GetForCurrentView().HideAsync();
        } catch (winrt::hresult_error const& e) {
            Logf("the status bar stays (0x%08X)", (unsigned)e.code());
        }
        Log("xaml: the window is up");

        int panelWidth = (int)std::lround(bounds.Width * rawPixels), panelHeight = (int)std::lround(panelDips * rawPixels);
        if (panelWidth < 8) panelWidth = 8;
        if (panelHeight < 8) panelHeight = 8;
        std::thread(Trial, folder, rawPixels, (double)bounds.Width, (double)bounds.Height, panelWidth, panelHeight).detach();
    }
};

}  // namespace

// ===[ What w10m/bench/d3dbench.cpp needs ]===

extern "C" void host_log(const char* text) {
    Log(text);
    TakeFromBench(text);
}

extern "C" void host_logf(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    host_log(text);
}

// The bench compiles its shaders. d3dcompiler_47.dll is looked for when it is first asked for,
// not when the app loads: if a phone has none, this app still runs and says so.
typedef HRESULT(WINAPI* CompileFunction)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                         UINT, UINT, ID3DBlob**, ID3DBlob**);
extern "C" HRESULT WINAPI D3DCompile(LPCVOID source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    static CompileFunction compile = [] {
        HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, 0);
        CompileFunction found = module ? (CompileFunction)GetProcAddress(module, "D3DCompile") : nullptr;
        if (!found) Problem("no d3dcompiler_47.dll with D3DCompile (error %u)", (unsigned)GetLastError());
        return found;
    }();
    if (errors) *errors = nullptr;
    if (code) *code = nullptr;
    return compile ? compile(source, size, name, defines, include, entry, target, flags1, flags2, code, errors) : E_NOTIMPL;
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(LastChance);
    // The entry thread is MTA; Application::Start makes the ASTA UI thread itself
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Application::Start([](auto&&) { winrt::make<App>(); });
    return 0;
}
