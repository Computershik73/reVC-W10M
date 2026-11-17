// What a draw costs through Direct3D 11 itself (REVC_BENCH=1), next to what it costs through
// ANGLE (glbench.cpp): the same small quad, the same number of times, one thing changed
// between the draws at a time. On a device of its own, into a texture: nothing of the game's
// picture is touched. What the two tables differ by is what a renderer that calls Direct3D
// itself could save over one that calls OpenGL ES; what they have in common is the phone's
// driver, and stays.
//
// It also says what this phone's Direct3D has: the formats a renderer would ask for, and
// whether the calls of a frame can be recorded on one thread and made on another.
//
// REVC_BENCH=fill is another measurement, further down: what the GPU's own work costs, a
// layer of pixels and a hundred thousand triangles.
#include <windows.h>

#include <d3d11_1.h>
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
// the matrix not from a constant buffer but from the vertices, one for an instance: what a
// draw is placed with then changes with where a second vertex buffer is read from
const char kVertexInstanced[] =
    "cbuffer Big : register(b1) { float4 u_big[64]; };\n"
    "struct VI { float3 pos : POSITION; float2 tex : TEXCOORD0;\n"
    "  float4 m0 : TEXCOORD1; float4 m1 : TEXCOORD2; float4 m2 : TEXCOORD3; float4 m3 : TEXCOORD4; };\n"
    "struct VO { float4 pos : SV_Position; float2 tex : TEXCOORD0; };\n"
    "VO main(VI i) { VO o; o.tex = i.tex; float4 p = float4(i.pos.xy, 0.0, 1.0);\n"
    "  o.pos = float4(dot(i.m0, p), dot(i.m1, p), dot(i.m2, p), dot(i.m3, p)) + u_big[(int)i.pos.z]*0.000001; return o; }\n";
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
    ID3D11VertexShader *vertex[2] = {}, *vertexOne = nullptr, *vertexInstanced = nullptr;
    ID3D11PixelShader* pixel[2] = {};
    ID3D11InputLayout *layout = nullptr, *layoutOne = nullptr, *layoutInstanced = nullptr;
    // a matrix that is there already, twice; 400 of them for instances; 200 in one constant buffer
    ID3D11Buffer *matrixTwo[2] = {}, *instances = nullptr, *manyMatrices = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
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
        Release(context1);
        Release(manyMatrices);
        Release(instances);
        for (auto& p : matrixTwo) Release(p);
        Release(layoutInstanced);
        Release(vertexInstanced);
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

    // other ways of handing a draw its matrix (they may not be there: the rows say so)
    {
        static float matrices[400 * 16];
        for (int i = 0; i < 400; i++) memcpy(matrices + i * 16, kIdentity, 64);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.ByteWidth = 64;
        data.pSysMem = kIdentity;
        for (int i = 0; i < 2; i++) b.device->CreateBuffer(&bd, &data, &b.matrixTwo[i]);
        bd.ByteWidth = 200 * 256;
        static float spaced[200 * 64];
        for (int i = 0; i < 200; i++) memcpy(spaced + i * 64, kIdentity, 64);
        data.pSysMem = spaced;
        b.device->CreateBuffer(&bd, &data, &b.manyMatrices);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.CPUAccessFlags = 0;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.ByteWidth = sizeof(matrices);
        data.pSysMem = matrices;
        b.device->CreateBuffer(&bd, &data, &b.instances);
        static const D3D11_INPUT_ELEMENT_DESC instanced[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        if (ID3DBlob* code = Compile(kVertexInstanced, sizeof(kVertexInstanced) - 1, vertexTarget)) {
            if (SUCCEEDED(b.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &b.vertexInstanced)))
                b.device->CreateInputLayout(instanced, 6, code->GetBufferPointer(), code->GetBufferSize(), &b.layoutInstanced);
            Release(code);
        }
        D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
        if (SUCCEEDED(b.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
            options.ConstantBufferOffsetting)
            b.context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&b.context1);
    }

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
        "a mat4 for the vertex shader: the other of two buffers that have it already",
        "a mat4 for the vertex shader from a second vertex buffer, one for an instance, further along for each draw",
        "a mat4 for the vertex shader further along in one big constant buffer (Direct3D 11.1)",
        "nothing changed, once more",
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
        if (regime == 24 && !(b.matrixTwo[0] && b.matrixTwo[1])) continue;
        if (regime == 25) {
            if (!(b.layoutInstanced && b.instances)) {
                host_logf("d3d bench: %s: not on this device", names[regime]);
                continue;
            }
            c->IASetInputLayout(b.layoutInstanced);
            c->VSSetShader(b.vertexInstanced, nullptr, 0);
        }
        if (regime == 26 && !(b.context1 && b.manyMatrices)) {
            host_logf("d3d bench: %s: not on this device", names[regime]);
            continue;
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
            case 24: c->VSSetConstantBuffers(0, 1, &b.matrixTwo[i & 1]); break;
            case 25: {
                UINT instanceStride = 64, instanceOffset = (UINT)i * 64;
                c->IASetVertexBuffers(1, 1, &b.instances, &instanceStride, &instanceOffset);
                break;
            }
            case 26: {
                UINT first = (UINT)(i % 200) * 16, count = 16;
                b.context1->VSSetConstantBuffers1(0, 1, &b.manyMatrices, &first, &count);
                break;
            }
            case 22:
                for (int j = 0; j < 6; j++) moved[j] = (unsigned short)(i * 4 + kQuadIndices[j]);
                b.Write(b.streamVertices, kQuad, sizeof(kQuad), i ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                        i * sizeof(kQuad));
                b.Write(b.streamIndices, moved, sizeof(moved), i ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                        i * sizeof(moved));
                break;
            }
            if (regime == 25)
                c->DrawIndexedInstanced(6, 1, 0, 0, 0);
            else if (regime == 20)
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

// ===[ What the GPU itself costs (REVC_BENCH=fill) ]===
// The draws above are the processor's work. Where a frame waits for the GPU (a Lumia 650 at
// more than 640x360) this is what it waits for: a layer of pixels over the whole picture, by
// what it is drawn with and by whether it is hidden; and a hundred thousand triangles, by
// the vertex shader that places them.

const char kFillVertex[] =
    "cbuffer C : register(b0) { float4 u_place; float4 u_uv; };\n"
    "struct VI { float3 pos : POSITION; float2 tex : TEXCOORD0; };\n"
    "struct VO { float4 colour : TEXCOORD0; float3 tex : TEXCOORD1; float4 atlas : TEXCOORD2; float4 pos : SV_Position; };\n"
    "VO main(VI i) { VO o; o.pos = float4(i.pos.xy, u_place.x, 1.0); o.colour = float4(1.0, 1.0, 1.0, u_place.y);\n"
    "  o.tex = float3(i.tex*u_uv.xy, 0.9); o.atlas = float4(0.0, 0.0, 1.0, 1.0); return o; }\n";

#define FILL_PIXEL_HEAD \
    "cbuffer P : register(b0) { float4 u_alphaRef; float4 u_fogColor; };\n" \
    "Texture2D tex0 : register(t0);\n" \
    "SamplerState smp0 : register(s0);\n" \
    "struct PI { float4 colour : TEXCOORD0; float3 tex : TEXCOORD1; float4 atlas : TEXCOORD2; };\n"
const char kFillColour[] = FILL_PIXEL_HEAD "float4 main(PI v) : SV_Target { return v.colour; }\n";
// the renderer's simple_ps.hlsl without the alpha test, with it, and its atlas_ps.hlsl
const char kFillSimple[] = FILL_PIXEL_HEAD
    "float4 main(PI v) : SV_Target { float4 c = v.colour*tex0.Sample(smp0, v.tex.xy);\n"
    "  c.rgb = lerp(u_fogColor.rgb, c.rgb, v.tex.z); return c; }\n";
const char kFillTest[] = FILL_PIXEL_HEAD
    "float4 main(PI v) : SV_Target { float4 c = v.colour*tex0.Sample(smp0, v.tex.xy);\n"
    "  c.rgb = lerp(u_fogColor.rgb, c.rgb, v.tex.z);\n"
    "  if(c.a < u_alphaRef.x || c.a >= u_alphaRef.y) discard; return c; }\n";
const char kFillAtlas[] = FILL_PIXEL_HEAD
    "float4 main(PI v) : SV_Target { float2 uv = v.tex.xy;\n"
    "  if(v.atlas.z > 0.0) uv = v.atlas.xy + frac(uv)*v.atlas.zw;\n"
    "  float4 c = v.colour*tex0.Sample(smp0, uv);\n"
    "  c.rgb = lerp(u_fogColor.rgb, c.rgb, v.tex.z);\n"
    "  if(c.a < u_alphaRef.x || c.a >= u_alphaRef.y) discard; return c; }\n";

// The renderer's default_vs.hlsl: ONE with the three matrices as one, THREE as it has them,
// LIGHTS with its eight lights as well (none of them lit: what a building is drawn with when
// the one shader with the lights draws everything)
#define GEOMETRY_HEAD \
    "cbuffer G : register(b0) { float4x4 u_world; float4x4 u_view; float4x4 u_proj;\n" \
    "  float4 u_ambLight; float4 u_matColor; float4 u_surfProps; float4 u_fogData;\n" \
    "  float4 u_lightParams[8]; float4 u_lightPosition[8]; float4 u_lightDirection[8]; float4 u_lightColor[8]; };\n" \
    "struct VI { float3 pos : POSITION; float2 tex : TEXCOORD0; };\n" \
    "struct VO { float4 colour : TEXCOORD0; float3 tex : TEXCOORD1; float4 atlas : TEXCOORD2; float4 pos : SV_Position; };\n"
const char kGeometryOne[] = GEOMETRY_HEAD
    "VO main(VI i) { VO o; o.pos = mul(u_proj, float4(i.pos.xy, 0.5, 1.0));\n"
    "  o.colour = u_matColor; o.tex = float3(i.tex, 0.9); o.atlas = float4(0.0, 0.0, 0.0, 0.0); return o; }\n";
const char kGeometryThree[] = GEOMETRY_HEAD
    "VO main(VI i) { VO o; float4 V = mul(u_world, float4(i.pos.xy, 0.5, 1.0));\n"
    "  o.pos = mul(u_proj, mul(u_view, V));\n"
    "  float4 c = float4(i.tex, 0.5, 1.0); c.rgb += u_ambLight.rgb*u_surfProps.x; c = clamp(c, 0.0, 1.0);\n"
    "  o.colour = c*u_matColor; o.tex = float3(i.tex.x, 1.0 - i.tex.y, clamp((o.pos.w - u_fogData.y)*u_fogData.z, u_fogData.w, 1.0));\n"
    "  o.atlas = float4(0.0, 0.0, 0.0, 0.0); return o; }\n";
const char kGeometryLights[] = GEOMETRY_HEAD
    "VO main(VI i) { VO o; float4 V = mul(u_world, float4(i.pos.xy, 0.5, 1.0));\n"
    "  o.pos = mul(u_proj, mul(u_view, V));\n"
    "  float3 N = mul((float3x3)u_world, float3(0.0, 0.0, 1.0));\n"
    "  float4 c = float4(i.tex, 0.5, 1.0); c.rgb += u_ambLight.rgb*u_surfProps.x;\n"
    "  float3 lit = float3(0.0, 0.0, 0.0);\n"
    "  [loop] for(int l = 0; l < 8; l++){\n"
    "    float type = u_lightParams[l].x;\n"
    "    float d = max(0.0, dot(N, -u_lightDirection[l].xyz));\n"
    "    lit += (type == 1.0 ? d : 0.0)*u_lightColor[l].rgb;\n"
    "    float3 dir = V.xyz - u_lightPosition[l].xyz;\n"
    "    float dist = max(length(dir), 0.0001);\n"
    "    float atten = max(0.0, 1.0 - dist/max(u_lightParams[l].y, 0.0001));\n"
    "    dir /= dist;\n"
    "    float p = max(0.0, dot(N, -dir));\n"
    "    lit += (type == 2.0 ? p*atten : 0.0)*u_lightColor[l].rgb;\n"
    "    float pcos = dot(dir, u_lightDirection[l].xyz);\n"
    "    float ccos = -u_lightParams[l].z;\n"
    "    float falloff = (pcos - ccos)/(1.0 - ccos);\n"
    "    float spot = falloff < 0.0 ? 0.0 : p*max(falloff, u_lightParams[l].w);\n"
    "    lit += (type == 3.0 ? spot*atten : 0.0)*u_lightColor[l].rgb;\n"
    "  }\n"
    "  c.rgb += lit*u_surfProps.z; c = clamp(c, 0.0, 1.0);\n"
    "  o.colour = c*u_matColor; o.tex = float3(i.tex.x, 1.0 - i.tex.y, clamp((o.pos.w - u_fogData.y)*u_fogData.z, u_fogData.w, 1.0));\n"
    "  o.atlas = float4(0.0, 0.0, 0.0, 0.0); return o; }\n";

// atlas_ps.hlsl in other forms: without the alpha test; with the texture's level chosen by
// the slopes of the coordinates as they were before the wrapping (what an atlas with smaller
// copies of its textures needs: at a wrap the wrapped coordinates jump); and the coordinates
// worked out but not wrapped, to see what the working out alone costs
const char kFillAtlasPlain[] = FILL_PIXEL_HEAD
    "float4 main(PI v) : SV_Target { float2 uv = v.atlas.xy + frac(v.tex.xy)*v.atlas.zw;\n"
    "  float4 c = v.colour*tex0.Sample(smp0, uv);\n"
    "  c.rgb = lerp(u_fogColor.rgb, c.rgb, v.tex.z); return c; }\n";
const char kFillAtlasGrad[] = FILL_PIXEL_HEAD
    "float4 main(PI v) : SV_Target { float2 uv = v.atlas.xy + frac(v.tex.xy)*v.atlas.zw;\n"
    "  float4 c = v.colour*tex0.SampleGrad(smp0, uv, ddx(v.tex.xy)*v.atlas.zw, ddy(v.tex.xy)*v.atlas.zw);\n"
    "  c.rgb = lerp(u_fogColor.rgb, c.rgb, v.tex.z);\n"
    "  if(c.a < u_alphaRef.x || c.a >= u_alphaRef.y) discard; return c; }\n";
const char kFillWorkedOut[] = FILL_PIXEL_HEAD
    "float4 main(PI v) : SV_Target { float2 uv = v.atlas.xy + v.tex.xy*v.atlas.zw;\n"
    "  float4 c = v.colour*tex0.Sample(smp0, uv);\n"
    "  c.rgb = lerp(u_fogColor.rgb, c.rgb, v.tex.z);\n"
    "  if(c.a < u_alphaRef.x || c.a >= u_alphaRef.y) discard; return c; }\n";

struct Fill {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D *target = nullptr, *depth = nullptr;
    ID3D11RenderTargetView* targetView = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    ID3D11VertexShader *vertex = nullptr, *geometry[3] = {};
    ID3D11PixelShader* pixel[7] = {};
    ID3D11InputLayout *layout = nullptr, *geometryLayout[3] = {};
    ID3D11Buffer *quad = nullptr, *quadIndices = nullptr, *grid = nullptr, *gridIndices = nullptr;
    ID3D11Buffer *place = nullptr, *pixelConstants = nullptr, *geometryConstants = nullptr;
    ID3D11Texture2D* textures[3] = {};
    ID3D11ShaderResourceView* views[3] = {};
    ID3D11SamplerState* samplers[2] = {};
    ID3D11BlendState* blends[2] = {};
    ID3D11DepthStencilState* depths[3] = {};
    ID3D11RasterizerState* raster = nullptr;
    ID3D11Query* done = nullptr;
    int width = 0, height = 0;
    UINT gridCount = 0;

    ~Fill() {
        if (context) context->ClearState();
        Release(done);
        Release(raster);
        for (auto& p : depths) Release(p);
        for (auto& p : blends) Release(p);
        for (auto& p : samplers) Release(p);
        for (auto& p : views) Release(p);
        for (auto& p : textures) Release(p);
        Release(geometryConstants);
        Release(pixelConstants);
        Release(place);
        Release(gridIndices);
        Release(grid);
        Release(quadIndices);
        Release(quad);
        for (auto& p : geometryLayout) Release(p);
        Release(layout);
        for (auto& p : pixel) Release(p);
        for (auto& p : geometry) Release(p);
        Release(vertex);
        ReleaseTarget();
        Release(context);
        Release(device);
    }

    void ReleaseTarget() {
        Release(depthView);
        Release(targetView);
        Release(depth);
        Release(target);
    }

    void Finish() {
        context->End(done);
        context->Flush();
        BOOL finished = FALSE;
        double t0 = Now();
        while (context->GetData(done, &finished, sizeof(finished), 0) != S_OK || !finished) {
            if (Now() - t0 > 20e6) break;
            Sleep(0);
        }
    }

    bool Write(ID3D11Buffer* buffer, const void* data, size_t size) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        memcpy(mapped.pData, data, size);
        context->Unmap(buffer, 0);
        return true;
    }

    bool Target(int w, int h, DXGI_FORMAT depthFormat) {
        ReleaseTarget();
        width = w;
        height = h;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &target)) ||
            FAILED(device->CreateRenderTargetView(target, nullptr, &targetView)))
            return false;
        td.Format = depthFormat;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &depth)) &&
               SUCCEEDED(device->CreateDepthStencilView(depth, nullptr, &depthView));
    }
};

unsigned Noise() {
    static unsigned state = 0x2545F491u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// a compressed texture (BC1, as most of the game's are) of 512x512 with or without its smaller copies
bool MakeCompressed(Fill& f, int index, bool mips) {
    static unsigned char blocks[(512 / 4) * (512 / 4) * 8 * 2];
    D3D11_SUBRESOURCE_DATA data[10] = {};
    size_t at = 0;
    int levels = 0;
    for (int size = 512; size >= 1; size /= 2) {
        int across = size >= 4 ? size / 4 : 1;
        data[levels].pSysMem = blocks + at;
        data[levels].SysMemPitch = across * 8;
        for (int i = 0; i < across * across; i++) {
            unsigned short c0 = (unsigned short)Noise(), c1 = (unsigned short)Noise();
            if (c0 < c1) {  // the first colour the greater: four colours and no transparent one
                unsigned short t = c0;
                c0 = c1;
                c1 = t;
            }
            if (c0 == c1) c0 = (unsigned short)(c1 + 1);
            unsigned bits = Noise();
            unsigned char* b = blocks + at + i * 8;
            b[0] = (unsigned char)c0;
            b[1] = (unsigned char)(c0 >> 8);
            b[2] = (unsigned char)c1;
            b[3] = (unsigned char)(c1 >> 8);
            memcpy(b + 4, &bits, 4);
        }
        at += (size_t)across * across * 8;
        levels++;
        if (!mips) break;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 512;
    td.MipLevels = levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC1_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(f.device->CreateTexture2D(&td, data, &f.textures[index])) &&
           SUCCEEDED(f.device->CreateShaderResourceView(f.textures[index], nullptr, &f.views[index]));
}

bool MakeFill(Fill& f, D3D_FEATURE_LEVEL* got) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3};
    int first = 0;
    if (const char* cap = getenv("REVC_ANGLE_FL"))
        if (strcmp(cap, "9_3") == 0) first = 3;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + first, 4 - first, D3D11_SDK_VERSION,
                                 &f.device, got, &f.context)))
        return false;
    const char* vertexTarget = *got >= D3D_FEATURE_LEVEL_10_0 ? "vs_4_0" : "vs_4_0_level_9_3";
    const char* pixelTarget = *got >= D3D_FEATURE_LEVEL_10_0 ? "ps_4_0" : "ps_4_0_level_9_3";
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    ID3DBlob* vs = Compile(kFillVertex, sizeof(kFillVertex) - 1, vertexTarget);
    if (!vs) return false;
    bool ok = SUCCEEDED(f.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &f.vertex)) &&
              SUCCEEDED(f.device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &f.layout));
    Release(vs);
    static const struct {
        const char* source;
        size_t length;
    } geometry[3] = {{kGeometryOne, sizeof(kGeometryOne) - 1},
                     {kGeometryThree, sizeof(kGeometryThree) - 1},
                     {kGeometryLights, sizeof(kGeometryLights) - 1}},
      pixel[7] = {{kFillColour, sizeof(kFillColour) - 1},
                  {kFillSimple, sizeof(kFillSimple) - 1},
                  {kFillTest, sizeof(kFillTest) - 1},
                  {kFillAtlas, sizeof(kFillAtlas) - 1},
                  {kFillAtlasPlain, sizeof(kFillAtlasPlain) - 1},
                  {kFillAtlasGrad, sizeof(kFillAtlasGrad) - 1},
                  {kFillWorkedOut, sizeof(kFillWorkedOut) - 1}};
    for (int i = 0; ok && i < 3; i++) {
        ID3DBlob* code = Compile(geometry[i].source, geometry[i].length, vertexTarget);
        ok = code && SUCCEEDED(f.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &f.geometry[i])) &&
             SUCCEEDED(f.device->CreateInputLayout(elements, 2, code->GetBufferPointer(), code->GetBufferSize(), &f.geometryLayout[i]));
        Release(code);
    }
    for (int i = 0; ok && i < 7; i++) {
        ID3DBlob* code = Compile(pixel[i].source, pixel[i].length, pixelTarget);
        // the last three may not be there for every GPU: their rows are left out then
        bool made = code && SUCCEEDED(f.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &f.pixel[i]));
        if (i < 4) ok = made;
        Release(code);
    }
    if (!ok) return false;

    // a quad over the whole picture, and a grid of 128x128 squares over it: 32768 triangles
    static const float quad[4 * 5] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
    };
    enum { G = 128 };
    static float grid[(G + 1) * (G + 1) * 5];
    static unsigned short gridIndices[G * G * 6];
    for (int y = 0; y <= G; y++)
        for (int x = 0; x <= G; x++) {
            float* v = &grid[(y * (G + 1) + x) * 5];
            v[0] = -1.0f + 2.0f * x / G;
            v[1] = -1.0f + 2.0f * y / G;
            v[2] = 0.0f;
            v[3] = (float)x / G;
            v[4] = (float)y / G;
        }
    for (int y = 0; y < G; y++)
        for (int x = 0; x < G; x++) {
            unsigned short* i = &gridIndices[(y * G + x) * 6];
            unsigned short a = (unsigned short)(y * (G + 1) + x), b = (unsigned short)(a + 1);
            unsigned short c = (unsigned short)(a + G + 1), d = (unsigned short)(c + 1);
            i[0] = a; i[1] = b; i[2] = d;
            i[3] = a; i[4] = d; i[5] = c;
        }
    f.gridCount = G * G * 6;
    D3D11_BUFFER_DESC bd = {};
    D3D11_SUBRESOURCE_DATA data = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = sizeof(quad);
    data.pSysMem = quad;
    if (FAILED(f.device->CreateBuffer(&bd, &data, &f.quad))) return false;
    bd.ByteWidth = sizeof(grid);
    data.pSysMem = grid;
    if (FAILED(f.device->CreateBuffer(&bd, &data, &f.grid))) return false;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = sizeof(kQuadIndices);
    data.pSysMem = kQuadIndices;
    if (FAILED(f.device->CreateBuffer(&bd, &data, &f.quadIndices))) return false;
    bd.ByteWidth = sizeof(gridIndices);
    data.pSysMem = gridIndices;
    if (FAILED(f.device->CreateBuffer(&bd, &data, &f.gridIndices))) return false;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 32;
    if (FAILED(f.device->CreateBuffer(&bd, nullptr, &f.place)) || FAILED(f.device->CreateBuffer(&bd, nullptr, &f.pixelConstants)))
        return false;
    bd.ByteWidth = 768;
    if (FAILED(f.device->CreateBuffer(&bd, nullptr, &f.geometryConstants))) return false;

    static unsigned char noise[64 * 64 * 4];
    for (size_t i = 0; i < sizeof(noise); i++) noise[i] = (i & 3) == 3 ? 0xFF : (unsigned char)Noise();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = noise;
    data.SysMemPitch = 64 * 4;
    if (FAILED(f.device->CreateTexture2D(&td, &data, &f.textures[0])) ||
        FAILED(f.device->CreateShaderResourceView(f.textures[0], nullptr, &f.views[0])))
        return false;
    if (!MakeCompressed(f, 1, false) || !MakeCompressed(f, 2, true)) {
        host_log("d3d fill: no compressed texture of 512x512 (with its smaller copies?)");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;  // what the game's textures are sampled with
    if (FAILED(f.device->CreateSamplerState(&sd, &f.samplers[0]))) return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(f.device->CreateSamplerState(&sd, &f.samplers[1]))) return false;

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(f.device->CreateBlendState(&blend, &f.blends[0]))) return false;
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(f.device->CreateBlendState(&blend, &f.blends[1]))) return false;

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(f.device->CreateDepthStencilState(&ds, &f.depths[0]))) return false;  // no depth
    ds.DepthEnable = TRUE;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (FAILED(f.device->CreateDepthStencilState(&ds, &f.depths[1]))) return false;  // tested and written
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(f.device->CreateDepthStencilState(&ds, &f.depths[2]))) return false;  // tested only

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(f.device->CreateRasterizerState(&rs, &f.raster))) return false;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    return SUCCEEDED(f.device->CreateQuery(&qd, &f.done));
}

// What is drawn: so many layers, each a quad over the whole picture
struct Layers {
    const char* name;
    int pixel;         // 0 a colour, 1 a texture and the fog, 2 with the alpha test, 3 from an atlas with it,
                       // 4 from an atlas without it, 5 from an atlas by the slopes before the wrap, 6 worked out, not wrapped
    int texture;       // 0 small, 1 compressed 512x512, 2 that with its smaller copies
    int sampler;
    float texels;      // texels of the texture across a pixel of the picture
    int blend;
    int depth;         // 0 none, 1 tested and written, 2 tested only
    int order;         // 0 each layer in front of the one before, 1 the first in front of all the others
};

void SetCommon(Fill& f) {
    ID3D11DeviceContext* c = f.context;
    static const float pixelConstants[8] = {2.0f / 255.0f, 2.0f, 0.0f, 0.0f, 0.5f, 0.6f, 0.7f, 1.0f};
    f.Write(f.pixelConstants, pixelConstants, sizeof(pixelConstants));
    D3D11_VIEWPORT viewport = {0, 0, (float)f.width, (float)f.height, 0, 1};
    c->OMSetRenderTargets(1, &f.targetView, f.depthView);
    c->RSSetViewports(1, &viewport);
    c->RSSetState(f.raster);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->PSSetConstantBuffers(0, 1, &f.pixelConstants);
}

// milliseconds for a frame of so many of those layers: cleared, drawn, and done by the GPU
double Frame(Fill& f, const Layers& l, int layers) {
    ID3D11DeviceContext* c = f.context;
    static const float black[4] = {0, 0, 0, 1};
    UINT stride = 20, offset = 0;
    SetCommon(f);
    c->IASetInputLayout(f.layout);
    c->IASetVertexBuffers(0, 1, &f.quad, &stride, &offset);
    c->IASetIndexBuffer(f.quadIndices, DXGI_FORMAT_R16_UINT, 0);
    c->VSSetShader(f.vertex, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &f.place);
    c->PSSetShader(f.pixel[l.pixel], nullptr, 0);
    c->PSSetShaderResources(0, 1, &f.views[l.texture]);
    c->PSSetSamplers(0, 1, &f.samplers[l.sampler]);
    c->OMSetBlendState(f.blends[l.blend], nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(f.depths[l.depth], 0);
    float size = l.texture == 0 ? 64.0f : 512.0f;
    enum { REPEATS = 5 };
    double t0 = 0;
    for (int r = 0; r <= REPEATS; r++) {
        if (r == 1) t0 = Now();  // the first is not counted
        c->ClearRenderTargetView(f.targetView, black);
        c->ClearDepthStencilView(f.depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        for (int i = 0; i < layers; i++) {
            float z = l.order == 0 ? 0.9f - 0.05f * i : (i == 0 ? 0.1f : 0.5f + 0.02f * i);
            float place[8] = {z, l.blend ? 0.5f : 1.0f, 0, 0, f.width * l.texels / size, f.height * l.texels / size, 0, 0};
            f.Write(f.place, place, sizeof(place));
            if (l.depth == 2) {  // behind a first layer that is in the depth buffer
                c->OMSetDepthStencilState(f.depths[i == 0 ? 1 : 2], 0);
                c->OMSetBlendState(f.blends[i == 0 ? 0 : l.blend], nullptr, 0xFFFFFFFF);
            }
            c->DrawIndexed(6, 0, 0);
        }
        f.Finish();
    }
    return (Now() - t0) / REPEATS / 1000.0;
}

// milliseconds for a frame of the grid drawn so many times with one of the vertex shaders,
// over the whole picture or over a 64th of its width and height
double Grids(Fill& f, int shader, int times, bool tiny) {
    ID3D11DeviceContext* c = f.context;
    static const float black[4] = {0, 0, 0, 1};
    static float constants[768 / 4];
    UINT stride = 20, offset = 0;
    memset(constants, 0, sizeof(constants));
    float scale = tiny ? 1.0f / 64.0f : 1.0f;
    for (int m = 0; m < 3; m++)
        for (int i = 0; i < 4; i++) constants[m * 16 + i * 5] = m == 2 && i < 2 ? scale : 1.0f;
    constants[48] = constants[49] = constants[50] = 0.3f;                          // u_ambLight
    constants[52] = constants[53] = constants[54] = constants[55] = 1.0f;          // u_matColor
    constants[56] = constants[58] = 1.0f;                                          // u_surfProps
    constants[60] = 100.0f, constants[61] = 300.0f, constants[62] = -0.005f, constants[63] = 0.0f;  // u_fogData
    SetCommon(f);
    f.Write(f.geometryConstants, constants, sizeof(constants));
    c->IASetInputLayout(f.geometryLayout[shader]);
    c->IASetVertexBuffers(0, 1, &f.grid, &stride, &offset);
    c->IASetIndexBuffer(f.gridIndices, DXGI_FORMAT_R16_UINT, 0);
    c->VSSetShader(f.geometry[shader], nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &f.geometryConstants);
    c->PSSetShader(f.pixel[1], nullptr, 0);
    c->PSSetShaderResources(0, 1, &f.views[0]);
    c->PSSetSamplers(0, 1, &f.samplers[0]);
    c->OMSetBlendState(f.blends[0], nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(f.depths[0], 0);
    enum { REPEATS = 4 };
    double t0 = 0;
    for (int r = 0; r <= REPEATS; r++) {
        if (r == 1) t0 = Now();
        c->ClearRenderTargetView(f.targetView, black);
        c->ClearDepthStencilView(f.depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        for (int i = 0; i < times; i++) c->DrawIndexed(f.gridCount, 0, 0);
        f.Finish();
    }
    return (Now() - t0) / REPEATS / 1000.0;
}

void FillBenchmark() {
    Fill f;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    if (!MakeFill(f, &level)) {
        host_log("d3d fill: could not be set up");
        return;
    }
    static const Layers kinds[] = {
        {"one colour, no texture", 0, 0, 0, 0.1f, 0, 0, 0},
        {"a small texture and the fog (the renderer's shader without the alpha test)", 1, 0, 0, 0.1f, 0, 0, 0},
        {"the same with the alpha test", 2, 0, 0, 0.1f, 0, 0, 0},
        {"the same from an atlas, with the alpha test", 3, 0, 0, 0.1f, 0, 0, 0},
        {"blended over what is there", 1, 0, 0, 0.1f, 1, 0, 0},
        {"a compressed texture of 512x512, a texel to a pixel", 1, 1, 0, 1.0f, 0, 0, 0},
        {"that texture, 4 texels to a pixel (far away), no smaller copies of it", 1, 1, 0, 4.0f, 0, 0, 0},
        {"that texture, 4 texels to a pixel, with its smaller copies", 1, 2, 0, 4.0f, 0, 0, 0},
        {"that texture, 4 texels to a pixel, with its smaller copies blended into one another", 1, 2, 1, 4.0f, 0, 0, 0},
        {"that texture, 16 texels to a pixel, no smaller copies of it", 1, 1, 0, 16.0f, 0, 0, 0},
        {"that texture, 16 texels to a pixel, with its smaller copies", 1, 2, 0, 16.0f, 0, 0, 0},
        {"with the depth test, each layer in front of the one before", 1, 0, 0, 0.1f, 0, 1, 0},
        {"with the depth test, hidden behind the first layer", 1, 0, 0, 0.1f, 0, 1, 1},
        {"with the depth and the alpha test, each layer in front of the one before", 2, 0, 0, 0.1f, 0, 1, 0},
        {"with the depth and the alpha test, hidden behind the first layer", 2, 0, 0, 0.1f, 0, 1, 1},
        {"blended, depth tested and not written, hidden behind the first layer", 1, 0, 0, 0.1f, 1, 2, 1},
        {"from an atlas, without the alpha test", 4, 0, 0, 0.1f, 0, 0, 0},
        {"at coordinates the pixel shader works out but does not wrap, with the alpha test", 6, 0, 0, 0.1f, 0, 0, 0},
        {"from an atlas with the alpha test, the level by the slopes before the wrap (SampleGrad)", 5, 0, 0, 0.1f, 0, 0, 0},
        {"from an atlas with the alpha test, the compressed texture at 16 texels to a pixel, no smaller copies", 3, 1, 0, 16.0f, 0, 0, 0},
        {"from an atlas by the slopes before the wrap, that texture at 16 texels to a pixel with its smaller copies", 5, 2, 0, 16.0f, 0, 0, 0},
        {"the same, its smaller copies blended into one another", 5, 2, 1, 16.0f, 0, 0, 0},
        {"from an atlas by the slopes before the wrap, that texture at 4 texels to a pixel with its smaller copies", 5, 2, 0, 4.0f, 0, 0, 0},
    };
    static const struct {
        int width, height;
        DXGI_FORMAT depth;
        const char* name;
    } targets[] = {
        {1280, 720, DXGI_FORMAT_D24_UNORM_S8_UINT, "1280x720"},
        {768, 432, DXGI_FORMAT_D24_UNORM_S8_UINT, "768x432"},
        {1280, 720, DXGI_FORMAT_D16_UNORM, "1280x720 with a depth buffer of 16 bits"},
    };
    host_logf("d3d fill: Direct3D 11 at feature level %s; milliseconds until the GPU has drawn it", LevelName(level));
    for (const auto& t : targets) {
        if (!f.Target(t.width, t.height, t.depth)) {
            host_logf("d3d fill: no target of %s", t.name);
            continue;
        }
        Frame(f, kinds[0], 1);
        double nothing = Frame(f, kinds[0], 0);
        host_logf("d3d fill: %s: cleared and nothing drawn, %.2f ms", t.name, nothing);
        int count = (int)(sizeof(kinds) / sizeof(kinds[0]));
        if (&t != &targets[0]) count = 2;  // the other targets: what a layer costs there, no more
        for (int k = 0; k < count; k++) {
            if (!f.pixel[kinds[k].pixel]) {
                host_logf("d3d fill: %s: %s: no such shader on this GPU", t.name, kinds[k].name);
                continue;
            }
            double one = Frame(f, kinds[k], 1), five = Frame(f, kinds[k], 5);
            host_logf("d3d fill: %s: %s: one layer %.2f ms, each further one %.2f ms", t.name, kinds[k].name, one,
                      (five - one) / 4.0);
        }
        if (&t != &targets[0]) continue;
        static const char* shaders[3] = {"one matrix", "the renderer's three matrices, colour and fog", "that and eight lights, none lit"};
        for (int tiny = 1; tiny >= 0; tiny--)
            for (int s = 0; s < 3; s++) {
                double one = Grids(f, s, 1, tiny != 0), four = Grids(f, s, 4, tiny != 0);
                host_logf("d3d fill: %s: 32768 triangles of 16641 vertices %s, vertex shader with %s: the first %.2f ms, "
                          "each further time %.2f ms (%.2f ms for 100 thousand triangles)",
                          t.name, tiny ? "in a 64th of the picture's width and height" : "over the whole picture",
                          shaders[s], one, (four - one) / 3.0, (four - one) / 3.0 * 100000.0 / 32768.0);
            }
    }
}

}  // namespace

extern "C" void W10M_Direct3DBenchmark(void) {
    // REVC_BENCH=fill: what the GPU's own work costs, and nothing else
    const char* which = getenv("REVC_BENCH");
    if (which && strcmp(which, "fill") == 0) {
        FillBenchmark();
        return;
    }
    Capabilities();
    Run(0, "a device as Direct3D makes it");
    Run(D3D11_CREATE_DEVICE_SINGLETHREADED, "a device for one thread (no locks of its own)");
}
