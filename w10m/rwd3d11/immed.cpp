// Immediate mode: the HUD, the menus, the effects. Vertices that are new with every draw.
//
// They go into one vertex buffer and one index buffer that are written along, each draw
// after the last, without Direct3D waiting for what it still draws from them (a write that
// overwrites nothing); when a buffer is full it is given up for a new one. A draw is then
// its part of the two buffers.
//
// Direct3D has no triangle fans: a fan is drawn as the triangles it is.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

namespace rw {
namespace gl3 {

uint32 im2DVbo, im2DIbo;	// the OpenGL renderer's, for its callers

Shader *im2dOverrideShader;
// what is drawn in two dimensions lies behind everything drawn so far (see drawIm2D)
static bool32 im2dBehind;
static Shader *im2dBehindShader;

void im2dBehindOn(bool32 on) { im2dBehind = on; }

// The two buffers immediate mode writes along. With a drawing thread they are begun anew for
// every list of calls (a deferred context discards a buffer before it writes on in it, and
// keeps the whole of a discarded buffer for the thread), so they are smaller there.
static uint32 STREAM_VERTEX_BYTES = 1<<20;
static uint32 STREAM_INDICES = 1<<17;

static ID3D11Buffer *streamVertices, *streamIndices;
static uint32 vertexAt;	// bytes
static uint32 indexAt;	// indices
static bool32 vertexFresh, indexFresh;	// nothing written yet

static Shader *im2dShader;
static AttribDesc im2dattribDesc[3] = {
	{ ATTRIB_POS,        GL_FLOAT,         GL_FALSE, 4,
		sizeof(Im2DVertex), 0 },
	{ ATTRIB_COLOR,      GL_UNSIGNED_BYTE, GL_TRUE,  4,
		sizeof(Im2DVertex), offsetof(Im2DVertex, r) },
	{ ATTRIB_TEXCOORDS0, GL_FLOAT,         GL_FALSE, 2,
		sizeof(Im2DVertex), offsetof(Im2DVertex, u) },
};
static uint32 im2dFormat;

static int primTypeMap[] = {
	GL_POINTS,	// invalid
	GL_LINES,
	GL_LINE_STRIP,
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_TRIANGLE_FAN,
	GL_POINTS
};

// Vertices into the buffer: the number of the first of them, counted in vertices of this
// size from the buffer's start. -1: too many for the buffer.
static int32
putVertices(const void *vertices, int32 numVertices, uint32 stride)
{
	uint32 size = numVertices*stride;
	if(streamVertices == nil || size > STREAM_VERTEX_BYTES)
		return -1;
	uint32 at = (vertexAt + stride - 1)/stride*stride;
	D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
	if(vertexFresh || at + size > STREAM_VERTEX_BYTES){
		at = 0;
		how = D3D11_MAP_WRITE_DISCARD;
		vertexFresh = 0;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(FAILED(d3d.context->Map(streamVertices, 0, how, 0, &mapped)))
		return -1;
	memcpy((uint8*)mapped.pData + at, vertices, size);
	d3d.context->Unmap(streamVertices, 0);
	vertexAt = at + size;
	return at/stride;
}

// Indices into the buffer, or those of a fan's triangles: where the first of them is.
// *numIndices is how many were written.
static int32
putIndices(const uint16 *indices, int32 *numIndices, bool32 fan)
{
	int32 n = *numIndices;
	int32 count = fan ? (n >= 3 ? (n - 2)*3 : 0) : n;
	if(streamIndices == nil || count <= 0 || count > STREAM_INDICES)
		return -1;
	D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
	if(indexFresh || indexAt + count > STREAM_INDICES){
		indexAt = 0;
		how = D3D11_MAP_WRITE_DISCARD;
		indexFresh = 0;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(FAILED(d3d.context->Map(streamIndices, 0, how, 0, &mapped)))
		return -1;
	uint16 *to = (uint16*)mapped.pData + indexAt;
	if(fan){
		// the n corners of a fan are given by `indices`, or are 0, 1, 2...
		for(int32 i = 0; i < n - 2; i++){
			to[i*3+0] = indices ? indices[0] : 0;
			to[i*3+1] = indices ? indices[i+1] : (uint16)(i + 1);
			to[i*3+2] = indices ? indices[i+2] : (uint16)(i + 2);
		}
	}else
		memcpy(to, indices, count*2);
	d3d.context->Unmap(streamIndices, 0);
	int32 first = indexAt;
	indexAt += count;
	*numIndices = count;
	return first;
}

void
freshStreams(void)
{
	vertexFresh = indexFresh = 1;
}

static bool32
openStreams(void)
{
	if(d3d.threaded){
		STREAM_VERTEX_BYTES = 1<<19;
		STREAM_INDICES = 1<<16;
	}
	D3D11_BUFFER_DESC bd;
	memset(&bd, 0, sizeof(bd));
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	bd.ByteWidth = STREAM_VERTEX_BYTES;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	HRESULT hr = d3d.device->CreateBuffer(&bd, nil, &streamVertices);
	if(SUCCEEDED(hr)){
		bd.ByteWidth = STREAM_INDICES*2;
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		hr = d3d.device->CreateBuffer(&bd, nil, &streamIndices);
	}
	if(FAILED(hr)){
		logf("d3d: no buffers for immediate mode, 0x%08X", (unsigned)hr);
		return 0;
	}
	vertexAt = indexAt = 0;
	vertexFresh = indexFresh = 1;
	return 1;
}

static void
immediateDone(uint64 t0)
{
	if(perf.enabled){
		uint64 took = perfNow() - t0;
		perf.immediateNs += took;
		perf.immediates++;
		perf.changed = 0;
		if(took > 1000000){
			perf.immediateLongNs += took;
			perf.immediateLong++;
		}
	}
}

// What has been put into the buffers, drawn: `first` is the first vertex; `indices`, where
// there are any (or the corners of a fan), are as the caller has them
static void
drawStream(PrimitiveType primType, int32 first, int32 numVertices, uint32 stride, uint32 format,
	AttribDesc *attribs, int32 numAttribs, const uint16 *indices, int32 numIndices)
{
	if(first < 0)
		return;
	// what is not geometry comes after everything of the geometry before it
	drawLater();
	bool32 fan = primType == PRIMTYPETRIFAN;
	int32 firstIndex = -1;
	if(indices || fan){
		if(indices == nil)
			numIndices = numVertices;
		firstIndex = putIndices(indices, &numIndices, fan);
		if(firstIndex < 0)
			return;
	}
	setVertexInput(streamVertices, stride, streamIndices, format, attribs, numAttribs);
	setTopology(fan ? GL_TRIANGLES : primTypeMap[primType]);
	if(!readyToDraw())
		return;
	countDraw(firstIndex >= 0 ? numIndices : numVertices);
	if(firstIndex >= 0)
		d3d.context->DrawIndexed(numIndices, firstIndex, first);
	else
		d3d.context->Draw(numVertices, first);
}

void
openIm2D(void)
{
	im2dShader = Shader::create("im2d", "simple");
	assert(im2dShader);
	im2dBehindShader = Shader::create("im2d", "simple_masked");
	im2dFormat = formatOf(im2dattribDesc, 3);
	openStreams();
}

void
closeIm2D(void)
{
	invalidateVertexInput();
	release(streamIndices);
	release(streamVertices);
	im2dShader->destroy();
	im2dShader = nil;
	if(im2dBehindShader){
		im2dBehindShader->destroy();
		im2dBehindShader = nil;
	}
}

static Im2DVertex tmpprimbuf[3];

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	im2DRenderPrimitive(PRIMTYPELINELIST, tmpprimbuf, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	tmpprimbuf[2] = verts[vert3];
	im2DRenderPrimitive(PRIMTYPETRILIST, tmpprimbuf, 3);
}

// ===[ What lies behind everything, drawn after it ]===
// The game draws its sky first, over the whole picture, and the world over it: in a street
// one and a half pictures' worth of clouds of which a third is seen in the end, every pixel
// of them worked out (a Lumia 650: 3.1 ms a picture's worth at 1280x720). Drawn after the
// world's houses instead, at the far end of the depth buffer and tested against it, the
// pixels a house hides are given up before they are worked out (0.5 ms), if the shader does
// not throw pixels away: simple_masked does the alpha test's work without. The game says
// when (rwd3d11_draw_behind); what is drawn in between is drawn so whatever depth and
// whatever test it asks for itself, and writes no depth.
static void
drawIm2D(PrimitiveType primType, void *vertices, int32 numVertices, void *indices, int32 numIndices)
{
	uint64 t0 = perf.enabled ? perfNow() : 0;
	Im2DVertex *moved = nil;
	uint32 ztest = 0, zwrite = 0;
	if(im2dBehind && numVertices > 0){
		moved = (Im2DVertex*)rwMalloc(numVertices*sizeof(Im2DVertex), MEMDUR_FUNCTION | ID_DRIVER);
		memcpy(moved, vertices, numVertices*sizeof(Im2DVertex));
		for(int32 i = 0; i < numVertices; i++)
			moved[i].z = 1.0f;
		vertices = moved;
		ztest = rw::GetRenderState(rw::ZTESTENABLE);
		zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
		rw::SetRenderState(rw::ZTESTENABLE, 1);
		rw::SetRenderState(rw::ZWRITEENABLE, 0);
	}
	if(im2dOverrideShader)
		im2dOverrideShader->use();
	else if(moved && im2dBehindShader)
		im2dBehindShader->use();
	else
		im2dShader->use();
	im2DSetXform();
	drawStream(primType, putVertices(vertices, numVertices, sizeof(Im2DVertex)), numVertices, sizeof(Im2DVertex),
		im2dFormat, im2dattribDesc, 3, (uint16*)indices, numIndices);
	if(moved){
		rw::SetRenderState(rw::ZTESTENABLE, ztest);
		rw::SetRenderState(rw::ZWRITEENABLE, zwrite);
		rwFree(moved);
	}
	immediateDone(t0);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	drawIm2D(primType, vertices, numVertices, nil, 0);
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	drawIm2D(primType, vertices, numVertices, indices, numIndices);
}

// with the shader, the states, the uniforms and the texture that are set (device.cpp)
void
im2DDrawStrip(Im2DVertex *vertices, int32 numVertices)
{
	int32 first = putVertices(vertices, numVertices, sizeof(Im2DVertex));
	if(first < 0)
		return;
	drawLater();
	setVertexInput(streamVertices, sizeof(Im2DVertex), streamIndices, im2dFormat, im2dattribDesc, 3);
	setTopology(GL_TRIANGLE_STRIP);
	bindShaders();
	if(setInputLayout(im2dFormat, im2dattribDesc, 3))
		d3d.context->Draw(numVertices, first);
}

// Vertices of the application's own format (two sets of texture coordinates: the drops on
// the screen), drawn as immediate mode draws its own, with the shader that is in use
void
im2DRenderIndexedCustom(PrimitiveType primType, void *vertices, int32 numVertices, uint32 stride,
	AttribDesc *attribs, int32 numAttribs, void *indices, int32 numIndices)
{
	uint64 t0 = perf.enabled ? perfNow() : 0;
	im2DSetXform();
	drawStream(primType, putVertices(vertices, numVertices, stride), numVertices, stride,
		formatOf(attribs, numAttribs), attribs, numAttribs, (uint16*)indices, numIndices);
	immediateDone(t0);
}


// Im3D


static Shader *im3dShader;
static AttribDesc im3dattribDesc[4] = {
	{ ATTRIB_POS,        GL_FLOAT,         GL_FALSE, 3,
		sizeof(Im3DVertex), 0 },
	{ ATTRIB_NORMAL,     GL_FLOAT,         GL_FALSE, 3,
		sizeof(Im3DVertex), offsetof(Im3DVertex, normal) },
	{ ATTRIB_COLOR,      GL_UNSIGNED_BYTE, GL_TRUE,  4,
		sizeof(Im3DVertex), offsetof(Im3DVertex, r) },
	{ ATTRIB_TEXCOORDS0, GL_FLOAT,         GL_FALSE, 2,
		sizeof(Im3DVertex), offsetof(Im3DVertex, u) },
};
static uint32 im3dFormat;
static int32 num3DVertices;
static int32 first3DVertex;

void
openIm3D(void)
{
	im3dShader = Shader::create("im3d", "simple");
	assert(im3dShader);
	im3dFormat = formatOf(im3dattribDesc, 4);
}

void
closeIm3D(void)
{
	im3dShader->destroy();
	im3dShader = nil;
}

// settable by user - TOOD: make this less shit
RGBA im3dMaterialColor = { 255, 255, 255, 255 };
SurfaceProperties im3dSurfaceProps = { 1.0f, 1.0f, 1.0f };

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	uint64 t0 = perf.enabled ? perfNow() : 0;
	if(world == nil){
		static Matrix ident;
		ident.setIdentity();
		world = &ident;
	}
	setWorldMatrix(world);
	if(flags & im3d::LIGHTING){
		setMaterial(im3dMaterialColor, im3dSurfaceProps);
		lightingCB();
		defaultShader_fullLight->use();
	}else
		im3dShader->use();

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

	first3DVertex = putVertices(vertices, numVertices, sizeof(Im3DVertex));
	num3DVertices = numVertices;
	if(perf.enabled)
		perf.immediateNs += perfNow() - t0;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	uint64 t0 = perf.enabled ? perfNow() : 0;
	drawStream(primType, first3DVertex, num3DVertices, sizeof(Im3DVertex), im3dFormat, im3dattribDesc, 4, nil, 0);
	immediateDone(t0);
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	uint64 t0 = perf.enabled ? perfNow() : 0;
	drawStream(primType, first3DVertex, num3DVertices, sizeof(Im3DVertex), im3dFormat, im3dattribDesc, 4,
		(uint16*)indices, numIndices);
	immediateDone(t0);
}

void
im3DEnd(void)
{
}

}
}

extern "C" void
rwd3d11_draw_behind(int on)
{
	rw::gl3::im2dBehindOn(on != 0);
}
