// What the trial's code and its C runtime call that the Windows Phone 8.1 kit's import
// libraries do not have: the list is the link's unresolved externals, against nothing but the
// kit (build.py). The C runtime asks for a console, a command line and an environment block
// that an 8.1 app has none of; C++/WinRT for LoadLibraryW, which 8.1 gives an app as
// LoadLibraryExW.
//
// A call through a dllimport declaration reads the address from __imp_<name>; defining that
// pointer here, to a function of our own, takes the place of an import.
#include <windows.h>
#include <roapi.h>

#include <wchar.h>

namespace {

HLOCAL WINAPI LocalFreeStub(HLOCAL memory) {
    // what FormatMessage's FORMAT_MESSAGE_ALLOCATE_BUFFER gave: LMEM_FIXED, a block of the
    // process's heap
    if (memory && !HeapFree(GetProcessHeap(), 0, memory)) return memory;
    return nullptr;
}

HANDLE WINAPI GetStdHandleStub(DWORD) {
    SetLastError(ERROR_INVALID_HANDLE);
    return nullptr;  // no console: what the C runtime takes for "none"
}
BOOL WINAPI SetStdHandleStub(DWORD, HANDLE) { return TRUE; }
BOOL WINAPI SetConsoleCtrlHandlerStub(PHANDLER_ROUTINE, BOOL) { return TRUE; }
UINT WINAPI GetConsoleCPStub() { return CP_UTF8; }
BOOL WINAPI GetConsoleModeStub(HANDLE, LPDWORD mode) {
    if (mode) *mode = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;  // not a console
}
BOOL WINAPI ReadConsoleWStub(HANDLE, LPVOID, DWORD, LPDWORD read, PCONSOLE_READCONSOLE_CONTROL) {
    if (read) *read = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
BOOL WINAPI WriteConsoleWStub(HANDLE, const VOID*, DWORD, LPDWORD written, LPVOID) {
    if (written) *written = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

char gCommandLineA[] = "";
wchar_t gCommandLineW[] = L"";
LPSTR WINAPI GetCommandLineAStub() { return gCommandLineA; }
LPWSTR WINAPI GetCommandLineWStub() { return gCommandLineW; }

// an empty environment block: no strings, then the end
wchar_t gEnvironment[2] = {0, 0};
LPWCH WINAPI GetEnvironmentStringsWStub() { return gEnvironment; }
BOOL WINAPI FreeEnvironmentStringsWStub(LPWCH) { return TRUE; }
// the C runtime keeps its own copy of what _putenv sets; nothing outside the app reads it
BOOL WINAPI SetEnvironmentVariableAStub(LPCSTR, LPCSTR) { return TRUE; }
BOOL WINAPI SetEnvironmentVariableWStub(LPCWSTR, LPCWSTR) { return TRUE; }

}  // namespace

// ARM: the pointer under the import's own name. x86 (for the emulator): the import's name is
// decorated with the bytes of its arguments, which is no name in C++: the linker is told to
// take our pointer for it.
#if defined(_M_IX86)
#define IMPORT_STUB(name, bytes)                              extern "C" void* StubPtr_##name = (void*)&name##Stub;     __pragma(comment(linker, "/alternatename:__imp__" #name "@" #bytes "=_StubPtr_" #name))
#else
#define IMPORT_STUB(name, bytes) extern "C" void* __imp_##name = (void*)&name##Stub;
#endif

IMPORT_STUB(LocalFree, 4)
IMPORT_STUB(GetStdHandle, 4)
IMPORT_STUB(SetStdHandle, 8)
IMPORT_STUB(SetConsoleCtrlHandler, 8)
IMPORT_STUB(GetConsoleCP, 0)
IMPORT_STUB(GetConsoleMode, 8)
IMPORT_STUB(ReadConsoleW, 20)
IMPORT_STUB(WriteConsoleW, 20)
IMPORT_STUB(GetCommandLineA, 0)
IMPORT_STUB(GetCommandLineW, 0)
IMPORT_STUB(GetEnvironmentStringsW, 0)
IMPORT_STUB(FreeEnvironmentStringsW, 4)
IMPORT_STUB(SetEnvironmentVariableA, 8)
IMPORT_STUB(SetEnvironmentVariableW, 8)

#if defined(_M_IX86)
// x86's C++ library (winapisupp.obj, which comes in for other things) has wrappers of the
// thread pool's timers and waits that the ARM one has not: nothing here calls them, and the
// 8.1 kit has none of them. They fail.
namespace {
VOID WINAPI FreeLibraryWhenCallbackReturnsStub(PTP_CALLBACK_INSTANCE, HMODULE) {}
PTP_TIMER WINAPI CreateThreadpoolTimerStub(PTP_TIMER_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
}
VOID WINAPI SetThreadpoolTimerStub(PTP_TIMER, PFILETIME, DWORD, DWORD) {}
VOID WINAPI WaitForThreadpoolTimerCallbacksStub(PTP_TIMER, BOOL) {}
VOID WINAPI CloseThreadpoolTimerStub(PTP_TIMER) {}
PTP_WAIT WINAPI CreateThreadpoolWaitStub(PTP_WAIT_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
}
VOID WINAPI SetThreadpoolWaitStub(PTP_WAIT, HANDLE, PFILETIME) {}
VOID WINAPI CloseThreadpoolWaitStub(PTP_WAIT) {}
}  // namespace
IMPORT_STUB(FreeLibraryWhenCallbackReturns, 8)
IMPORT_STUB(CreateThreadpoolTimer, 12)
IMPORT_STUB(SetThreadpoolTimer, 16)
IMPORT_STUB(WaitForThreadpoolTimerCallbacks, 8)
IMPORT_STUB(CloseThreadpoolTimer, 4)
IMPORT_STUB(CreateThreadpoolWait, 12)
IMPORT_STUB(SetThreadpoolWait, 12)
IMPORT_STUB(CloseThreadpoolWait, 4)
#endif

extern "C" {

// C++/WinRT (base.h) finds RoGetActivationFactory, RoOriginateLanguageException and
// CoIncrementMTAUsage in combase.dll when it first needs them, and a component's DLL by name
// for a class the system does not know. The 8.1 kit has LoadLibraryExW (no LoadPackagedLibrary).
// For combase it is given the module that this program's own import of RoGetActivationFactory
// (through the kit's api-set) was bound to: that one is there, whatever an app container may
// load by name.
void* __stdcall WINRT_IMPL_LoadLibraryW(wchar_t const* name) noexcept {
    if (name && _wcsicmp(name, L"combase.dll") == 0) {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&RoGetActivationFactory),
                               &module))
            return module;
    }
    return LoadLibraryExW(name, nullptr, 0);
}

}  // extern "C"
