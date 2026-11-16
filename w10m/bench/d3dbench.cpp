// What a draw costs through Direct3D 11 itself (REVC_BENCH=1), next to what it costs through
// ANGLE (glbench.cpp): the same small quad, the same number of times, one thing changed
// between the draws at a time. On a device of its own, into a texture: nothing of the game's
// picture is touched. What the two tables differ by is what a renderer that calls Direct3D
// itself could save over one that calls OpenGL ES; what they have in common is the phone's
// driver, and stays.
//
// It also says what this phone's Direct3D has: the formats a renderer would ask for, and
// whether the calls of a frame can be recorded on one thread and made on another.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host_api.h"

namespace {

template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

double Now() {  // microseconds
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)frequency.QuadPart;
}

const char kVertexSplit[] =
    "cbuffer M : register(b0) { float4x4 u_m; };\n"
    "cbuffer Big : register(b1) { float4 u_big[64]; };\n"
    "struct VI { float3 pos : POSITION; float2 tex : TEXCOORD0; };\n"
    "struct VO { float4 pos : SV_Position; float2 tex : TEXCOORD0; };\n"
    "VO main(VI i) { VO o; o.tex = i.tex;\n"
    "  o.pos = mul(u_m, float4(i.pos.xy, 0.0, 1.0)) + u_big[(int)i.pos.z]*0.000001; return o; }\n";
// the same with the matrix and the 64 vectors in one buffer: what it costs to hand over a
// big buffer for a small change
const char kVertexOne[] =
    "cbuffer All : register(b0) { float4x4 u_m; float4 u_big[64]; };\n"
    "struct VI { float3 pos : POSITION; float2 tex : TEXCOORD0; };\n"
    "struct VO { float4 pos : SV_Position; float2 tex : TEXCOORD0; };\n"
    "VO main(VI i) { VO o; o.tex = i.tex;\n"
    "  o.pos = mul(u_m, float4(i.pos.xy, 0.0, 1.0)) + u_big[(int)i.pos.z]*0.000001; return o; }\n";
const char kPixel[] =
    "cbuffer C : register(b0) { float4 u_col; };\n"
    "Texture2D tex0 : register(t0);\n"
    "SamplerState smp0 : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 tex : TEXCOORD0) : SV_Target { return tex0.Sample(smp0, tex)*u_col; }\n";

ID3DBlob* Compile(const char* source, size_t length, const char* target) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source, length, nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    if (FAILED(hr)) {
        host_logf("d3d bench: a shader (%s) did not compile, 0x%08X: %s", target, (unsigned)hr,
                  errors ? (const char*)errors->GetBufferPointer() : "");
        Release(code);
    }
    Release(errors);
    return code;
}

struct Bench {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D *target = nullptr, *depth = nullptr;
    ID3D11RenderTargetView* targetView = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    ID3D11VertexShader *vertex[2] = {}, *vertexOne = nullptr;
    ID3D11PixelShader* pixel[2] = {};
    ID3D11InputLayout *layout = nullptr, *layoutOne = nullptr;
    ID3D11Buffer *vertices[2] = {}, *indices[2] = {}, *streamVertices = nullptr, *streamIndices = nullptr;
    ID3D11Buffer *matrix = nullptr, *matrixDefault = nullptr, *big = nullptr, *all = nullptr, *colour = nullptr;
    ID3D11Texture2D* textures[2] = {};
    ID3D11ShaderResourceView* textureViews[2] = {};
    ID3D11SamplerState* samplers[2] = {};
    ID3D11BlendState* blends[3] = {};
    ID3D11DepthStencilState* depths[4] = {};
    ID3D11RasterizerState* culls[3] = {};
    ID3D11Query* done = nullptr;

    ~Bench() {
        if (context) context->ClearState();
        Release(done);
        for (auto& p : culls) Release(p);
        for (auto& p : depths) Release(p);
        for (auto& p : blends) Release(p);
        for (auto& p : samplers) Release(p);
        for (auto& p : textureViews) Release(p);
        for (auto& p : textures) Release(p);
        Release(colour);
        Release(all);
        Release(big);
        Release(matrixDefault);
        Release(matrix);
        Release(streamIndices);
        Release(streamVertices);
        for (auto& p : indices) Release(p);
        for (auto& p : vertices) Release(p);
        Release(layoutOne);
        Release(layout);
        for (auto& p : pixel) Release(p);
        Release(vertexOne);
        for (auto& p : vertex) Release(p);
        Release(depthView);
        Release(targetView);
        Release(depth);
        Release(target);
        Release(context);
        Release(device);
    }

    // everything the driver has been handed is done
    void Finish() {
        context->End(done);
        context->Flush();
        BOOL finished = FALSE;
        double t0 = Now();
        while (context->GetData(done, &finished, sizeof(finished), 0) != S_OK || !finished) {
            if (Now() - t0 > 5e6) break;
            Sleep(0);
        }
    }

    bool Write(ID3D11Buffer* buffer, const void* data, size_t size, D3D11_MAP how = D3D11_MAP_WRITE_DISCARD,
               size_t at = 0) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(buffer, 0, how, 0, &mapped))) return false;
        memcpy((char*)mapped.pData + at, data, size);
        context->Unmap(buffer, 0);
        return true;
    }
};

const float kQuad[4 * 5] = {
    -0.99f, -0.99f, 0.0f, 0.0f, 0.0f, -0.98f, -0.99f, 1.0f, 1.0f, 0.0f,
    -0.98f, -0.98f, 2.0f, 1.0f, 1.0f, -0.99f, -0.98f, 3.0f, 0.0f, 1.0f,
};
const unsigned short kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

const char* LevelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "11_1 or more";
    }
}

bool Make(Bench& b, UINT flags, D3D_FEATURE_LEVEL* got) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    // the cap the tests on a PC put on ANGLE: the same level here
    int first = 0;
    if (const char* cap = getenv("REVC_ANGLE_FL")) {
        if (strcmp(cap, "10_0") == 0) first = 2;
        if (strcmp(cap, "9_3") == 0) first = 3;
        if (strcmp(cap, "9_1") == 0) first = 5;
    }
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + first, 6 - first,
                                   D3D11_SDK_VERSION, &b.device, got, &b.context);
    if (FAILED(hr)) {
        host_logf("d3d bench: no device, 0x%08X", (unsigned)hr);
        return false;
    }
    const char* vertexTarget = *got >= D3D_FEATURE_LEVEL_10_0 ? "vs_4_0" : "vs_4_0_level_9_3";
    const char* pixelTarget = *got >= D3D_FEATURE_LEVEL_10_0 ? "ps_4_0" : "ps_4_0_level_9_3";
    if (*got < D3D_FEATURE_LEVEL_9_3) {
        vertexTarget = "vs_4_0_level_9_1";
        pixelTarget = "ps_4_0_level_9_1";
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 768;
    td.Height = 432;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(b.device->CreateTexture2D(&td, nullptr, &b.target)) ||
        FAILED(b.device->CreateRenderTargetView(b.target, nullptr, &b.targetView)))
        return false;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(b.device->CreateTexture2D(&td, nullptr, &b.depth)) ||
        FAILED(b.device->CreateDepthStencilView(b.depth, nullptr, &b.depthView)))
        return false;

    ID3DBlob* vs = Compile(kVertexSplit, sizeof(kVertexSplit) - 1, vertexTarget);
    ID3DBlob* vsOne = Compile(kVertexOne, sizeof(kVertexOne) - 1, vertexTarget);
    ID3DBlob* ps = Compile(kPixel, sizeof(kPixel) - 1, pixelTarget);
    bool ok = vs && vsOne && ps;
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    for (int i = 0; ok && i < 2; i++)
        ok = SUCCEEDED(b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &b.vertex[i])) &&
             SUCCEEDED(b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &b.pixel[i]));
    ok = ok && SUCCEEDED(b.device->CreateVertexShader(vsOne->GetBufferPointer(), vsOne->GetBufferSize(), nullptr, &b.vertexOne)) &&
         SUCCEEDED(b.device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &b.layout)) &&
         SUCCEEDED(b.device->CreateInputLayout(elements, 2, vsOne->GetBufferPointer(), vsOne->GetBufferSize(), &b.layoutOne));
    Release(vs);
    Release(vsOne);
    Release(ps);
    if (!ok) return false;

    D3D11_BUFFER_DESC bd = {};
    D3D11_SUBRESOURCE_DATA data = {};
    for (int i = 0; i < 2; i++) {
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = sizeof(kQuad);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        data.pSysMem = kQuad;
        if (FAILED(b.device->CreateBuffer(&bd, &data, &b.vertices[i]))) return false;
        bd.ByteWidth = sizeof(kQuadIndices);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        data.pSysMem = kQuadIndices;
        if (FAILED(b.device->CreateBuffer(&bd, &data, &b.indices[i]))) return false;
    }
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 65536;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.streamVertices))) return false;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.streamIndices))) return false;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 64;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.matrix))) return false;
    bd.ByteWidth = 64 * 16;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.big))) return false;
    bd.ByteWidth = 64 + 64 * 16;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.all))) return false;
    bd.ByteWidth = 16;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.colour))) return false;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    bd.ByteWidth = 64;
    if (FAILED(b.device->CreateBuffer(&bd, nullptr, &b.matrixDefault))) return false;

    static unsigned char pixels[4 * 4 * 4];
    memset(pixels, 0xFF, sizeof(pixels));
    td.Width = td.Height = 4;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = pixels;
    data.SysMemPitch = 16;
    for (int i = 0; i < 2; i++)
        if (FAILED(b.device->CreateTexture2D(&td, &data, &b.textures[i])) ||
            FAILED(b.device->CreateShaderResourceView(b.textures[i], nullptr, &b.textureViews[i])))
            return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(b.device->CreateSamplerState(&sd, &b.samplers[0]))) return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (FAILED(b.device->CreateSamplerState(&sd, &b.samplers[1]))) return false;

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(b.device->CreateBlendState(&blend, &b.blends[0]))) return false;
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(b.device->CreateBlendState(&blend, &b.blends[1]))) return false;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    if (FAILED(b.device->CreateBlendState(&blend, &b.blends[2]))) return false;

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(b.device->CreateDepthStencilState(&ds, &b.depths[0]))) return false;  // no test, no writing
    ds.DepthEnable = TRUE;
    if (FAILED(b.device->CreateDepthStencilState(&ds, &b.depths[1]))) return false;  // always, no writing
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (FAILED(b.device->CreateDepthStencilState(&ds, &b.depths[2]))) return false;  // always, writing
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(b.device->CreateDepthStencilState(&ds, &b.depths[3]))) return false;  // less or equal, no writing

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.DepthClipEnable = TRUE;
    rs.CullMode = D3D11_CULL_NONE;
    if (FAILED(b.device->CreateRasterizerState(&rs, &b.culls[0]))) return false;
    rs.CullMode = D3D11_CULL_BACK;
    if (FAILED(b.device->CreateRasterizerState(&rs, &b.culls[1]))) return false;
    rs.CullMode = D3D11_CULL_FRONT;
    if (FAILED(b.device->CreateRasterizerState(&rs, &b.culls[2]))) return false;

    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    return SUCCEEDED(b.device->CreateQuery(&qd, &b.done));
}

void Base(Bench& b, ID3D11DeviceContext* c) {
    static float big[64 * 4];
    static const float white[4] = {1, 1, 1, 1};
    b.Write(b.matrix, kIdentity, 64);
    b.Write(b.big, big, sizeof(big));
    b.Write(b.colour, white, 16);
    D3D11_VIEWPORT viewport = {0, 0, 768, 432, 0, 1};
    UINT stride = 20, offset = 0;
    ID3D11Buffer* constants[2] = {b.matrix, b.big};
    c->OMSetRenderTargets(1, &b.targetView, b.depthView);
    c->RSSetViewports(1, &viewport);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(b.layout);
    c->IASetVertexBuffers(0, 1, &b.vertices[0], &stride, &offset);
    c->IASetIndexBuffer(b.indices[0], DXGI_FORMAT_R16_UINT, 0);
    c->VSSetShader(b.vertex[0], nullptr, 0);
    c->PSSetShader(b.pixel[0], nullptr, 0);
    c->VSSetConstantBuffers(0, 2, constants);
    c->PSSetConstantBuffers(0, 1, &b.colour);
    c->PSSetShaderResources(0, 1, &b.textureViews[0]);
    c->PSSetSamplers(0, 1, &b.samplers[0]);
    c->OMSetBlendState(b.blends[0], nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(b.depths[0], 0);
    c->RSSetState(b.culls[0]);
}

void Run(UINT flags, const char* kind) {
    enum { N = 400 };
    Bench b;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    if (!Make(b, flags, &level)) {
        host_logf("d3d bench: could not be set up (%s)", kind);
        return;
    }
    ID3D11DeviceContext* c = b.context;
    host_logf("d3d bench: Direct3D 11 at feature level %s, %s", LevelName(level), kind);

    static const char* names[] = {
        "nothing changed",
        "a vec4 for the pixel shader",
        "a mat4 for the vertex shader, in a buffer of its own",
        "both",
        "64 vec4 for the vertex shader",
        "a mat4 by UpdateSubresource",
        "a mat4 that shares a buffer with the 64 vec4",
        "the other texture",
        "the other sampler",
        "the same buffers bound again",
        "the other buffers",
        "blending on or off",
        "the other blend function",
        "depth test on: depth writing on or off",
        "depth test on: the other depth function",
        "culling on or off",
        "the other face culled",
        "the other shaders",
        "the other shaders and a mat4 for them",
        "the other pixel shader",
        "Draw, nothing changed",
        "new vertices, the buffer discarded for each draw",
        "new vertices and indices further along each time, nothing overwritten",
        "nothing changed, again",
    };
    static float big[64 * 4];
    static float all[16 + 64 * 4];
    memcpy(all, kIdentity, 64);
    UINT stride = 20, offset = 0;

    Base(b, c);
    c->ClearRenderTargetView(b.targetView, kIdentity);
    c->ClearDepthStencilView(b.depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    for (int i = 0; i < 20; i++) c->DrawIndexed(6, 0, 0);
    b.Finish();

    for (int regime = 0; regime < (int)(sizeof(names) / sizeof(names[0])); regime++) {
        if (regime == 5) c->VSSetConstantBuffers(0, 1, &b.matrixDefault);
        if (regime == 6) {
            b.Write(b.all, all, sizeof(all));
            c->IASetInputLayout(b.layoutOne);
            c->VSSetShader(b.vertexOne, nullptr, 0);
            c->VSSetConstantBuffers(0, 1, &b.all);
        }
        if (regime == 13 || regime == 14) c->OMSetDepthStencilState(b.depths[1], 0);
        if (regime == 21 || regime == 22) {
            c->IASetVertexBuffers(0, 1, &b.streamVertices, &stride, &offset);
            if (regime == 22) c->IASetIndexBuffer(b.streamIndices, DXGI_FORMAT_R16_UINT, 0);
        }
        if (regime == 5 || regime == 6 || regime == 13 || regime == 14 || regime == 21 || regime == 22)
            for (int i = 0; i < 4; i++) {
                if (regime == 5) c->UpdateSubresource(b.matrixDefault, 0, nullptr, kIdentity, 0, 0);
                if (regime == 21) b.Write(b.streamVertices, kQuad, sizeof(kQuad));
                if (regime == 22) {
                    b.Write(b.streamVertices, kQuad, sizeof(kQuad));
                    b.Write(b.streamIndices, kQuadIndices, sizeof(kQuadIndices));
                }
                c->DrawIndexed(6, 0, 0);
            }
        b.Finish();
        double t0 = Now();
        for (int i = 0; i < N; i++) {
            float f = (i & 1) ? 1.0f : 0.99f;
            float colour[4] = {f, 1.0f, 1.0f, 1.0f};
            unsigned short moved[6];
            switch (regime) {
            case 1: b.Write(b.colour, colour, 16); break;
            case 2: b.Write(b.matrix, kIdentity, 64); break;
            case 3:
                b.Write(b.colour, colour, 16);
                b.Write(b.matrix, kIdentity, 64);
                break;
            case 4:
                big[4] = f;
                b.Write(b.big, big, sizeof(big));
                break;
            case 5: c->UpdateSubresource(b.matrixDefault, 0, nullptr, kIdentity, 0, 0); break;
            case 6: b.Write(b.all, all, sizeof(all)); break;
            case 7: c->PSSetShaderResources(0, 1, &b.textureViews[i & 1]); break;
            case 8: c->PSSetSamplers(0, 1, &b.samplers[i & 1]); break;
            case 9:
                c->IASetVertexBuffers(0, 1, &b.vertices[0], &stride, &offset);
                c->IASetIndexBuffer(b.indices[0], DXGI_FORMAT_R16_UINT, 0);
                break;
            case 10:
                c->IASetVertexBuffers(0, 1, &b.vertices[i & 1], &stride, &offset);
                c->IASetIndexBuffer(b.indices[i & 1], DXGI_FORMAT_R16_UINT, 0);
                break;
            case 11: c->OMSetBlendState(b.blends[i & 1], nullptr, 0xFFFFFFFF); break;
            case 12: c->OMSetBlendState(b.blends[1 + (i & 1)], nullptr, 0xFFFFFFFF); break;
            case 13: c->OMSetDepthStencilState(b.depths[1 + (i & 1)], 0); break;
            case 14: c->OMSetDepthStencilState(b.depths[(i & 1) ? 1 : 3], 0); break;
            case 15: c->RSSetState(b.culls[i & 1]); break;
            case 16: c->RSSetState(b.culls[1 + (i & 1)]); break;
            case 17:
                c->VSSetShader(b.vertex[i & 1], nullptr, 0);
                c->PSSetShader(b.pixel[i & 1], nullptr, 0);
                break;
            case 18:
                c->VSSetShader(b.vertex[i & 1], nullptr, 0);
                c->PSSetShader(b.pixel[i & 1], nullptr, 0);
                b.Write(b.matrix, kIdentity, 64);
                break;
            case 19: c->PSSetShader(b.pixel[i & 1], nullptr, 0); break;
            case 21: b.Write(b.streamVertices, kQuad, sizeof(kQuad)); break;
            case 22:
                for (int j = 0; j < 6; j++) moved[j] = (unsigned short)(i * 4 + kQuadIndices[j]);
                b.Write(b.streamVertices, kQuad, sizeof(kQuad), i ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                        i * sizeof(kQuad));
                b.Write(b.streamIndices, moved, sizeof(moved), i ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                        i * sizeof(moved));
                break;
            }
            if (regime == 20)
                c->Draw(3, 0);
            else if (regime == 22)
                c->DrawIndexed(6, i * 6, 0);
            else
                c->DrawIndexed(6, 0, 0);
        }
        double t1 = Now();
        b.Finish();
        double t2 = Now();
        host_logf("d3d bench: %s: %.1f us a draw, %.1f with what the GPU had left to do", names[regime], (t1 - t0) / N,
                  (t2 - t0) / N);
        Base(b, c);
    }

    // The calls of a frame recorded on one thread, to be made on another: Direct3D's own way
    // of having a drawing thread. What recording costs, and what making them does then.
    D3D11_FEATURE_DATA_THREADING threading = {};
    b.device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading));
    ID3D11DeviceContext* deferred = nullptr;
    HRESULT hr = b.device->CreateDeferredContext(0, &deferred);
    host_logf("d3d bench: a deferred context: %s (0x%08X); the driver's own command lists: %s, objects made on "
              "several threads at once: %s",
              SUCCEEDED(hr) ? "yes" : "NO", (unsigned)hr, threading.DriverCommandLists ? "yes" : "no",
              threading.DriverConcurrentCreates ? "yes" : "no");
    if (SUCCEEDED(hr) && deferred) {
        for (int pass = 0; pass < 2; pass++) {
            b.Finish();
            double t0 = Now();
            Base(b, deferred);
            for (int i = 0; i < N; i++) {
                if (pass == 1) {
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(deferred->Map(b.matrix, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        memcpy(mapped.pData, kIdentity, 64);
                        deferred->Unmap(b.matrix, 0);
                    }
                    deferred->PSSetShaderResources(0, 1, &b.textureViews[i & 1]);
                }
                deferred->DrawIndexed(6, 0, 0);
            }
            ID3D11CommandList* list = nullptr;
            HRESULT finished = deferred->FinishCommandList(FALSE, &list);
            double t1 = Now();
            if (SUCCEEDED(finished) && list) {
                c->ExecuteCommandList(list, FALSE);
                double t2 = Now();
                b.Finish();
                double t3 = Now();
                host_logf("d3d bench: recorded, %s: %.1f us a draw to record, %.1f to make the calls, %.1f with what "
                          "the GPU had left to do",
                          pass ? "a mat4 and the other texture each time" : "nothing changed", (t1 - t0) / N,
                          (t2 - t1) / N, (t3 - t1) / N);
                list->Release();
            } else {
                host_logf("d3d bench: the recorded calls could not be finished, 0x%08X", (unsigned)finished);
            }
            Base(b, c);
        }
        deferred->Release();
    }
}

// What a renderer would ask this Direct3D for
void Capabilities() {
    Bench b;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 6, D3D11_SDK_VERSION, &b.device,
                                 &level, &b.context)))
        return;
    static const struct {
        DXGI_FORMAT format;
        const char* name;
    } formats[] = {
        {DXGI_FORMAT_BC1_UNORM, "BC1"},
        {DXGI_FORMAT_BC2_UNORM, "BC2"},
        {DXGI_FORMAT_BC3_UNORM, "BC3"},
        {DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
        {DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8"},
        {DXGI_FORMAT_B8G8R8X8_UNORM, "BGRX8"},
        {DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5"},
        {DXGI_FORMAT_B5G5R5A1_UNORM, "B5G5R5A1"},
        {DXGI_FORMAT_B4G4R4A4_UNORM, "B4G4R4A4"},
        {DXGI_FORMAT_A8_UNORM, "A8"},
        {DXGI_FORMAT_R8_UNORM, "R8"},
        {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24S8"},
        {DXGI_FORMAT_D16_UNORM, "D16"},
        {DXGI_FORMAT_R16_UINT, "R16 (indices)"},
        {DXGI_FORMAT_R32_UINT, "R32 (indices)"},
        {DXGI_FORMAT_R8G8B8A8_UINT, "RGBA8 as integers (vertices)"},
    };
    char text[900];
    int used = snprintf(text, sizeof(text), "d3d bench: the device's own level is %s; formats:", LevelName(level));
    for (const auto& f : formats) {
        UINT support = 0;
        b.device->CheckFormatSupport(f.format, &support);
        used += snprintf(text + used, sizeof(text) - used, " %s %s%s%s%s%s%s;", f.name,
                         support & D3D11_FORMAT_SUPPORT_TEXTURE2D ? "t" : "",
                         support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE ? "s" : "",
                         support & D3D11_FORMAT_SUPPORT_MIP ? "m" : "",
                         support & D3D11_FORMAT_SUPPORT_RENDER_TARGET ? "r" : "",
                         support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ? "d" : "",
                         support & (D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER | D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER) ? "i" : "");
        if (used > (int)sizeof(text) - 60) break;
    }
    host_log(text);
    host_log("d3d bench: (t a texture, s sampled, m with mip levels, r a render target, d depth, i vertex or index input)");
}

}  // namespace

extern "C" void W10M_Direct3DBenchmark(void) {
    Capabilities();
    Run(0, "a device as Direct3D makes it");
    Run(D3D11_CREATE_DEVICE_SINGLETHREADED, "a device for one thread (no locks of its own)");
}
