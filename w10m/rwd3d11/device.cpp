// librw's render device on Direct3D 11: the device and what it shows, the render states, the
// camera, the frame's begin and end.
//
// This is the interface of librw's OpenGL renderer (rw::gl3, vendor/librw/src/gl/gl3device.cpp),
// which the game's own pipelines and effects are written against, with Direct3D 11 called
// behind it: no OpenGL ES and nothing that translates it. The window and the input are still
// GLFW's API (w10m/glfw), which here makes no context: the picture goes into a swap chain that
// the shell puts into its panel (host_set_swap_chain), or, without a shell (the tests on a PC),
// into a texture.
//
// What is kept as the OpenGL renderer has it, so that the game's textures (txd.img, converted
// once on each phone) and everything written against the interface stay as they are: a
// texture's rows are bottom-up and it is sampled at (u, 1-v). A texture that is drawn into is
// therefore drawn into upside down (flipTarget), and the picture is turned over when it is
// copied into one.
//
// Depth is Direct3D's, 0 to 1 (the device says so: zNear, zFar).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwrender.h"
#include "rwengine.h"
#include "rwpipeline.h"
#include "rwobjects.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3impl.h"

#include "d3d11impl.h"
#include "host_api.h"
#include "rwd3d11_host.h"

#define PLUGIN_ID 0

namespace rw {
namespace gl3 {

GlGlobals glGlobals;
D3d d3d;

Gl3Caps gl3Caps;
bool32 needToReadBackTextures;
// skin matrices go to the shaders as rows: always, here (shaders/skin_vs.hlsl)
bool32 bonesAsRows = 1;

int32   alphaFunc;
float32 alphaRef;

void
logf(const char *format, ...)
{
	char text[1024];
	va_list va;
	va_start(va, format);
	vsnprintf(text, sizeof(text), format, va);
	va_end(va);
	host_log(text);
}

struct UniformState
{
	float32 fogStart;
	float32 fogEnd;
	float32 fogRange;
	float32 fogDisable;

	RGBAf   fogColor;
};

struct UniformScene
{
	float32 proj[16];
	float32 view[16];
};

#define MAX_LIGHTS 8

struct UniformObject
{
	RawMatrix    world;
	RGBAf        ambLight;
	struct {
		float type;
		float radius;
		float minusCosAngle;
		float hardSpot;
	} lightParams[MAX_LIGHTS];
	V4d lightPosition[MAX_LIGHTS];
	V4d lightDirection[MAX_LIGHTS];
	RGBAf lightColor[MAX_LIGHTS];
};

static UniformState uniformState;
static UniformScene uniformScene;
static UniformObject uniformObject;

// State
int32 u_alphaRef;
int32 u_fogData;
int32 u_fogColor;

// Scene
int32 u_proj;
int32 u_view;

// Object
int32 u_world;
int32 u_ambLight;
int32 u_lightParams;
int32 u_lightPosition;
int32 u_lightDirection;
int32 u_lightColor;

int32 u_matColor;
int32 u_surfProps;

Shader *defaultShader, *defaultShader_noAT;
Shader *defaultShader_fullLight, *defaultShader_fullLight_noAT;
Shader *atlasShader, *atlasShader_fullLight;

struct RwRasterStateCache {
	Raster *raster;
	Texture::Addressing addressingU;
	Texture::Addressing addressingV;
	Texture::FilterMode filter;
};

// cached RW render states
struct RwStateCache {
	bool32 vertexAlpha;
	uint32 alphaTestEnable;
	uint32 alphaFunc;
	bool32 textureAlpha;
	bool32 blendEnable;
	uint32 srcblend, destblend;
	uint32 zwrite;
	uint32 ztest;
	uint32 cullmode;
	uint32 stencilenable;
	uint32 stencilpass;
	uint32 stencilfail;
	uint32 stencilzfail;
	uint32 stencilfunc;
	uint32 stencilref;
	uint32 stencilmask;
	uint32 stencilwritemask;
	uint32 fogEnable;
	float32 fogStart;
	float32 fogEnd;

	// emulation of PS2 GS
	bool32 gsalpha;
	uint32 gsalpharef;

	RwRasterStateCache texstage[MAXNUMSTAGES];
};
static RwStateCache rwStateCache;

enum
{
	RWGL_ALPHAFUNC,
	RWGL_ALPHAREF,
	RWGL_FOG,
	RWGL_FOGSTART,
	RWGL_FOGEND,
	RWGL_FOGCOLOR,

	RWGL_NUM_STATES
};
static bool uniformStateDirty[RWGL_NUM_STATES];

// ===[ Direct3D's state objects ]===
// A blend, a depth and stencil and a rasterizer state are each an object made once for every
// combination that is asked for, and what a draw is drawn with is three pointers.

// what is asked for, and what the device has
struct DeviceState
{
	// blending: with RenderWare's numbers, which are Direct3D's
	bool32 blendEnable;
	uint32 srcblend, destblend;
	// depth
	bool32 depthTest;
	bool32 depthAlways;	// written whatever is there (no test asked for, but writing)
	bool32 depthMask;
	// stencil
	bool32 stencilEnable;
	uint32 stencilFunc, stencilFail, stencilZFail, stencilPass;
	uint32 stencilRef, stencilMask, stencilWriteMask;
	// culling: CULLNONE, CULLBACK, CULLFRONT
	uint32 cullmode;
	bool32 scissor;
};
static DeviceState curState, oldState;
static bool32 blendDirty, depthDirty, rasterDirty;

// what is drawn into is a texture, kept bottom-up like every other: the picture goes into it
// upside down, and what faces the camera is wound the other way round
static bool32 flipTarget;

static ID3D11ShaderResourceView *boundResource[MAXNUMSTAGES];
static ID3D11SamplerState *boundSampler[MAXNUMSTAGES];
static uint32 stageDirty;	// a bit for each texture stage whose raster or sampling changed
static ID3D11RenderTargetView *boundTarget;
static ID3D11DepthStencilView *boundDepth;

static ID3D11BlendState *blendStates[2][12][12];
static ID3D11RasterizerState *rasterStates[3][2];
static ID3D11SamplerState *samplerStates[7][5][5];

struct DepthState
{
	uint32 key[3];
	ID3D11DepthStencilState *state;
};
static DepthState depthStates[64];
static int32 numDepthStates;

static D3D11_BLEND
alphaFactor(uint32 blend)
{
	switch(blend){
	case BLENDSRCCOLOR: return D3D11_BLEND_SRC_ALPHA;
	case BLENDINVSRCCOLOR: return D3D11_BLEND_INV_SRC_ALPHA;
	case BLENDDESTCOLOR: return D3D11_BLEND_DEST_ALPHA;
	case BLENDINVDESTCOLOR: return D3D11_BLEND_INV_DEST_ALPHA;
	default: return (D3D11_BLEND)blend;
	}
}

static ID3D11BlendState*
getBlendState(void)
{
	uint32 src = curState.blendEnable ? curState.srcblend : (uint32)BLENDONE;
	uint32 dst = curState.blendEnable ? curState.destblend : (uint32)BLENDZERO;
	if(src < 1 || src > 11) src = BLENDONE;
	if(dst < 1 || dst > 11) dst = BLENDZERO;
	ID3D11BlendState **state = &blendStates[curState.blendEnable ? 1 : 0][src][dst];
	if(*state == nil){
		D3D11_BLEND_DESC d;
		memset(&d, 0, sizeof(d));
		D3D11_RENDER_TARGET_BLEND_DESC *rt = &d.RenderTarget[0];
		rt->BlendEnable = curState.blendEnable ? TRUE : FALSE;
		rt->SrcBlend = (D3D11_BLEND)src;
		rt->DestBlend = (D3D11_BLEND)dst;
		rt->BlendOp = D3D11_BLEND_OP_ADD;
		rt->SrcBlendAlpha = alphaFactor(src);
		rt->DestBlendAlpha = alphaFactor(dst);
		rt->BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt->RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		HRESULT hr = d3d.device->CreateBlendState(&d, state);
		if(FAILED(hr))
			logf("d3d: no blend state for %u and %u, 0x%08X", (unsigned)src, (unsigned)dst, (unsigned)hr);
	}
	return *state;
}

static ID3D11DepthStencilState*
getDepthState(void)
{
	uint32 key[3];
	key[0] = (curState.depthTest ? 1 : 0) | (curState.depthAlways ? 2 : 0) | (curState.depthMask ? 4 : 0) |
		(curState.stencilEnable ? 8 : 0);
	key[1] = key[2] = 0;
	if(curState.stencilEnable){
		key[1] = curState.stencilFunc | curState.stencilFail<<4 | curState.stencilZFail<<8 | curState.stencilPass<<12;
		key[2] = (curState.stencilMask & 0xFF) | (curState.stencilWriteMask & 0xFF)<<8;
	}
	for(int32 i = 0; i < numDepthStates; i++)
		if(memcmp(depthStates[i].key, key, sizeof(key)) == 0)
			return depthStates[i].state;

	D3D11_DEPTH_STENCIL_DESC d;
	memset(&d, 0, sizeof(d));
	d.DepthEnable = curState.depthTest ? TRUE : FALSE;
	d.DepthWriteMask = curState.depthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	d.DepthFunc = curState.depthAlways ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_LESS_EQUAL;
	d.StencilEnable = curState.stencilEnable ? TRUE : FALSE;
	d.StencilReadMask = curState.stencilEnable ? (UINT8)curState.stencilMask : 0xFF;
	d.StencilWriteMask = curState.stencilEnable ? (UINT8)curState.stencilWriteMask : 0xFF;
	// RenderWare's numbers for the stencil are Direct3D's
	d.FrontFace.StencilFunc = curState.stencilEnable ? (D3D11_COMPARISON_FUNC)curState.stencilFunc : D3D11_COMPARISON_ALWAYS;
	d.FrontFace.StencilFailOp = curState.stencilEnable ? (D3D11_STENCIL_OP)curState.stencilFail : D3D11_STENCIL_OP_KEEP;
	d.FrontFace.StencilDepthFailOp = curState.stencilEnable ? (D3D11_STENCIL_OP)curState.stencilZFail : D3D11_STENCIL_OP_KEEP;
	d.FrontFace.StencilPassOp = curState.stencilEnable ? (D3D11_STENCIL_OP)curState.stencilPass : D3D11_STENCIL_OP_KEEP;
	d.BackFace = d.FrontFace;
	ID3D11DepthStencilState *state = nil;
	HRESULT hr = d3d.device->CreateDepthStencilState(&d, &state);
	if(FAILED(hr))
		logf("d3d: no depth state 0x%X, 0x%08X", (unsigned)key[0], (unsigned)hr);
	if(numDepthStates == (int32)nelem(depthStates)){
		// more than a game asks for: the oldest goes
		release(depthStates[0].state);
		memmove(&depthStates[0], &depthStates[1], (nelem(depthStates)-1)*sizeof(DepthState));
		numDepthStates--;
	}
	memcpy(depthStates[numDepthStates].key, key, sizeof(key));
	depthStates[numDepthStates].state = state;
	numDepthStates++;
	return state;
}

static ID3D11RasterizerState*
getRasterState(void)
{
	uint32 cull = curState.cullmode;
	// drawn upside down, a face is seen from its other side
	if(flipTarget && cull != CULLNONE)
		cull = cull == CULLBACK ? (uint32)CULLFRONT : (uint32)CULLBACK;
	int32 index = cull == CULLBACK ? 1 : cull == CULLFRONT ? 2 : 0;
	ID3D11RasterizerState **state = &rasterStates[index][curState.scissor ? 1 : 0];
	if(*state == nil){
		D3D11_RASTERIZER_DESC d;
		memset(&d, 0, sizeof(d));
		d.FillMode = D3D11_FILL_SOLID;
		d.CullMode = index == 1 ? D3D11_CULL_BACK : index == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
		d.FrontCounterClockwise = TRUE;	// as OpenGL has it
		d.DepthClipEnable = TRUE;
		d.ScissorEnable = curState.scissor ? TRUE : FALSE;
		HRESULT hr = d3d.device->CreateRasterizerState(&d, state);
		if(FAILED(hr))
			logf("d3d: no rasterizer state %d, 0x%08X", (int)index, (unsigned)hr);
	}
	return *state;
}

static ID3D11SamplerState*
getSamplerState(uint32 filter, uint32 addressU, uint32 addressV)
{
	static const D3D11_FILTER filters[7] = {
		D3D11_FILTER_MIN_MAG_MIP_POINT,
		D3D11_FILTER_MIN_MAG_MIP_POINT,		// NEAREST
		D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,	// LINEAR
		D3D11_FILTER_MIN_MAG_MIP_POINT,		// MIPNEAREST
		D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,	// MIPLINEAR
		D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,	// LINEARMIPNEAREST
		D3D11_FILTER_MIN_MAG_MIP_LINEAR,	// LINEARMIPLINEAR
	};
	static const D3D11_TEXTURE_ADDRESS_MODE addresses[5] = {
		D3D11_TEXTURE_ADDRESS_WRAP,
		D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
		D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,	// a border's colour is not asked for
	};
	if(filter > 6) filter = Texture::LINEAR;
	if(addressU > 4) addressU = Texture::WRAP;
	if(addressV > 4) addressV = Texture::WRAP;
	ID3D11SamplerState **state = &samplerStates[filter][addressU][addressV];
	if(*state == nil){
		D3D11_SAMPLER_DESC d;
		memset(&d, 0, sizeof(d));
		d.Filter = filters[filter];
		d.AddressU = addresses[addressU];
		d.AddressV = addresses[addressV];
		d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		d.MaxAnisotropy = 1;
		d.ComparisonFunc = D3D11_COMPARISON_NEVER;
		d.MaxLOD = D3D11_FLOAT32_MAX;
		HRESULT hr = d3d.device->CreateSamplerState(&d, state);
		if(FAILED(hr))
			logf("d3d: no sampler %u %u %u, 0x%08X", (unsigned)filter, (unsigned)addressU, (unsigned)addressV, (unsigned)hr);
	}
	return *state;
}

static void
releaseStateObjects(void)
{
	for(int32 a = 0; a < 2; a++)
		for(int32 b = 0; b < 12; b++)
			for(int32 c = 0; c < 12; c++)
				release(blendStates[a][b][c]);
	for(int32 a = 0; a < 3; a++)
		for(int32 b = 0; b < 2; b++)
			release(rasterStates[a][b]);
	for(int32 a = 0; a < 7; a++)
		for(int32 b = 0; b < 5; b++)
			for(int32 c = 0; c < 5; c++)
				release(samplerStates[a][b][c]);
	for(int32 i = 0; i < numDepthStates; i++)
		release(depthStates[i].state);
	numDepthStates = 0;
}

void
invalidateStates(void)
{
	blendDirty = depthDirty = rasterDirty = 1;
	stageDirty = (1u << MAXNUMSTAGES) - 1;
	for(int32 i = 0; i < MAXNUMSTAGES; i++){
		boundResource[i] = nil;
		boundSampler[i] = nil;
	}
	boundTarget = nil;
	boundDepth = nil;
	glGlobals.presentWidth = glGlobals.presentHeight = 0;
	invalidateVertexInput();
}

void
flushStates(void)
{
	if(blendDirty){
		blendDirty = 0;
		// with blending off the two factors count for nothing
		if(oldState.blendEnable != curState.blendEnable ||
		   (curState.blendEnable && (oldState.srcblend != curState.srcblend || oldState.destblend != curState.destblend))){
			oldState.blendEnable = curState.blendEnable;
			oldState.srcblend = curState.srcblend;
			oldState.destblend = curState.destblend;
			d3d.context->OMSetBlendState(getBlendState(), nil, 0xFFFFFFFF);
			perf.blends++;
		}
	}
	if(depthDirty){
		depthDirty = 0;
		d3d.context->OMSetDepthStencilState(getDepthState(), curState.stencilRef);
		perf.depths++;
	}
	if(rasterDirty){
		rasterDirty = 0;
		d3d.context->RSSetState(getRasterState());
		perf.culls++;
	}
	if(stageDirty){
		for(int32 i = 0; i < MAXNUMSTAGES; i++){
			if((stageDirty & (1u << i)) == 0)
				continue;
			RwRasterStateCache *stage = &rwStateCache.texstage[i];
			ID3D11ShaderResourceView *resource = whiteTexture;
			uint32 addressU = stage->addressingU, addressV = stage->addressingV;
			if(stage->raster){
				Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, stage->raster, nativeRasterOffset);
				if(natras->d3dResource)
					resource = (ID3D11ShaderResourceView*)natras->d3dResource;
				if(natras->d3dClamp)
					addressU = addressV = Texture::CLAMP;
			}
			if(resource != boundResource[i]){
				boundResource[i] = resource;
				d3d.context->PSSetShaderResources(i, 1, &resource);
				perf.textures++;
				perf.changed |= 1;
				if(perf.enabled){
					static uint32 seenInFrame[4096];
					uint32 slot = (uint32)((uintptr)resource >> 4) & 4095;
					if(seenInFrame[slot] != perf.frame + 1){
						seenInFrame[slot] = perf.frame + 1;
						perf.differentTextures++;
					}
				}
			}
			ID3D11SamplerState *sampler = getSamplerState(stage->filter, addressU, addressV);
			if(sampler != boundSampler[i]){
				boundSampler[i] = sampler;
				d3d.context->PSSetSamplers(i, 1, &sampler);
				perf.samplers++;
			}
		}
		stageDirty = 0;
	}
}

// ===[ RenderWare's render states ]===

void
setAlphaBlend(bool32 enable)
{
	if(rwStateCache.blendEnable != enable){
		rwStateCache.blendEnable = enable;
		curState.blendEnable = enable;
		blendDirty = 1;
	}
}

bool32
getAlphaBlend(void)
{
	return rwStateCache.blendEnable;
}

bool32 getAlphaTest(void) { return rwStateCache.alphaTestEnable; }

static void
setDepthTest(bool32 enable)
{
	if(rwStateCache.ztest != enable){
		rwStateCache.ztest = enable;
		if(rwStateCache.zwrite && !enable){
			// If we still want to write, enable but set mode to always
			curState.depthTest = 1;
			curState.depthAlways = 1;
		}else{
			curState.depthTest = rwStateCache.ztest;
			curState.depthAlways = 0;
		}
		depthDirty = 1;
	}
}

static void
setDepthWrite(bool32 enable)
{
	enable = enable ? 1 : 0;
	if(rwStateCache.zwrite != (uint32)enable){
		rwStateCache.zwrite = enable;
		if(enable && !rwStateCache.ztest){
			// Have to switch on ztest so writing can work
			curState.depthTest = 1;
			curState.depthAlways = 1;
		}
		curState.depthMask = rwStateCache.zwrite;
		depthDirty = 1;
	}
}

static void
setAlphaTest(bool32 enable)
{
	uint32 shaderfunc;
	if(rwStateCache.alphaTestEnable != (uint32)enable){
		rwStateCache.alphaTestEnable = enable;
		shaderfunc = rwStateCache.alphaTestEnable ? rwStateCache.alphaFunc : ALPHAALWAYS;
		if(alphaFunc != (int32)shaderfunc){
			alphaFunc = shaderfunc;
			uniformStateDirty[RWGL_ALPHAFUNC] = true;
		}
	}
}

static void
setAlphaTestFunction(uint32 function)
{
	uint32 shaderfunc;
	if(rwStateCache.alphaFunc != function){
		rwStateCache.alphaFunc = function;
		shaderfunc = rwStateCache.alphaTestEnable ? rwStateCache.alphaFunc : ALPHAALWAYS;
		if(alphaFunc != (int32)shaderfunc){
			alphaFunc = shaderfunc;
			uniformStateDirty[RWGL_ALPHAFUNC] = true;
		}
	}
}

static void
setVertexAlpha(bool32 enable)
{
	if(rwStateCache.vertexAlpha != enable){
		if(!rwStateCache.textureAlpha){
			setAlphaBlend(enable);
			setAlphaTest(enable);
		}
		rwStateCache.vertexAlpha = enable;
	}
}

static void
setFilterMode(uint32 stage, int32 filter)
{
	if(rwStateCache.texstage[stage].filter != (Texture::FilterMode)filter){
		rwStateCache.texstage[stage].filter = (Texture::FilterMode)filter;
		stageDirty |= 1u << stage;
	}
}

static void
setAddressU(uint32 stage, int32 addressing)
{
	if(rwStateCache.texstage[stage].addressingU != (Texture::Addressing)addressing){
		rwStateCache.texstage[stage].addressingU = (Texture::Addressing)addressing;
		stageDirty |= 1u << stage;
	}
}

static void
setAddressV(uint32 stage, int32 addressing)
{
	if(rwStateCache.texstage[stage].addressingV != (Texture::Addressing)addressing){
		rwStateCache.texstage[stage].addressingV = (Texture::Addressing)addressing;
		stageDirty |= 1u << stage;
	}
}

static void
setRasterStage(uint32 stage, Raster *raster)
{
	bool32 alpha;
	if(raster != rwStateCache.texstage[stage].raster){
		rwStateCache.texstage[stage].raster = raster;
		stageDirty |= 1u << stage;
		if(raster){
			assert(raster->platform == PLATFORM_GL3);
			Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, raster, nativeRasterOffset);
			alpha = natras->hasAlpha;
		}else
			alpha = 0;

		if(stage == 0){
			if(alpha != rwStateCache.textureAlpha){
				rwStateCache.textureAlpha = alpha;
				if(!rwStateCache.vertexAlpha){
					setAlphaBlend(alpha);
					setAlphaTest(alpha);
				}
			}
		}
	}
}

void
evictRaster(Raster *raster)
{
	int i;
	for(i = 0; i < MAXNUMSTAGES; i++){
		if(rwStateCache.texstage[i].raster != raster)
			continue;
		setRasterStage(i, nil);
		// and off the device now: the texture may be gone, or about to be drawn into
		if(d3d.context && boundResource[i] != whiteTexture){
			boundResource[i] = whiteTexture;
			d3d.context->PSSetShaderResources(i, 1, &whiteTexture);
		}
	}
}

void
setTexture(int32 stage, Texture *tex)
{
	if(tex == nil || tex->raster == nil){
		setRasterStage(stage, nil);
		return;
	}
	setRasterStage(stage, tex->raster);
	setFilterMode(stage, tex->getFilter());
	setAddressU(stage, tex->getAddressU());
	setAddressV(stage, tex->getAddressV());
}

static void
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	switch(state){
	case TEXTURERASTER:
		setRasterStage(0, (Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		setAddressU(0, value);
		setAddressV(0, value);
		break;
	case TEXTUREADDRESSU:
		setAddressU(0, value);
		break;
	case TEXTUREADDRESSV:
		setAddressV(0, value);
		break;
	case TEXTUREFILTER:
		setFilterMode(0, value);
		break;
	case VERTEXALPHA:
		setVertexAlpha(value);
		break;
	case SRCBLEND:
		if(rwStateCache.srcblend != value){
			rwStateCache.srcblend = value;
			curState.srcblend = value;
			blendDirty = 1;
		}
		break;
	case DESTBLEND:
		if(rwStateCache.destblend != value){
			rwStateCache.destblend = value;
			curState.destblend = value;
			blendDirty = 1;
		}
		break;
	case ZTESTENABLE:
		setDepthTest(value);
		break;
	case ZWRITEENABLE:
		setDepthWrite(value);
		break;
	case FOGENABLE:
		if(rwStateCache.fogEnable != value){
			rwStateCache.fogEnable = value;
			uniformStateDirty[RWGL_FOG] = true;
		}
		break;
	case FOGCOLOR:
		// no cache check here...too lazy
		RGBA c;
		c.red = value;
		c.green = value>>8;
		c.blue = value>>16;
		c.alpha = value>>24;
		convColor(&uniformState.fogColor, &c);
		uniformStateDirty[RWGL_FOGCOLOR] = true;
		break;
	case CULLMODE:
		if(rwStateCache.cullmode != value){
			rwStateCache.cullmode = value;
			curState.cullmode = value;
			rasterDirty = 1;
		}
		break;

	case STENCILENABLE:
		if(rwStateCache.stencilenable != value){
			rwStateCache.stencilenable = value;
			curState.stencilEnable = value;
			depthDirty = 1;
		}
		break;
	case STENCILFAIL:
		if(rwStateCache.stencilfail != value){
			rwStateCache.stencilfail = value;
			curState.stencilFail = value;
			depthDirty = 1;
		}
		break;
	case STENCILZFAIL:
		if(rwStateCache.stencilzfail != value){
			rwStateCache.stencilzfail = value;
			curState.stencilZFail = value;
			depthDirty = 1;
		}
		break;
	case STENCILPASS:
		if(rwStateCache.stencilpass != value){
			rwStateCache.stencilpass = value;
			curState.stencilPass = value;
			depthDirty = 1;
		}
		break;
	case STENCILFUNCTION:
		if(rwStateCache.stencilfunc != value){
			rwStateCache.stencilfunc = value;
			curState.stencilFunc = value;
			depthDirty = 1;
		}
		break;
	case STENCILFUNCTIONREF:
		if(rwStateCache.stencilref != value){
			rwStateCache.stencilref = value;
			curState.stencilRef = value;
			depthDirty = 1;
		}
		break;
	case STENCILFUNCTIONMASK:
		if(rwStateCache.stencilmask != value){
			rwStateCache.stencilmask = value;
			curState.stencilMask = value;
			depthDirty = 1;
		}
		break;
	case STENCILFUNCTIONWRITEMASK:
		if(rwStateCache.stencilwritemask != value){
			rwStateCache.stencilwritemask = value;
			curState.stencilWriteMask = value;
			depthDirty = 1;
		}
		break;

	case ALPHATESTFUNC:
		setAlphaTestFunction(value);
		break;
	case ALPHATESTREF:
		if(alphaRef != value/255.0f){
			alphaRef = value/255.0f;
			uniformStateDirty[RWGL_ALPHAREF] = true;
		}
		break;
	case GSALPHATEST:
		rwStateCache.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rwStateCache.gsalpharef = value;
	}
}

static void*
getRenderState(int32 state)
{
	uint32 val;
	RGBA rgba;
	switch(state){
	case TEXTURERASTER:
		return rwStateCache.texstage[0].raster;
	case TEXTUREADDRESS:
		if(rwStateCache.texstage[0].addressingU == rwStateCache.texstage[0].addressingV)
			val = rwStateCache.texstage[0].addressingU;
		else
			val = 0;	// invalid
		break;
	case TEXTUREADDRESSU:
		val = rwStateCache.texstage[0].addressingU;
		break;
	case TEXTUREADDRESSV:
		val = rwStateCache.texstage[0].addressingV;
		break;
	case TEXTUREFILTER:
		val = rwStateCache.texstage[0].filter;
		break;

	case VERTEXALPHA:
		val = rwStateCache.vertexAlpha;
		break;
	case SRCBLEND:
		val = rwStateCache.srcblend;
		break;
	case DESTBLEND:
		val = rwStateCache.destblend;
		break;
	case ZTESTENABLE:
		val = rwStateCache.ztest;
		break;
	case ZWRITEENABLE:
		val = rwStateCache.zwrite;
		break;
	case FOGENABLE:
		val = rwStateCache.fogEnable;
		break;
	case FOGCOLOR:
		convColor(&rgba, &uniformState.fogColor);
		val = RWRGBAINT(rgba.red, rgba.green, rgba.blue, rgba.alpha);
		break;
	case CULLMODE:
		val = rwStateCache.cullmode;
		break;

	case STENCILENABLE:
		val = rwStateCache.stencilenable;
		break;
	case STENCILFAIL:
		val = rwStateCache.stencilfail;
		break;
	case STENCILZFAIL:
		val = rwStateCache.stencilzfail;
		break;
	case STENCILPASS:
		val = rwStateCache.stencilpass;
		break;
	case STENCILFUNCTION:
		val = rwStateCache.stencilfunc;
		break;
	case STENCILFUNCTIONREF:
		val = rwStateCache.stencilref;
		break;
	case STENCILFUNCTIONMASK:
		val = rwStateCache.stencilmask;
		break;
	case STENCILFUNCTIONWRITEMASK:
		val = rwStateCache.stencilwritemask;
		break;

	case ALPHATESTFUNC:
		val = rwStateCache.alphaFunc;
		break;
	case ALPHATESTREF:
		val = (uint32)(alphaRef*255.0f);
		break;
	case GSALPHATEST:
		val = rwStateCache.gsalpha;
		break;
	case GSALPHATESTREF:
		val = rwStateCache.gsalpharef;
		break;
	default:
		val = 0;
	}
	return (void*)(uintptr)val;
}

static void
resetRenderState(void)
{
	rwStateCache.alphaFunc = ALPHAGREATEREQUAL;
	alphaFunc = 0;
	alphaRef = 10.0f/255.0f;
	uniformStateDirty[RWGL_ALPHAREF] = true;
	uniformState.fogDisable = 1.0f;
	uniformState.fogStart = 0.0f;
	uniformState.fogEnd = 0.0f;
	uniformState.fogRange = 0.0f;
	uniformState.fogColor.red = uniformState.fogColor.green = uniformState.fogColor.blue = uniformState.fogColor.alpha = 1.0f;
	uniformStateDirty[RWGL_FOG] = true;
	uniformStateDirty[RWGL_FOGCOLOR] = true;
	rwStateCache.gsalpha = 0;
	rwStateCache.gsalpharef = 128;

	rwStateCache.vertexAlpha = 0;
	rwStateCache.textureAlpha = 0;
	rwStateCache.alphaTestEnable = 0;

	memset(&curState, 0, sizeof(curState));
	memset(&oldState, 0xFE, sizeof(oldState));

	rwStateCache.blendEnable = 0;
	curState.blendEnable = 0;
	rwStateCache.srcblend = BLENDSRCALPHA;
	rwStateCache.destblend = BLENDINVSRCALPHA;
	curState.srcblend = rwStateCache.srcblend;
	curState.destblend = rwStateCache.destblend;

	rwStateCache.zwrite = 1;
	curState.depthMask = 1;
	rwStateCache.ztest = 1;
	curState.depthTest = 1;
	curState.depthAlways = 0;

	rwStateCache.cullmode = CULLNONE;
	curState.cullmode = CULLNONE;

	rwStateCache.stencilenable = 0;
	curState.stencilEnable = 0;
	rwStateCache.stencilfail = STENCILKEEP;
	curState.stencilFail = STENCILKEEP;
	rwStateCache.stencilzfail = STENCILKEEP;
	curState.stencilZFail = STENCILKEEP;
	rwStateCache.stencilpass = STENCILKEEP;
	curState.stencilPass = STENCILKEEP;
	rwStateCache.stencilfunc = STENCILALWAYS;
	curState.stencilFunc = STENCILALWAYS;
	rwStateCache.stencilref = 0;
	curState.stencilRef = 0;
	rwStateCache.stencilmask = 0xFFFFFFFF;
	curState.stencilMask = 0xFFFFFFFF;
	rwStateCache.stencilwritemask = 0xFFFFFFFF;
	curState.stencilWriteMask = 0xFFFFFFFF;

	for(int i = 0; i < MAXNUMSTAGES; i++){
		rwStateCache.texstage[i].raster = nil;
		rwStateCache.texstage[i].filter = Texture::NEAREST;
		rwStateCache.texstage[i].addressingU = Texture::WRAP;
		rwStateCache.texstage[i].addressingV = Texture::WRAP;
	}
	invalidateStates();
}

// ===[ Uniforms that are the device's ]===

void
setWorldMatrix(Matrix *mat)
{
	convMatrix(&uniformObject.world, mat);
	setUniform(u_world, &uniformObject.world);
}

int32
setLights(WorldLights *lightData)
{
	int i, n;
	Light *l;
	int32 bits;

	uniformObject.ambLight = lightData->ambient;

	bits = 0;

	if(lightData->numAmbients)
		bits |= VSLIGHT_AMBIENT;

	n = 0;
	for(i = 0; i < lightData->numDirectionals && n < MAX_LIGHTS; i++){
		l = lightData->directionals[i];
		uniformObject.lightParams[n].type = 1.0f;
		uniformObject.lightColor[n] = l->color;
		memcpy(&uniformObject.lightDirection[n], &l->getFrame()->getLTM()->at, sizeof(V3d));
		bits |= VSLIGHT_DIRECT;
		n++;
	}

	for(i = 0; i < lightData->numLocals && n < MAX_LIGHTS; i++){
		l = lightData->locals[i];

		switch(l->getType()){
		case Light::POINT:
			uniformObject.lightParams[n].type = 2.0f;
			uniformObject.lightParams[n].radius = l->radius;
			uniformObject.lightColor[n] = l->color;
			memcpy(&uniformObject.lightPosition[n], &l->getFrame()->getLTM()->pos, sizeof(V3d));
			bits |= VSLIGHT_POINT;
			n++;
			break;
		case Light::SPOT:
		case Light::SOFTSPOT:
			uniformObject.lightParams[n].type = 3.0f;
			uniformObject.lightParams[n].minusCosAngle = l->minusCosAngle;
			uniformObject.lightParams[n].radius = l->radius;
			uniformObject.lightColor[n] = l->color;
			memcpy(&uniformObject.lightPosition[n], &l->getFrame()->getLTM()->pos, sizeof(V3d));
			memcpy(&uniformObject.lightDirection[n], &l->getFrame()->getLTM()->at, sizeof(V3d));
			// lower bound of falloff
			if(l->getType() == Light::SOFTSPOT)
				uniformObject.lightParams[n].hardSpot = 0.0f;
			else
				uniformObject.lightParams[n].hardSpot = 1.0f;
			bits |= VSLIGHT_SPOT;
			n++;
			break;
		}
	}

	// the shaders go through all eight: those that are not there are of no type
	for(; n < MAX_LIGHTS; n++)
		uniformObject.lightParams[n].type = 0.0f;

	setUniform(u_ambLight, &uniformObject.ambLight);
	// which lights there are, always: a shader that counts lights may draw what has none.
	// The rest only when there are some.
	setUniform(u_lightParams, uniformObject.lightParams);
	if(bits & VSLIGHT_MASK){
		setUniform(u_lightPosition, uniformObject.lightPosition);
		setUniform(u_lightDirection, uniformObject.lightDirection);
		setUniform(u_lightColor, uniformObject.lightColor);
	}
	return bits;
}

void
setProjectionMatrix(float32 *mat)
{
	memcpy(&uniformScene.proj, mat, 64);
	if(flipTarget){
		// y turned over: the second row of the matrix, which is every column's second number
		uniformScene.proj[1] = -uniformScene.proj[1];
		uniformScene.proj[5] = -uniformScene.proj[5];
		uniformScene.proj[9] = -uniformScene.proj[9];
		uniformScene.proj[13] = -uniformScene.proj[13];
	}
	setUniform(u_proj, uniformScene.proj);
}

void
setViewMatrix(float32 *mat)
{
	memcpy(&uniformScene.view, mat, 64);
	setUniform(u_view, uniformScene.view);
}

void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	rw::RGBAf col;
	convColor(&col, &color);
	setUniform(u_matColor, &col);

	float surfProps[4];
	surfProps[0] = surfaceprops.ambient;
	surfProps[1] = surfaceprops.specular;
	surfProps[2] = surfaceprops.diffuse;
	surfProps[3] = extraSurfProp;
	setUniform(u_surfProps, surfProps);
}

void
im2DSetXform(void)
{
	float xform[4];
	Camera *cam;
	cam = (Camera*)engine->currentCamera;
	xform[0] = 2.0f/cam->frameBuffer->width;
	xform[1] = -2.0f/cam->frameBuffer->height;
	xform[2] = -1.0f;
	xform[3] = 1.0f;
	if(flipTarget){
		xform[1] = -xform[1];
		xform[3] = -xform[3];
	}
	static int32 u_xform = -1;
	if(u_xform < 0)
		u_xform = registerUniform("u_xform", UNIFORM_VEC4);
	setUniform(u_xform, xform);
}

void
flushCache(void)
{
	flushStates();

	uniformState.fogDisable = rwStateCache.fogEnable ? 0.0f : 1.0f;
	uniformState.fogStart = rwStateCache.fogStart;
	uniformState.fogEnd = rwStateCache.fogEnd;
	uniformState.fogRange = 1.0f/(rwStateCache.fogStart - rwStateCache.fogEnd);

	if(uniformStateDirty[RWGL_ALPHAFUNC] || uniformStateDirty[RWGL_ALPHAREF]){
		float alphaTest[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		switch(alphaFunc){
		case ALPHAALWAYS:
		default:
			alphaTest[0] = -1000.0f;
			alphaTest[1] = 1000.0f;
			break;
		case ALPHAGREATEREQUAL:
			alphaTest[0] = alphaRef;
			alphaTest[1] = 1000.0f;
			break;
		case ALPHALESS:
			alphaTest[0] = -1000.0f;
			alphaTest[1] = alphaRef;
			break;
		}
		setUniform(u_alphaRef, alphaTest);
		uniformStateDirty[RWGL_ALPHAFUNC] = false;
		uniformStateDirty[RWGL_ALPHAREF] = false;
	}

	if(uniformStateDirty[RWGL_FOG] ||
	   uniformStateDirty[RWGL_FOGSTART] ||
	   uniformStateDirty[RWGL_FOGEND]){
		float fogData[4] = {
			uniformState.fogStart,
			uniformState.fogEnd,
			uniformState.fogRange,
			uniformState.fogDisable
		};
		setUniform(u_fogData, fogData);
		uniformStateDirty[RWGL_FOG] = false;
		uniformStateDirty[RWGL_FOGSTART] = false;
		uniformStateDirty[RWGL_FOGEND] = false;
	}

	if(uniformStateDirty[RWGL_FOGCOLOR]){
		setUniform(u_fogColor, &uniformState.fogColor);
		uniformStateDirty[RWGL_FOGCOLOR] = false;
	}

	flushUniforms();
}

// what the OpenGL renderer's callers know of it and this one has no use for
void bindFramebuffer(uint32) {}
uint32 bindTexture(uint32) { return 0; }

// ===[ The camera ]===

// What a camera draws into: the picture, or a texture; with its depth buffer
static void
setFrameBuffer(Camera *cam)
{
	Raster *fbuf = cam->frameBuffer->parent;
	Raster *zbuf = cam->zBuffer ? cam->zBuffer->parent : nil;
	assert(fbuf);
	assert(fbuf->type == Raster::CAMERA || fbuf->type == Raster::CAMERATEXTURE);

	Gl3Raster *natfb = PLUGINOFFSET(Gl3Raster, fbuf, nativeRasterOffset);
	ID3D11RenderTargetView *target = fbuf->type == Raster::CAMERA ? d3d.backBufferTarget :
		(ID3D11RenderTargetView*)natfb->d3dTarget;
	ID3D11DepthStencilView *depth = nil;
	if(zbuf){
		Gl3Raster *natzb = PLUGINOFFSET(Gl3Raster, zbuf, nativeRasterOffset);
		depth = (ID3D11DepthStencilView*)natzb->d3dDepth;
		// Direct3D wants the two of one size
		int32 w = fbuf->type == Raster::CAMERA ? d3d.width : fbuf->width;
		int32 h = fbuf->type == Raster::CAMERA ? d3d.height : fbuf->height;
		if(zbuf->width != w || zbuf->height != h){
			static bool32 said;
			if(!said){
				said = 1;
				logf("d3d: a depth buffer of %dx%d for a picture of %dx%d: drawn without it",
					(int)zbuf->width, (int)zbuf->height, (int)w, (int)h);
			}
			depth = nil;
		}
	}
	bool32 flip = fbuf->type == Raster::CAMERATEXTURE;
	if(flip != flipTarget){
		flipTarget = flip;
		rasterDirty = 1;
	}
	if(target != boundTarget || depth != boundDepth){
		// what is drawn into cannot be sampled at the same time
		if(fbuf->type == Raster::CAMERATEXTURE)
			evictRaster(fbuf);
		boundTarget = target;
		boundDepth = depth;
		d3d.context->OMSetRenderTargets(1, &target, depth);
	}
}

static Rect
getFramebufferRect(Raster *frameBuffer)
{
	Rect r;
	Raster *fb = frameBuffer->parent;
	if(fb->type == Raster::CAMERA){
		r.w = d3d.width;
		r.h = d3d.height;
	}else{
		r.w = fb->width;
		r.h = fb->height;
	}
	r.x = 0;
	r.y = 0;

	// Got a subraster
	if(frameBuffer != fb){
		r.x = frameBuffer->offsetX;
		r.y = frameBuffer->offsetY;
		// a texture is drawn into upside down
		if(fb->type == Raster::CAMERATEXTURE)
			r.y = r.h - frameBuffer->height - frameBuffer->offsetY;
		r.w = frameBuffer->width;
		r.h = frameBuffer->height;
	}

	return r;
}

static void
setViewport(Raster *frameBuffer)
{
	Rect r = getFramebufferRect(frameBuffer);
	if(r.w != glGlobals.presentWidth || r.h != glGlobals.presentHeight ||
	   r.x != glGlobals.presentOffX || r.y != glGlobals.presentOffY){
		D3D11_VIEWPORT viewport;
		viewport.TopLeftX = (float)r.x;
		viewport.TopLeftY = (float)r.y;
		viewport.Width = (float)r.w;
		viewport.Height = (float)r.h;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		d3d.context->RSSetViewports(1, &viewport);
		glGlobals.presentWidth = r.w;
		glGlobals.presentHeight = r.h;
		glGlobals.presentOffX = r.x;
		glGlobals.presentOffY = r.y;
	}
}

static void
beginUpdate(Camera *cam)
{
	float view[16], proj[16];
	// View Matrix
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	// Since we're looking into positive Z,
	// flip X to ge a left handed view space.
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  =  -inv.at.x;
	view[9]  =   inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, &view, sizeof(RawMatrix));

	// Projection Matrix: depth from 0 to 1
	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane-cam->nearPlane);

	proj[0] = invwx;
	proj[1] = 0.0f;
	proj[2] = 0.0f;
	proj[3] = 0.0f;

	proj[4] = 0.0f;
	proj[5] = invwy;
	proj[6] = 0.0f;
	proj[7] = 0.0f;

	proj[8] = cam->viewOffset.x*invwx;
	proj[9] = cam->viewOffset.y*invwy;
	proj[12] = -proj[8];
	proj[13] = -proj[9];
	if(cam->projection == Camera::PERSPECTIVE){
		proj[10] = cam->farPlane*invz;
		proj[11] = 1.0f;

		proj[15] = 0.0f;
	}else{
		proj[10] = invz;
		proj[11] = 0.0f;

		proj[15] = 1.0f;
	}
	proj[14] = -cam->nearPlane*proj[10];
	memcpy(&cam->devProj, &proj, sizeof(RawMatrix));

	if(rwStateCache.fogStart != cam->fogPlane){
		rwStateCache.fogStart = cam->fogPlane;
		uniformStateDirty[RWGL_FOGSTART] = true;
	}
	if(rwStateCache.fogEnd != cam->farPlane){
		rwStateCache.fogEnd = cam->farPlane;
		uniformStateDirty[RWGL_FOGEND] = true;
	}

	// before the matrices: whether the picture is upside down is the target's
	setFrameBuffer(cam);

	setViewMatrix(view);
	setProjectionMatrix(proj);

	setViewport(cam->frameBuffer);
}

static void
endUpdate(Camera *cam)
{
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	RGBAf colf;

	setFrameBuffer(cam);

	convColor(&colf, col);
	bool32 part = cam->frameBuffer != cam->frameBuffer->parent;
	if((mode & Camera::CLEARIMAGE) && boundTarget){
		if(part){
			// Direct3D clears all of a target or none of it: a part of the picture is
			// painted over
			static bool32 said;
			if(!said){
				said = 1;
				logf("d3d: a part of the picture was to be cleared: left as it is");
			}
		}else
			d3d.context->ClearRenderTargetView(boundTarget, (float*)&colf);
	}
	uint32 flags = 0;
	if(mode & Camera::CLEARZ)
		flags |= D3D11_CLEAR_DEPTH;
	if(mode & Camera::CLEARSTENCIL)
		flags |= D3D11_CLEAR_STENCIL;
	if(flags && boundDepth)
		d3d.context->ClearDepthStencilView(boundDepth, flags, 1.0f, 0);
}

static void
showRaster(Raster *raster, uint32 flags)
{
	if(flags & Raster::FLIPWAITVSYNCH)
		glfwSwapInterval(1);
	else
		glfwSwapInterval(0);
	glfwSwapBuffers(glGlobals.window);
}

// The picture so far into a texture (the colour filters and the drops on the screen ask for
// it): drawn into it turned over, since a texture is kept bottom-up. Where the back buffer
// cannot be sampled, by way of a copy that can.
static ID3D11Texture2D *copySource;
static ID3D11ShaderResourceView *copySourceResource;

bool32
copyBackBuffer(Raster *dst, int32 x, int32 y)
{
	Gl3Raster *natdst = PLUGINOFFSET(Gl3Raster, dst, nativeRasterOffset);
	ID3D11RenderTargetView *target = (ID3D11RenderTargetView*)natdst->d3dTarget;
	if(target == nil || d3d.backBuffer == nil)
		return 0;
	ID3D11ShaderResourceView *source = d3d.backBufferResource;
	if(source == nil){
		if(copySource == nil){
			D3D11_TEXTURE2D_DESC td;
			d3d.backBuffer->GetDesc(&td);
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = 0;
			td.MiscFlags = 0;
			if(FAILED(d3d.device->CreateTexture2D(&td, nil, &copySource)) ||
			   FAILED(d3d.device->CreateShaderResourceView(copySource, nil, &copySourceResource))){
				release(copySource);
				return 0;
			}
		}
		d3d.context->CopyResource(copySource, d3d.backBuffer);
		source = copySourceResource;
	}

	// a quad over all of the texture, with what the immediate mode draws its own with
	Shader *before = currentShader;
	DeviceState stateBefore = curState;
	bool32 flipBefore = flipTarget;
	evictRaster(dst);
	d3d.context->OMSetRenderTargets(1, &target, nil);	// which takes the back buffer off: it is sampled now
	// The picture goes where OpenGL's copy puts it: x across, and its top row y rows below the
	// texture's top, which is where the texture's rows end
	D3D11_VIEWPORT viewport = { (float)x, (float)(dst->height - d3d.height - y), (float)d3d.width, (float)d3d.height,
		0.0f, 1.0f };
	d3d.context->RSSetViewports(1, &viewport);
	memset(&curState, 0, sizeof(curState));	// no blending, no depth, no culling
	curState.cullmode = CULLNONE;
	flipTarget = 0;
	blendDirty = depthDirty = rasterDirty = 1;
	memset(&oldState, 0xFE, sizeof(oldState));
	stageDirty = 0;
	flushStates();
	ID3D11SamplerState *sampler = getSamplerState(Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	d3d.context->PSSetShaderResources(0, 1, &source);
	d3d.context->PSSetSamplers(0, 1, &sampler);

	static Shader *copyShader;
	if(copyShader == nil)
		copyShader = Shader::create("im2d", "simple_noAT");
	if(copyShader == nil)
		return 0;
	copyShader->use();
	// x and y of the corners straight into clip space; the fog off
	static int32 u_xform = -1;
	if(u_xform < 0)
		u_xform = registerUniform("u_xform", UNIFORM_VEC4);
	float xform[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
	float fogData[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	setUniform(u_xform, xform);
	setUniform(u_fogData, fogData);
	uniformStateDirty[RWGL_FOG] = true;
	flushUniforms();

	// The picture's bottom row is to be the first of its rows in the texture, which is the
	// top of what is drawn into here: the corners at the top sample the picture's bottom.
	Im2DVertex verts[4];
	static const float corners[4][4] = {
		// clip x, clip y, u, v as handed to the shader (it samples at 1-v)
		{ -1.0f,  1.0f, 0.0f, 0.0f },
		{  1.0f,  1.0f, 1.0f, 0.0f },
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f, -1.0f, 1.0f, 1.0f },
	};
	for(int32 i = 0; i < 4; i++){
		verts[i].x = corners[i][0];
		verts[i].y = corners[i][1];
		verts[i].z = 0.0f;
		verts[i].w = 1.0f;
		verts[i].r = verts[i].g = verts[i].b = verts[i].a = 255;
		verts[i].u = corners[i][2];
		verts[i].v = corners[i][3];
	}
	im2DDrawStrip(verts, 4);
	ID3D11ShaderResourceView *noResource = nil;
	d3d.context->PSSetShaderResources(0, 1, &noResource);	// the back buffer is to be drawn into again

	// back to what the game was drawing into, with everything set again
	curState = stateBefore;
	flipTarget = flipBefore;
	invalidateStates();
	memset(&oldState, 0xFE, sizeof(oldState));
	if(engine->currentCamera){
		Camera *cam = (Camera*)engine->currentCamera;
		setFrameBuffer(cam);
		setViewport(cam->frameBuffer);
	}
	if(before)
		before->use();
	return 1;
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	Raster *src = raster;
	Raster *dst = Raster::getCurrentContext();

	switch(dst->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		switch(src->type){
		case Raster::CAMERA:
			return copyBackBuffer(dst, x, y);
		}
		break;
	}
	return 0;
}

// ===[ The device and what it shows ]===

static const char*
levelName(D3D_FEATURE_LEVEL level)
{
	switch(level){
	case D3D_FEATURE_LEVEL_9_1: return "9_1";
	case D3D_FEATURE_LEVEL_9_2: return "9_2";
	case D3D_FEATURE_LEVEL_9_3: return "9_3";
	case D3D_FEATURE_LEVEL_10_0: return "10_0";
	case D3D_FEATURE_LEVEL_10_1: return "10_1";
	case D3D_FEATURE_LEVEL_11_0: return "11_0";
	default: return "11_1 or more";
	}
}

static void
releaseBackBuffer(void)
{
	if(d3d.context){
		ID3D11RenderTargetView *nothing = nil;
		d3d.context->OMSetRenderTargets(1, &nothing, nil);
	}
	boundTarget = nil;
	boundDepth = nil;
	release(copySourceResource);
	release(copySource);
	release(d3d.readBack);
	release(d3d.backBufferResource);
	release(d3d.backBufferTarget);
	release(d3d.backBuffer);
}

// the swap chain's buffer, or a texture of our own, as what is drawn into
static bool32
takeBackBuffer(void)
{
	HRESULT hr;
	if(d3d.swapChain){
		hr = d3d.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&d3d.backBuffer);
		if(FAILED(hr)){
			logf("d3d: the swap chain has no buffer, 0x%08X", (unsigned)hr);
			return 0;
		}
	}else{
		D3D11_TEXTURE2D_DESC td;
		memset(&td, 0, sizeof(td));
		td.Width = d3d.width;
		td.Height = d3d.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		hr = d3d.device->CreateTexture2D(&td, nil, &d3d.backBuffer);
		if(FAILED(hr)){
			logf("d3d: no texture of %dx%d to draw into, 0x%08X", (int)d3d.width, (int)d3d.height, (unsigned)hr);
			return 0;
		}
	}
	hr = d3d.device->CreateRenderTargetView(d3d.backBuffer, nil, &d3d.backBufferTarget);
	if(FAILED(hr)){
		logf("d3d: the back buffer cannot be drawn into, 0x%08X", (unsigned)hr);
		return 0;
	}
	// not every swap chain's buffer can be sampled: copyBackBuffer() has another way
	if(FAILED(d3d.device->CreateShaderResourceView(d3d.backBuffer, nil, &d3d.backBufferResource)))
		d3d.backBufferResource = nil;
	return 1;
}

static bool32
makeSwapChain(void)
{
	IDXGIDevice *dxgiDevice = nil;
	IDXGIAdapter *adapter = nil;
	IDXGIFactory2 *factory = nil;
	HRESULT hr = d3d.device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
	if(SUCCEEDED(hr))
		hr = dxgiDevice->GetAdapter(&adapter);
	if(SUCCEEDED(hr))
		hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
	if(SUCCEEDED(hr)){
		DXGI_SWAP_CHAIN_DESC1 sd;
		memset(&sd, 0, sizeof(sd));
		sd.Width = d3d.width;
		sd.Height = d3d.height;
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
		sd.BufferCount = 2;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		// what the game leaves in the back buffer's alpha is not coverage
		sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		hr = factory->CreateSwapChainForComposition(d3d.device, &sd, nil, &d3d.swapChain);
		if(FAILED(hr)){
			// without the buffer being something to sample
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			hr = factory->CreateSwapChainForComposition(d3d.device, &sd, nil, &d3d.swapChain);
		}
	}
	release(factory);
	release(adapter);
	release(dxgiDevice);
	if(FAILED(hr)){
		logf("d3d: no swap chain of %dx%d, 0x%08X", (int)d3d.width, (int)d3d.height, (unsigned)hr);
		return 0;
	}
	if(!host_set_swap_chain(d3d.swapChain, d3d.width, d3d.height)){
		logf("d3d: the shell did not take the swap chain");
		release(d3d.swapChain);
		return 0;
	}
	return 1;
}

static int
startDirect3D(void)
{
	static const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1
	};
	// REVC_D3D_LEVEL=9_3 (REVC_ANGLE_FL, as the tests have it): no more than that level, to see
	// on one device what another has
	int first = 0;
	const char *cap = getenv("REVC_D3D_LEVEL");
	if(cap == nil)
		cap = getenv("REVC_ANGLE_FL");
	if(cap){
		if(strcmp(cap, "10_1") == 0) first = 1;
		if(strcmp(cap, "10_0") == 0) first = 2;
		if(strcmp(cap, "9_3") == 0) first = 3;
		if(strcmp(cap, "9_2") == 0) first = 4;
		if(strcmp(cap, "9_1") == 0) first = 5;
	}
	// BGRA: what a swap chain in a XAML panel is made of. The device keeps its own lock (it is
	// not made for one thread only): the game's thread makes every call that draws, but the
	// shell's UI thread places the swap chain in its panel while the game goes on.
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT hr = D3D11CreateDevice(nil, D3D_DRIVER_TYPE_HARDWARE, nil, flags, levels + first, 6 - first,
		D3D11_SDK_VERSION, &d3d.device, &d3d.level, &d3d.context);
	if(FAILED(hr) && getenv("REVC_D3D_WARP")){
		// no GPU (a machine that only builds): Direct3D's own rasterizer
		hr = D3D11CreateDevice(nil, D3D_DRIVER_TYPE_WARP, nil, flags, levels + first, 6 - first,
			D3D11_SDK_VERSION, &d3d.device, &d3d.level, &d3d.context);
	}
	if(FAILED(hr)){
		logf("d3d: no Direct3D 11 device, 0x%08X", (unsigned)hr);
		return 0;
	}
	if(d3d.level < D3D_FEATURE_LEVEL_9_3){
		// the shaders are compiled for 9_3 (tools/make_shaders.py)
		logf("d3d: feature level %s: the shaders need 9_3", levelName(d3d.level));
		release(d3d.context);
		release(d3d.device);
		return 0;
	}

	glfwGetFramebufferSize(glGlobals.window, &d3d.width, &d3d.height);
	if(host_panel()){
		if(!makeSwapChain())
			return 0;
	}
	if(!takeBackBuffer())
		return 0;
	logf("d3d: Direct3D 11 at feature level %s, %dx%d %s", levelName(d3d.level), (int)d3d.width, (int)d3d.height,
		d3d.swapChain ? "in the panel" : "into a texture (no panel)");
	return 1;
}

static int
stopDirect3D(void)
{
	if(d3d.context)
		d3d.context->ClearState();
	releaseBackBuffer();
	if(d3d.swapChain)
		host_set_swap_chain(nil, 0, 0);
	release(d3d.swapChain);
	release(d3d.context);
	release(d3d.device);
	return 1;
}

// ===[ GLFW's window: the panel, its size, the input ]===

static void
addVideoMode(const GLFWvidmode *mode)
{
	int i;

	for(i = 1; i < glGlobals.numModes; i++){
		if(glGlobals.modes[i].mode.width == mode->width &&
		   glGlobals.modes[i].mode.height == mode->height &&
		   glGlobals.modes[i].mode.redBits == mode->redBits &&
		   glGlobals.modes[i].mode.greenBits == mode->greenBits &&
		   glGlobals.modes[i].mode.blueBits == mode->blueBits){
			// had this mode already, remember highest refresh rate
			if(mode->refreshRate > glGlobals.modes[i].mode.refreshRate)
				glGlobals.modes[i].mode.refreshRate = mode->refreshRate;
			return;
		}
	}

	// none found, add
	glGlobals.modes[glGlobals.numModes].mode = *mode;
	glGlobals.modes[glGlobals.numModes].flags = VIDEOMODEEXCLUSIVE;
	glGlobals.numModes++;
}

static void
makeVideoModeList(GLFWmonitor *monitor)
{
	int i, num;
	const GLFWvidmode *modes;

	modes = glfwGetVideoModes(monitor, &num);
	rwFree(glGlobals.modes);
	glGlobals.modes = rwNewT(DisplayMode, num+1, ID_DRIVER | MEMDUR_EVENT);

	glGlobals.modes[0].mode = *glfwGetVideoMode(monitor);
	glGlobals.modes[0].flags = 0;
	glGlobals.numModes = 1;

	for(i = 0; i < num; i++)
		addVideoMode(&modes[i]);

	for(i = 0; i < glGlobals.numModes; i++){
		num = glGlobals.modes[i].mode.redBits +
			glGlobals.modes[i].mode.greenBits +
			glGlobals.modes[i].mode.blueBits;
		// set depth to power of two
		for(glGlobals.modes[i].depth = 1; glGlobals.modes[i].depth < num; glGlobals.modes[i].depth <<= 1);
	}
}

static int
openGLFW(EngineOpenParams *openparams)
{
	glGlobals.winWidth = openparams->width;
	glGlobals.winHeight = openparams->height;
	glGlobals.winTitle = openparams->windowtitle;
	glGlobals.pWindow = openparams->window;

	memset(&gl3Caps, 0, sizeof(gl3Caps));

	/* Init GLFW */
	if(!glfwInit()){
		RWERROR((ERR_GENERAL, "glfwInit() failed"));
		return 0;
	}

	glGlobals.monitor = glfwGetMonitors(&glGlobals.numMonitors)[0];

	makeVideoModeList(glGlobals.monitor);

	return 1;
}

static int
closeGLFW(void)
{
	glfwTerminate();
	return 1;
}

static int
startGLFW(void)
{
	GLFWwindow *win;
	DisplayMode *mode;

	mode = &glGlobals.modes[glGlobals.currentMode];

	// no client API: the window is the panel and its input, the picture is Direct3D's
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	if(mode->flags & VIDEOMODEEXCLUSIVE)
		win = glfwCreateWindow(mode->mode.width, mode->mode.height, glGlobals.winTitle, glGlobals.monitor, nil);
	else
		win = glfwCreateWindow(glGlobals.winWidth, glGlobals.winHeight, glGlobals.winTitle, nil, nil);
	if(win == nil){
		RWERROR((ERR_GENERAL, "glfwCreateWindow() failed"));
		return 0;
	}

	glGlobals.window = win;
	*glGlobals.pWindow = win;
	glGlobals.presentWidth = 0;
	glGlobals.presentHeight = 0;
	glGlobals.presentOffX = 0;
	glGlobals.presentOffY = 0;
	return 1;
}

static int
stopGLFW(void)
{
	glfwDestroyWindow(glGlobals.window);
	return 1;
}

static int
initDirect3D(void)
{
	// What the interface's callers ask of the OpenGL renderer, answered as an OpenGL ES 2.0
	// context does (which is also how the game's converted textures are marked, in txd.img)
	gl3Caps.gles = 1;
	gl3Caps.glversion = 20;
	UINT support = 0;
	gl3Caps.dxtSupported = SUCCEEDED(d3d.device->CheckFormatSupport(DXGI_FORMAT_BC1_UNORM, &support)) &&
		(support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
	gl3Caps.astcSupported = false;
	gl3Caps.maxAnisotropy = 1.0f;
	gl3Caps.vertexArrays = false;	// OpenGL's; the vertices' layout is an object here anyway
	gl3Caps.programBinaries = false;	// nothing is compiled on the device
	gl3Caps.verticesFromMemory = false;
	// LIBRW_NO_SHARED_BUFFERS=1, LIBRW_PAGE_VERTICES=16384, LIBRW_NO_ATLASES=1
	gl3Caps.sharedBuffers = getenv("LIBRW_NO_SHARED_BUFFERS") == nil;
	gl3Caps.atlases = gl3Caps.dxtSupported && getenv("LIBRW_NO_ATLASES") == nil;
	if(getenv("LIBRW_PAGE_VERTICES")){
		int n = atoi(getenv("LIBRW_PAGE_VERTICES"));
		pageVertices = n < 1024 ? 1024 : n > 65536 ? 65536 : n;
	}

	u_alphaRef = registerUniform("u_alphaRef", UNIFORM_VEC4);
	u_fogData = registerUniform("u_fogData", UNIFORM_VEC4);
	u_fogColor = registerUniform("u_fogColor", UNIFORM_VEC4);
	u_proj = registerUniform("u_proj", UNIFORM_MAT4);
	u_view = registerUniform("u_view", UNIFORM_MAT4);
	u_world = registerUniform("u_world", UNIFORM_MAT4);
	u_ambLight = registerUniform("u_ambLight", UNIFORM_VEC4);
	u_lightParams = registerUniform("u_lightParams", UNIFORM_VEC4, MAX_LIGHTS);
	u_lightPosition = registerUniform("u_lightPosition", UNIFORM_VEC4, MAX_LIGHTS);
	u_lightDirection = registerUniform("u_lightDirection", UNIFORM_VEC4, MAX_LIGHTS);
	u_lightColor = registerUniform("u_lightColor", UNIFORM_VEC4, MAX_LIGHTS);
	u_matColor = registerUniform("u_matColor", UNIFORM_VEC4);
	u_surfProps = registerUniform("u_surfProps", UNIFORM_VEC4);

	// for im2d
	registerUniform("u_xform", UNIFORM_VEC4);

	openShaders();
	openRasters();
	resetRenderState();

	defaultShader = Shader::create("default", "simple");
	assert(defaultShader);
	defaultShader_noAT = Shader::create("default", "simple_noAT");
	assert(defaultShader_noAT);
	defaultShader_fullLight = Shader::create("default_fullLight", "simple");
	assert(defaultShader_fullLight);
	defaultShader_fullLight_noAT = Shader::create("default_fullLight", "simple_noAT");
	assert(defaultShader_fullLight_noAT);
	if(gl3Caps.atlases){
		atlasShader = Shader::create("default_atlas", "atlas");
		assert(atlasShader);
		atlasShader_fullLight = Shader::create("default_atlas_fullLight", "atlas");
		assert(atlasShader_fullLight);
	}

	openIm2D();
	openIm3D();

	return 1;
}

static int
termDirect3D(void)
{
	closeIm3D();
	closeIm2D();

	defaultShader->destroy();
	defaultShader = nil;
	defaultShader_noAT->destroy();
	defaultShader_noAT = nil;
	defaultShader_fullLight->destroy();
	defaultShader_fullLight = nil;
	defaultShader_fullLight_noAT->destroy();
	defaultShader_fullLight_noAT = nil;
	if(atlasShader){
		atlasShader->destroy();
		atlasShader = nil;
		atlasShader_fullLight->destroy();
		atlasShader_fullLight = nil;
	}

	closeRasters();
	closeShaders();
	releaseStateObjects();
	return 1;
}

static int
finalizeDirect3D(void)
{
	return 1;
}

static int
deviceSystemGLFW(DeviceReq req, void *arg, int32 n)
{
	GLFWmonitor **monitors;
	VideoMode *rwmode;

	switch(req){
	case DEVICEOPEN:
		return openGLFW((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeGLFW();

	case DEVICEINIT:
		return startGLFW() && startDirect3D() && initDirect3D();
	case DEVICETERM:
		return termDirect3D() && stopDirect3D() && stopGLFW();

	case DEVICEFINALIZE:
		return finalizeDirect3D();


	case DEVICEGETNUMSUBSYSTEMS:
		return glGlobals.numMonitors;

	case DEVICEGETCURRENTSUBSYSTEM:
		return glGlobals.currentMonitor;

	case DEVICESETSUBSYSTEM:
		monitors = glfwGetMonitors(&glGlobals.numMonitors);
		if(n >= glGlobals.numMonitors)
			return 0;
		glGlobals.currentMonitor = n;
		glGlobals.monitor = monitors[glGlobals.currentMonitor];
		return 1;

	case DEVICEGETSUBSSYSTEMINFO:
		monitors = glfwGetMonitors(&glGlobals.numMonitors);
		if(n >= glGlobals.numMonitors)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, glfwGetMonitorName(monitors[n]), sizeof(SubSystemInfo::name));
		return 1;


	case DEVICEGETNUMVIDEOMODES:
		return glGlobals.numModes;

	case DEVICEGETCURRENTVIDEOMODE:
		return glGlobals.currentMode;

	case DEVICESETVIDEOMODE:
		if(n >= glGlobals.numModes)
			return 0;
		glGlobals.currentMode = n;
		return 1;

	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		rwmode->width = glGlobals.modes[n].mode.width;
		rwmode->height = glGlobals.modes[n].mode.height;
		rwmode->depth = glGlobals.modes[n].depth;
		rwmode->flags = glGlobals.modes[n].flags;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		return 1;
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		glGlobals.numSamples = 1;
		return 1;
	default:
		assert(0 && "not implemented");
		return 0;
	}
	return 1;
}

Device renderdevice = {
	0.0f, 1.0f,
	gl3::beginUpdate,
	gl3::endUpdate,
	gl3::clearCamera,
	gl3::showRaster,
	gl3::rasterRenderFast,
	gl3::setRenderState,
	gl3::getRenderState,
	gl3::im2DRenderLine,
	gl3::im2DRenderTriangle,
	gl3::im2DRenderPrimitive,
	gl3::im2DRenderIndexedPrimitive,
	gl3::im3DTransform,
	gl3::im3DRenderPrimitive,
	gl3::im3DRenderIndexedPrimitive,
	gl3::im3DEnd,
	gl3::deviceSystemGLFW
};

}
}

// ===[ For the GLFW layer (w10m/glfw): the frame's end, the picture read back, a new size ]===

using namespace rw;
using namespace rw::gl3;

extern "C" void
rwd3d11_show(int interval)
{
	if(d3d.swapChain){
		HRESULT hr = d3d.swapChain->Present(interval > 0 ? 1 : 0, 0);
		if(FAILED(hr)){
			static int said;
			if(said++ < 4)
				logf("d3d: the frame was not shown, 0x%08X; the device says 0x%08X", (unsigned)hr,
					(unsigned)d3d.device->GetDeviceRemovedReason());
		}
		// a swap chain that flips takes its buffer off the device when it shows it
		boundTarget = nil;
		boundDepth = nil;
	}else if(d3d.context)
		d3d.context->Flush();
}

extern "C" int
rwd3d11_feature_level(void)
{
	if(d3d.device == nil)
		return 0;
	return ((int)d3d.level >> 12)*10 + (((int)d3d.level >> 8) & 0xF);
}

// The picture as it is now, as RGBA with the bottom row first (what glReadPixels gives)
extern "C" bool
rwd3d11_read_back(int width, int height, unsigned char *rgba)
{
	if(d3d.backBuffer == nil || width != d3d.width || height != d3d.height)
		return false;
	if(d3d.readBack == nil){
		D3D11_TEXTURE2D_DESC td;
		d3d.backBuffer->GetDesc(&td);
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		td.MiscFlags = 0;
		if(FAILED(d3d.device->CreateTexture2D(&td, nil, &d3d.readBack)))
			return false;
	}
	d3d.context->CopyResource(d3d.readBack, d3d.backBuffer);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(FAILED(d3d.context->Map(d3d.readBack, 0, D3D11_MAP_READ, 0, &mapped)))
		return false;
	for(int y = 0; y < height; y++){
		const unsigned char *from = (const unsigned char*)mapped.pData + (size_t)(height - 1 - y)*mapped.RowPitch;
		unsigned char *to = rgba + (size_t)y*width*4;
		for(int x = 0; x < width; x++, from += 4, to += 4){
			to[0] = from[2];	// the buffer is BGRA
			to[1] = from[1];
			to[2] = from[0];
			to[3] = from[3];
		}
	}
	d3d.context->Unmap(d3d.readBack, 0);
	return true;
}

// The panel has another size: the swap chain's buffers are made anew at it
extern "C" void
rwd3d11_resize(int width, int height)
{
	if(d3d.device == nil || (width == d3d.width && height == d3d.height) || width <= 0 || height <= 0)
		return;
	releaseBackBuffer();
	d3d.width = width;
	d3d.height = height;
	if(d3d.swapChain){
		HRESULT hr = d3d.swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
		if(FAILED(hr))
			logf("d3d: the swap chain did not take %dx%d, 0x%08X", width, height, (unsigned)hr);
		host_set_swap_chain(d3d.swapChain, width, height);
	}
	takeBackBuffer();
	invalidateStates();
}
