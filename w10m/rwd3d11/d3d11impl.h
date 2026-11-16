// The Direct3D 11 renderer's own: what its files share. The interface it gives librw and the
// game is rw::gl3's (vendor/librw/src/gl/rwgl3.h, rwgl3shader.h, rwgl3impl.h).
//
// Everything here is called on one thread, the game's.
#ifndef RWD3D11_IMPL_H
#define RWD3D11_IMPL_H

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace rw {
namespace gl3 {

struct D3d
{
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	D3D_FEATURE_LEVEL level;
	// What is shown: the swap chain in the shell's panel, or, where there is no panel (the
	// tests on a PC), a texture nobody sees
	IDXGISwapChain1 *swapChain;
	ID3D11Texture2D *backBuffer;
	ID3D11RenderTargetView *backBufferTarget;
	ID3D11ShaderResourceView *backBufferResource;	// nil where it cannot be sampled
	ID3D11Texture2D *readBack;	// a copy of it that can be read (made when first asked for)
	int32 width, height;
};
extern D3d d3d;

template <class T> inline void
release(T *&p)
{
	if(p){
		p->Release();
		p = nil;
	}
}

void logf(const char *format, ...);

// ===[ device.cpp ]===

enum { MAXNUMSTAGES = 8 };

// what the next draw is drawn with, apart from the uniforms: render states, textures
void flushStates(void);
// a raster that is going away, or is about to be drawn into: off the texture stages
void evictRaster(Raster *raster);
// the picture drawn so far into a texture, x across and y down from its top left corner,
// turned upside down (textures are kept bottom-up): what rasterRenderFast is for
bool32 copyBackBuffer(Raster *dst, int32 x, int32 y);
// the device's state is not what this file thinks it is: everything is set again
void invalidateStates(void);

// ===[ shader.cpp ]===

struct ShaderBufferState
{
	ID3D11Buffer *buffer;
	uint8 *shadow;
	uint32 size;
	uint32 slot;
	bool32 dirty;
};

// where a registered uniform is in a stage's constant buffers
struct UniformPlace
{
	int16 buffer;	// -1: the shader does not have it
	uint16 size;
	uint32 offset;
};

struct InputLayout
{
	uint32 format;
	ID3D11InputLayout *layout;
};

// A compiled shader: one for each ShaderCode, shared by the programs that are made of it.
// Its uniforms are its own, whichever program it is used in.
struct ShaderStage
{
	const ShaderCode *code;
	ID3D11VertexShader *vertex;
	ID3D11PixelShader *pixel;
	int32 numBuffers;
	ShaderBufferState buffers[8];
	ID3D11Buffer *bound[8];	// by slot, as they are handed to the device
	int32 numSlots;
	UniformPlace places[MAX_UNIFORMS];
	uint32 serialNums[MAX_UNIFORMS];
	InputLayout layouts[24];
	int32 numLayouts;
};

void openShaders(void);
void closeShaders(void);
// the registered uniforms that have changed go into the shaders in use, and from there to
// the device
void flushUniforms(void);
// The layout of the vertices about to be drawn, for the vertex shader in use. `format` stands
// for the attributes (formatOf): it is all that is looked at when the layout is there already.
bool32 setInputLayout(uint32 format, AttribDesc *attribs, int32 numAttribs);
uint32 formatOf(AttribDesc *attribs, int32 numAttribs);

// ===[ render.cpp ]===

// The buffers and the layout of what is drawn next. Indices are 16 bits wide.
void setVertexInput(ID3D11Buffer *vertices, uint32 stride, ID3D11Buffer *indices, uint32 format,
	AttribDesc *attribs, int32 numAttribs);
void setTopology(uint32 primType);	// GL_TRIANGLES and its like
// everything is as the draw needs it: the states, the uniforms, the layout. False: not to be
// drawn (the shader asks for an attribute the vertices do not have).
bool32 readyToDraw(void);
void invalidateVertexInput(void);

// ===[ pipe.cpp ]===

enum
{
	INSTANCE_ATLAS = 1,	// with where each vertex's texture is in an atlas
	INSTANCE_SKIN = 2,	// with a normal in any case, the bones' weights and their indices
};
void instanceVertices(Geometry *geo, InstanceDataHeader *header, bool32 reinstance, uint32 flags);

// ===[ raster.cpp ]===

void openRasters(void);
void closeRasters(void);
extern ID3D11ShaderResourceView *whiteTexture;

// ===[ immed.cpp ]===

void im2DSetXform(void);
// Four corners as a strip, with the shader, the states and the texture that are set: for a
// copy from one texture into another
void im2DDrawStrip(Im2DVertex *vertices, int32 numVertices);

}
}

#endif
