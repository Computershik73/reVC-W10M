// The Direct3D 11 renderer's own: what its files share. The interface it gives librw and the
// game is rw::gl3's (vendor/librw/src/gl/rwgl3.h, rwgl3shader.h, rwgl3impl.h).
//
// Everything here is called on one thread, the game's; thread.cpp has the other one, where
// there is one, and says what goes across.
#ifndef RWD3D11_IMPL_H
#define RWD3D11_IMPL_H

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

namespace rw {
namespace gl3 {

struct D3d
{
	ID3D11Device *device;
	// What the renderer calls. With a drawing thread (threaded) it is a deferred context,
	// which keeps the calls for that thread to make; without, the device's own.
	ID3D11DeviceContext *context;
	// The device's own context: the drawing thread's, and the game's thread's only between
	// syncDevice() and its next call that is kept. The same as `context` without the thread.
	ID3D11DeviceContext *immediate;
	bool32 threaded;
	// the same context as Direct3D 11.1 has it (DiscardView), or nil; and whether the GPU is
	// told what it need not keep (REVC_D3D_KEEP=1: it is not)
	ID3D11DeviceContext1 *immediate1;
	bool32 discards;
	bool32 depthDiscards;	// the depth buffer told away after a frame as well (device.cpp)
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

// What REVC_PERF asks of this renderer alone: nanoseconds and counts since the line before
// (rwd3d11_perf_line). The draws themselves are in rw::gl3::perf, as the OpenGL renderer's.
struct D3dPerf
{
	uint64 renderNs;	// in the pipelines' render: instancing, and the render callback
	uint64 instanceNs;	// of that, instancing: a geometry's vertices made and handed over
	uint64 uploadNs;	// of that, Direct3D's part: buffers made and written
	uint32 instances, reinstances, uploadBytes;
	uint64 uniformNs;	// constant buffers handed over
	uint32 uniformBuffers;
	// of those, the vertex shaders': State, Scene, Object, Material, Lights and the rest, as
	// shaders/common.hlsli names them. Each is a draw whose vertex shader's constants are not
	// the draw's before, which a GPU of level 9 pays for (bench/d3dbench.cpp).
	uint32 vertexBuffers[6];
	uint64 cameraNs;	// a camera's begin and its clear
	uint64 textureNs;	// textures made, and written when they are unlocked
	uint32 textures;
	uint64 copyNs;	// the picture copied into a texture
	uint32 copies;
	uint64 showNs;	// the frame shown, or handed to the drawing thread
	uint32 readBacks;	// textures and frames read back: each waits for everything drawn so far
};
extern D3dPerf d3dPerf;

// frames shown so far (pipe.cpp has it for what it must not write over too soon)
extern uint32 frameNumber;

// the time of a scope into one of those, where REVC_PERF is on
struct PerfScope
{
	uint64 *sum;
	uint64 from;
	PerfScope(uint64 *s) : sum(s), from(perf.enabled ? perfNow() : 0) {}
	~PerfScope() { if(from) *sum += perfNow() - from; }
};

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
// The context is as a new one (the calls kept so far have been handed over): nothing is bound,
// and what a camera in the middle of its update draws into is bound again
void deviceStateLost(void);
// The frame is shown, on the thread that makes the device's calls. late: it took long enough
// not to wait for the vertical blank as well.
void presentFrame(int32 interval, bool32 late);

// ===[ thread.cpp: the drawing thread ]===
// The game's thread keeps a frame's calls in a deferred context; this thread makes them and
// shows the frame while the game's thread goes on with the next.

// d3d.context becomes a deferred context. False: there is none, everything stays as it is.
bool32 startDrawingThread(void);
void stopDrawingThread(void);
// What has been kept so far goes to the drawing thread, once it is done with what it had;
// present: the frame is shown after it, and `depth`, if there is one, need not be kept
void submitFrame(bool32 present, int32 interval, ID3D11DepthStencilView *depth = nil);
// Everything kept so far has been made and the drawing thread does nothing: the game's thread
// may use d3d.immediate. Nothing to do without the thread.
void syncDevice(void);
// Bytes for a part of a buffer that nothing made so far is drawn from, written before the
// frame's calls are made. base not nil: 16-bit indices, with that number added to each.
void queueBufferWrite(ID3D11Buffer *buffer, uint32 at, uint32 size, const void *data, const uint32 *base);

// what the drawing thread did since this was last asked (REVC_PERF)
struct ThreadPerf
{
	uint64 makeNs;	// buffers written and the kept calls made
	uint64 showNs;
	uint64 waitedNs;	// the game's thread, for the drawing thread to be done with the frame before
	uint32 frames, shows, showsAtOnce, writes;
};
void takeThreadPerf(ThreadPerf *p);
// counted by presentFrame, for whichever thread it is on
extern uint32 showsCounted, showsAtOnceCounted;

// ===[ measure.cpp: what the GPU is asked for, changed or counted, to see where its time goes ]===
struct Measure
{
	int32 viewport;	// REVC_D3D_VIEWPORT: per cent of the picture's width and height it is drawn into; 0: all
	int32 scissor;	// REVC_D3D_SCISSOR: per cent of them outside which nothing is drawn; 0: no such limit
	int32 pixels;	// REVC_D3D_PIXELS: 1 the pixels drawn are counted, 2 with the depth test always passing
	int32 delay;	// REVC_D3D_DELAY: milliseconds the drawing thread is kept busy before it makes a frame's calls
	uint64 triangles;	// asked for so far
};
extern Measure measure;
void openMeasure(void);	// once the device is there
void closeMeasure(void);
void measureFrameEnd(void);	// before the frame is shown

// ===[ immed.cpp ]===
// the buffers immediate mode writes along are begun anew (a deferred context wants it of
// every list of calls)
void freshStreams(void);

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
	// a vertex shader that counts lights of every kind: the same for directional lights alone,
	// which is what is drawn with while there are no others (nil: there is no such shader)
	ShaderStage *directionals;
	// The same shader reading what a draw has of its own with the vertices (INSTANCE,
	// shaders/default_vs.hlsl), which is what instanced geometry is drawn with where that is
	// done (nil: there is no such shader, or it is not done). And for such a shader, how
	// many vectors it reads that way: 5, or 6 with a material effect's colour.
	ShaderStage *instanced;
	int32 instanceVectors;
};
// no light is a point or a spot light (setLights says so)
extern bool32 onlyDirectionals;
// what differs between draws of geometry goes with the vertices (render.cpp says why)
extern bool32 instancing;
// Geometry is being drawn (drawInst), not immediate mode's vertices: the shader in use may be
// its form that reads a draw's own data with the vertices
extern bool32 geometryDraw;
// how many vectors the vertex shader that is drawn with now reads that way, 0 if none
int32 instanceVectorsInUse(void);
extern Shader *atlasShader_noAT, *atlasShader_fullLight_noAT;
extern int32 u_world, u_ambLight;

void openShaders(void);
void closeShaders(void);
// nothing is bound on the context any more: the shaders in use are bound again when next used
void forgetShaderBindings(void);
// the shaders in use are bound, if they are not
void bindShaders(void);
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
// so many indices or vertices are drawn, of that kind: their triangles are counted
void countDraw(uint32 n);
// What a draw of the default pipeline has of its own, for a shader that reads it with the
// vertices (render.cpp)
enum { INSTANCE_BYTES = 96 };
bool32 openInstances(void);
void closeInstances(void);
void freshInstances(void);	// as freshStreams
// What may be seen through is drawn after what may not (render.cpp): the draws that were
// kept for it are made now. Before anything is drawn that is not geometry, and where a
// camera's picture ends.
extern bool32 opaqueFirst;
void drawLater(void);
// the first texture stage as the game has set it, to put it back
struct TextureStage
{
	Raster *raster;
	uint32 addressU, addressV, filter;
};
void getTextureStage(TextureStage *stage);
void setTextureStage(const TextureStage *stage);
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
void im2dBehindOn(bool32 on);	// rwd3d11_draw_behind
// Four corners as a strip, with the shader, the states and the texture that are set: for a
// copy from one texture into another
void im2DDrawStrip(Im2DVertex *vertices, int32 numVertices);

}
}

#endif
