// Windows Phone 8.1: D3DCompile for the bench (w10m/bench/d3dbench.cpp, REVC_BENCH), the only
// thing in the game that compiles a shader (the renderer's are compiled at build time).
// d3dcompiler_47.dll is looked for when the bench first asks for it, not when the app loads: a
// phone without it still plays (an HTC One M8 with 8.1 has it: the trial's bench ran there).
#include <windows.h>

#include <d3dcompiler.h>

#include "host_api.h"

typedef HRESULT(WINAPI* CompileFunction)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                         UINT, UINT, ID3DBlob**, ID3DBlob**);

extern "C" HRESULT WINAPI D3DCompile(LPCVOID source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    static CompileFunction compile = [] {
        HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, 0);
        CompileFunction found = module ? reinterpret_cast<CompileFunction>(GetProcAddress(module, "D3DCompile")) : nullptr;
        if (!found) host_logf("wp81: no d3dcompiler_47.dll with D3DCompile (error %lu)", GetLastError());
        return found;
    }();
    if (errors) *errors = nullptr;
    if (code) *code = nullptr;
    return compile ? compile(source, size, name, defines, include, entry, target, flags1, flags2, code, errors) : E_NOTIMPL;
}
