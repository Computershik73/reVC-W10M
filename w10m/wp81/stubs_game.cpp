// Windows Phone 8.1: what the game, OpenAL Soft, the C runtime and C++/WinRT call beyond what
// stubs.cpp covers for the trial and the game alike. The list is the game's link against the
// 8.1 kit alone (build_game.py): the current directory, semaphores by their old names,
// LocalAlloc, anonymous pipes, CopyFileW, AreFileApisANSI, and the thread pool C++/WinRT's
// resume_background hands work to.
//
// Where the kit has no import library entry the phone may still have the function: Windows
// 8.1's api-sets lead to kernelbase.dll, and the phone's is Windows' own. So a stub looks there
// (and in kernel32.dll) first, and does it itself only where the phone has nothing. The current
// directory falls back on ntdll.dll's own (RtlSetCurrentDirectory_U): the game opens its files
// by paths relative to it. What was found where goes into the log (wp81_system_report).
#include <windows.h>

#include <new>
#include <stdio.h>
#include <string.h>

namespace {

enum Lookup { kSetDirectory, kGetDirectory, kCreatePipe, kPeekPipe, kThreadPool, kLookups };
const char* const kNames[kLookups] = {"SetCurrentDirectoryW", "GetCurrentDirectoryW", "CreatePipe", "PeekNamedPipe",
                                      "TrySubmitThreadpoolCallback"};
const char* gWhere[kLookups] = {};  // where each was found, once asked for

FARPROC FromSystem(Lookup which) {
    static const wchar_t* const modules[] = {L"kernelbase.dll", L"kernel32.dll"};
    static const char* const names[] = {"kernelbase.dll", "kernel32.dll"};
    for (int i = 0; i < 2; i++) {
        HMODULE module = GetModuleHandleW(modules[i]);
        FARPROC found = module ? GetProcAddress(module, kNames[which]) : nullptr;
        if (found) {
            gWhere[which] = names[i];
            return found;
        }
    }
    gWhere[which] = "none";
    return nullptr;
}

FARPROC FromNtdll(Lookup which, const char* name) {
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    FARPROC found = module ? GetProcAddress(module, name) : nullptr;
    gWhere[which] = found ? (which == kSetDirectory ? "ntdll.dll (RtlSetCurrentDirectory_U)"
                                                    : "ntdll.dll (RtlGetCurrentDirectory_U)")
                          : "none";
    return found;
}

// ===[ The current directory ]===

struct CountedString {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
};
typedef BOOL(WINAPI* SetDirectoryW)(LPCWSTR);
typedef DWORD(WINAPI* GetDirectoryW)(DWORD, LPWSTR);
typedef LONG(NTAPI* RtlSetDirectory)(CountedString*);
typedef ULONG(NTAPI* RtlGetDirectory)(ULONG, PWSTR);

BOOL WINAPI SetCurrentDirectoryWStub(LPCWSTR path) {
    static SetDirectoryW real = reinterpret_cast<SetDirectoryW>(FromSystem(kSetDirectory));
    if (real) return real(path);
    static RtlSetDirectory rtl = reinterpret_cast<RtlSetDirectory>(FromNtdll(kSetDirectory, "RtlSetCurrentDirectory_U"));
    if (rtl && path) {
        size_t length = wcslen(path);
        CountedString s{static_cast<USHORT>(length * sizeof(wchar_t)), static_cast<USHORT>((length + 1) * sizeof(wchar_t)),
                        const_cast<PWSTR>(path)};
        if (rtl(&s) >= 0) return TRUE;
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

// As GetCurrentDirectoryW counts: the characters written without the end, or the size needed
// with it. RtlGetCurrentDirectory_U counts the same in bytes.
DWORD WINAPI GetCurrentDirectoryWStub(DWORD size, LPWSTR buffer) {
    static GetDirectoryW real = reinterpret_cast<GetDirectoryW>(FromSystem(kGetDirectory));
    if (real) return real(size, buffer);
    static RtlGetDirectory rtl = reinterpret_cast<RtlGetDirectory>(FromNtdll(kGetDirectory, "RtlGetCurrentDirectory_U"));
    if (rtl) return rtl(size * sizeof(wchar_t), buffer) / sizeof(wchar_t);
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

BOOL WINAPI SetCurrentDirectoryAStub(LPCSTR path) {
    wchar_t wide[MAX_PATH * 2];
    if (!path || !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, ARRAYSIZE(wide))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return SetCurrentDirectoryWStub(wide);
}

DWORD WINAPI GetCurrentDirectoryAStub(DWORD size, LPSTR buffer) {
    wchar_t wide[MAX_PATH * 2];
    DWORD n = GetCurrentDirectoryWStub(ARRAYSIZE(wide), wide);
    if (!n || n >= ARRAYSIZE(wide)) return 0;
    int needed = WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return 0;
    if (!buffer || size < static_cast<DWORD>(needed)) return static_cast<DWORD>(needed);
    WideCharToMultiByte(CP_ACP, 0, wide, -1, buffer, static_cast<int>(size), nullptr, nullptr);
    return static_cast<DWORD>(needed - 1);
}

// ===[ Pipes: the shell reads what the game prints through one (App.cpp, CaptureOutput) ]===

typedef BOOL(WINAPI* CreatePipeFunction)(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
typedef BOOL(WINAPI* PeekPipeFunction)(HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD);

BOOL WINAPI CreatePipeStub(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES attributes, DWORD size) {
    static CreatePipeFunction real = reinterpret_cast<CreatePipeFunction>(FromSystem(kCreatePipe));
    if (real) return real(read, write, attributes, size);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI PeekNamedPipeStub(HANDLE pipe, LPVOID buffer, DWORD size, LPDWORD read, LPDWORD available, LPDWORD left) {
    static PeekPipeFunction real = reinterpret_cast<PeekPipeFunction>(FromSystem(kPeekPipe));
    if (real) return real(pipe, buffer, size, read, available, left);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

// ===[ What 8.1 has under another name ]===

HANDLE WINAPI CreateSemaphoreWStub(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name) {
    return CreateSemaphoreExW(attributes, initial, maximum, name, 0, SEMAPHORE_ALL_ACCESS);
}

HANDLE WINAPI CreateSemaphoreAStub(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCSTR name) {
    wchar_t wide[MAX_PATH];
    if (name && !MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return CreateSemaphoreExW(attributes, initial, maximum, name ? wide : nullptr, 0, SEMAPHORE_ALL_ACCESS);
}

// LMEM_FIXED memory is a block of the process's heap (stubs.cpp frees it so)
HLOCAL WINAPI LocalAllocStub(UINT flags, SIZE_T bytes) {
    return HeapAlloc(GetProcessHeap(), (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, bytes ? bytes : 1);
}

BOOL WINAPI AreFileApisANSIStub() { return TRUE; }

BOOL WINAPI CopyFileWStub(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    COPYFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwCopyFlags = failIfExists ? COPY_FILE_FAIL_IF_EXISTS : 0;
    HRESULT hr = CopyFile2(from, to, &parameters);
    if (SUCCEEDED(hr)) return TRUE;
    SetLastError(HRESULT_CODE(hr));
    return FALSE;
}

}  // namespace

#if defined(_M_IX86)
#define IMPORT_STUB(name, bytes)                          \
    extern "C" void* StubPtr_##name = (void*)&name##Stub; \
    __pragma(comment(linker, "/alternatename:__imp__" #name "@" #bytes "=_StubPtr_" #name))
#else
#define IMPORT_STUB(name, bytes) extern "C" void* __imp_##name = (void*)&name##Stub;
#endif

IMPORT_STUB(SetCurrentDirectoryW, 4)
IMPORT_STUB(GetCurrentDirectoryW, 8)
IMPORT_STUB(SetCurrentDirectoryA, 4)
IMPORT_STUB(GetCurrentDirectoryA, 8)
IMPORT_STUB(CreatePipe, 16)
IMPORT_STUB(PeekNamedPipe, 24)
IMPORT_STUB(CreateSemaphoreW, 16)
IMPORT_STUB(CreateSemaphoreA, 16)
IMPORT_STUB(LocalAlloc, 8)
IMPORT_STUB(AreFileApisANSI, 0)
IMPORT_STUB(CopyFileW, 12)

extern "C" {

// C++/WinRT's resume_background (the files screen's copy) hands work to the thread pool. The
// kit has no TrySubmitThreadpoolCallback; where the phone has none either, each piece of work
// gets a thread of its own.
int32_t __stdcall WINRT_IMPL_TrySubmitThreadpoolCallback(void(__stdcall* callback)(void*, void*), void* context,
                                                          void* environment) noexcept {
    typedef BOOL(WINAPI * Submit)(PTP_SIMPLE_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON);
    static Submit real = reinterpret_cast<Submit>(FromSystem(kThreadPool));
    if (real)
        return real(reinterpret_cast<PTP_SIMPLE_CALLBACK>(callback), context, static_cast<PTP_CALLBACK_ENVIRON>(environment));
    struct Work {
        void(__stdcall* callback)(void*, void*);
        void* context;
    };
    Work* work = new (std::nothrow) Work{callback, context};
    if (!work) return 0;
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID parameter) -> DWORD {
            Work w = *static_cast<Work*>(parameter);
            delete static_cast<Work*>(parameter);
            w.callback(nullptr, w.context);
            return 0;
        },
        work, 0, nullptr);
    if (!thread) {
        delete work;
        return 0;
    }
    CloseHandle(thread);
    return 1;
}

// For the log: where each of the functions the phone may have came from
void wp81_system_report(char* text, size_t size) {
    wchar_t directory[MAX_PATH];
    GetCurrentDirectoryWStub(ARRAYSIZE(directory), directory);
    if (!gWhere[kSetDirectory]) {
        // asked without changing anything: where SetCurrentDirectoryW would come from
        if (!FromSystem(kSetDirectory)) FromNtdll(kSetDirectory, "RtlSetCurrentDirectory_U");
    }
    if (!gWhere[kCreatePipe]) FromSystem(kCreatePipe);
    if (!gWhere[kPeekPipe]) FromSystem(kPeekPipe);
    if (!gWhere[kThreadPool]) FromSystem(kThreadPool);
    size_t used = 0;
    for (int i = 0; i < kLookups && used < size; i++) {
        int n = snprintf(text + used, size - used, "%s%s: %s", i ? "; " : "", kNames[i], gWhere[i] ? gWhere[i] : "?");
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
}

}  // extern "C"
