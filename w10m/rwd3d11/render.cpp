// Drawing instanced geometry: the buffers and the layout of what is drawn, the draw itself,
// the default pipeline's render callback.
//
// As vendor/librw/src/gl/gl3render.cpp, where OpenGL's vertex array objects stand for what
// Direct3D has as an input layout for every pair of a vertex format and a vertex shader
// (shader.cpp keeps them): binding a geometry is its two buffers, and they are a page's that
// many geometries share.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>

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

Perf perf;
bool32 fewerPrograms;

// nothing is compiled on the device, so there is nothing to keep between runs: these are
// the OpenGL renderer's, for its callers
const char *programCacheDir;
int32 numProgramsLoaded, numProgramsCompiled;
void saveProgramBinaries(void) {}

uint64
perfNow(void)
{
	return (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ===[ What is drawn next ]===

static ID3D11Buffer *boundVertices, *boundIndices;
static uint32 boundStride;
static uint32 boundTopology = ~0u;
static uint32 currentFormat;
static AttribDesc *currentAttribs;
static int32 currentNumAttribs;

void
invalidateVertexInput(void)
{
	boundVertices = nil;
	boundIndices = nil;
	boundStride = 0;
	boundTopology = ~0u;
}

void
setVertexInput(ID3D11Buffer *vertices, uint32 stride, ID3D11Buffer *indices, uint32 format,
	AttribDesc *attribs, int32 numAttribs)
{
	if(vertices != boundVertices || stride != boundStride){
		UINT offset = 0;
		boundVertices = vertices;
		boundStride = stride;
		d3d.context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
		perf.vertexArrays++;
		perf.changed |= 2;
	}
	if(indices != boundIndices){
		boundIndices = indices;
		d3d.context->IASetIndexBuffer(indices, DXGI_FORMAT_R16_UINT, 0);
	}
	currentFormat = format;
	currentAttribs = attribs;
	currentNumAttribs = numAttribs;
}

void
setTopology(uint32 primType)
{
	if(primType == boundTopology)
		return;
	boundTopology = primType;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	switch(primType){
	case GL_POINTS: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
	case GL_LINES: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
	case GL_LINE_STRIP: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
	case GL_TRIANGLE_STRIP: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
	default: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
	}
	d3d.context->IASetPrimitiveTopology(topology);
}

bool32
readyToDraw(void)
{
	if(currentShader == nil)
		return 0;
	flushCache();
	return setInputLayout(currentFormat, currentAttribs, currentNumAttribs);
}

void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	if(perf.enabled){
		uint64 t0 = perfNow();
		bool32 ready = readyToDraw();
		uint64 t1 = perfNow();
		if(ready)
			d3d.context->DrawIndexed(inst->numIndex, inst->offset/2, 0);
		uint64 t2 = perfNow();
		perf.stateNs += t1 - t0;
		perf.drawNs += t2 - t1;
		perf.draws++;
		perf.drawNsAfter[perf.changed & 7] += t2 - t1;
		perf.drawsAfter[perf.changed & 7]++;
		perf.changed = 0;
		perf.pipeNs[perf.pipe & 3] += t2 - t1;
		perf.pipeDraws[perf.pipe & 3]++;
		if(t2 - t1 > 1000000){
			perf.pipeLongNs[perf.pipe & 3] += t2 - t1;
			perf.pipeLong[perf.pipe & 3]++;
		}
		return;
	}
	if(readyToDraw())
		d3d.context->DrawIndexed(inst->numIndex, inst->offset/2, 0);
}

// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
void
drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst)
{
	uint32 hasAlpha;
	int alphafunc, alpharef, gsalpharef;
	int zwrite;
	hasAlpha = getAlphaBlend();
	if(hasAlpha){
		zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
		alphafunc = rw::GetRenderState(rw::ALPHATESTFUNC);
		if(zwrite){
			alpharef = rw::GetRenderState(rw::ALPHATESTREF);
			gsalpharef = rw::GetRenderState(rw::GSALPHATESTREF);

			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
			SetRenderState(rw::ALPHATESTREF, gsalpharef);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHALESS);
			SetRenderState(rw::ZWRITEENABLE, 0);
			drawInst_simple(header, inst);
			SetRenderState(rw::ZWRITEENABLE, 1);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
			SetRenderState(rw::ALPHATESTREF, alpharef);
		}else{
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
		}
	}else
		drawInst_simple(header, inst);
}

void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	if(rw::GetRenderState(rw::GSALPHATEST))
		drawInst_GSemu(header, inst);
	else
		drawInst_simple(header, inst);
}

// What OpenGL has vertex array objects for: nothing here
uint32 defaultVertexArray;
void useDefaultVertexArray(void) {}
void resetVertexArray(void) {}
void recordVertexInput(InstanceDataHeader *header) {}
void forgetVertexInput(InstanceDataHeader *header) {}

// An application's own vertices, in the buffers it has bound through setVertexInput... which
// is not what OpenGL's callers do: they are drawn by immed.cpp's buffers instead (see there).
// The layout alone is taken from here.
void
setAttribPointers(AttribDesc *attribDescs, int32 numAttribs)
{
	currentFormat = formatOf(attribDescs, numAttribs);
	currentAttribs = attribDescs;
	currentNumAttribs = numAttribs;
}

void
disableAttribPointers(AttribDesc *attribDescs, int32 numAttribs)
{
}

void
setupVertexInput(InstanceDataHeader *header)
{
	uint64 t0 = perf.enabled ? perfNow() : 0;
	setVertexInput((ID3D11Buffer*)header->d3dVertices, header->attribDesc[0].stride,
		(ID3D11Buffer*)header->d3dIndices, header->d3dFormat, header->attribDesc, header->numAttribs);
	setTopology(header->primType);
	if(perf.enabled)
		perf.inputNs += perfNow() - t0;
}

void
teardownVertexInput(InstanceDataHeader *header)
{
}

int32
lightingCB(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	if(atomic->geometry->flags & rw::Geometry::LIGHT)
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	else
		memset(&lightData, 0, sizeof(lightData));
	return setLights(&lightData);
}

int32
lightingCB(void)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	((World*)engine->currentWorld)->enumerateLights(&lightData);
	return setLights(&lightData);
}

static bool32
hasNormals(InstanceDataHeader *header)
{
	for(int32 i = 0; i < header->numAttribs; i++)
		if(header->attribDesc[i].index == ATTRIB_NORMAL)
			return 1;
	return 0;
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 vsBits = lightingCB(atomic);
	// The shaders with the lights read a normal: what has none is drawn by those without,
	// which is the same picture (a light does nothing to a vertex without a normal)
	bool32 normals = hasNormals(header);
	bool32 lit = normals && ((vsBits & VSLIGHT_MASK) != 0 || fewerPrograms == 1);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	// Geometry whose vertices say where their texture is in an atlas (atlasInstanceCB): the
	// meshes that follow one another and differ in nothing but their texture, which the
	// atlas has too, are one draw. The materials are looked at here, each time: their
	// colours may have been changed since the last.
	perf.pipe = header->atlasInput ? 1 : 0;
	if(header->atlasInput){
		bool32 strips = header->primType == GL_TRIANGLE_STRIP;
		for(int32 i = 0; i < n; ){
			InstanceData run = inst[i];
			InstanceData *last = &inst[i];
			m = run.material;
			int32 j = i + 1;
			if(run.atlas && m->texture == run.atlasFor){
				for(; j < n; j++){
					InstanceData *next = &inst[j];
					Material *o = next->material;
					uint32 after = last->offset + (last->numIndex + (strips ? (last->numIndex & 1 ? 3 : 2) : 0))*2;
					if(next->atlas != run.atlas || o->texture != next->atlasFor || next->offset != after ||
					   next->vertexAlpha != run.vertexAlpha ||
					   memcmp(&o->color, &m->color, sizeof(RGBA)) != 0 ||
					   memcmp(&o->surfaceProps, &m->surfaceProps, sizeof(SurfaceProperties)) != 0)
						break;
					last = next;
				}
				run.numIndex = (last->offset - run.offset)/2 + last->numIndex;
				perf.merged += j - i - 1;
				setTexture(0, run.atlas);
			}else
				setTexture(0, m->texture);
			setMaterial(flags, m->color, m->surfaceProps);
			rw::SetRenderState(VERTEXALPHA, run.vertexAlpha || m->color.alpha != 0xFF);
			(lit ? atlasShader_fullLight : atlasShader)->use();
			drawInst(header, &run);
			i = j;
		}
		teardownVertexInput(header);
		return;
	}

	while(n--){
		m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		if(fewerPrograms == 1 || fewerPrograms == 2)	// by the lights alone: what has none is spared their arithmetic
			(lit ? defaultShader_fullLight : defaultShader)->use();
		else if(fewerPrograms == 3)	// a measurement, not a picture: nothing is discarded
			(lit ? defaultShader_fullLight_noAT : defaultShader_noAT)->use();
		else if(!lit){
			if(getAlphaTest())
				defaultShader->use();
			else
				defaultShader_noAT->use();
		}else{
			if(getAlphaTest())
				defaultShader_fullLight->use();
			else
				defaultShader_fullLight_noAT->use();
		}

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}


}
}
