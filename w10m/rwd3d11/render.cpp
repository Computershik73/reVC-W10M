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
D3dPerf d3dPerf;
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

// ===[ What a draw has of its own, with its vertices ]===
// Where an object is, the light it stands in and its material are constants of the vertex
// shader in librw's renderers, set before each draw. A GPU of Direct3D's level 9 pays for a
// draw whose vertex shader's constants differ from those of the draw before: a Lumia 650
// 87 microseconds, against 19 for one that changes nothing, and the two hundred such draws
// of a street were a third of what a frame took at any size of the picture. Read with the
// vertices instead, from a second buffer that has them once for the draw (an instance of
// one), they cost 32 (bench/d3dbench.cpp, REVC_BENCH=1). The default pipeline's shaders
// and the material effects' (the vehicles) have that form (INSTANCE in shaders/default_vs.hlsl
// and matfx_env_vs.hlsl), and shader.cpp puts it in a shader's place for a draw of geometry;
// what is skinned keeps its constants (its bones are constants anyway).
//
// The records are written one after another into a buffer of their own, as immediate mode
// writes its vertices, and a draw binds the buffer where its record is.

enum { INSTANCE_RECORDS = 1024 };
static ID3D11Buffer *instanceStream;
static uint32 instanceAt;	// the next record's number
static bool32 instanceFresh;	// the buffer is to be begun anew (a new list of calls)
static bool32 instanceBound;	// the record last written is where the buffer is bound
static float instanceLast[INSTANCE_BYTES/4];

bool32
openInstances(void)
{
	D3D11_BUFFER_DESC bd;
	memset(&bd, 0, sizeof(bd));
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	bd.ByteWidth = INSTANCE_RECORDS*INSTANCE_BYTES;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	HRESULT hr = d3d.device->CreateBuffer(&bd, nil, &instanceStream);
	if(FAILED(hr)){
		logf("d3d: no buffer for what the draws have of their own, 0x%08X: it stays in constants", (unsigned)hr);
		return 0;
	}
	freshInstances();
	return 1;
}

void
closeInstances(void)
{
	release(instanceStream);
}

void
freshInstances(void)
{
	instanceFresh = 1;
	instanceBound = 0;
}

// The record of a draw, from the uniforms as they are set: the world matrix, the ambient
// light, the material
static void
fillInstance(float *record)
{
	const float *world = (const float*)uniformRegistry.uniforms[u_world].data;	// right, up, at, pos
	const float *ambient = (const float*)uniformRegistry.uniforms[u_ambLight].data;
	const float *colour = (const float*)uniformRegistry.uniforms[u_matColor].data;
	const float *surface = (const float*)uniformRegistry.uniforms[u_surfProps].data;	// ambient, specular, diffuse
	for(int32 row = 0; row < 3; row++){
		record[row*4+0] = world[0+row];
		record[row*4+1] = world[4+row];
		record[row*4+2] = world[8+row];
		record[row*4+3] = world[12+row];
	}
	record[12] = ambient[0]*surface[0];
	record[13] = ambient[1]*surface[0];
	record[14] = ambient[2]*surface[0];
	record[15] = surface[2];
	memcpy(&record[16], colour, 16);
	// the colour of a material effect's environment map, where there is such a thing
	static int32 u_envColor = -1;
	if(u_envColor < 0)
		u_envColor = findUniform("u_envColor");
	if(u_envColor >= 0)
		memcpy(&record[20], uniformRegistry.uniforms[u_envColor].data, 16);
	else
		memset(&record[20], 0, 16);
}

// The record of the draw that comes next is where it is read from. False: it is not, and
// there is nothing to draw with.
static bool32
placeInstance(const float *record)
{
	// the meshes of one object with one material: the record is there
	if(instanceBound && memcmp(record, instanceLast, INSTANCE_BYTES) == 0)
		return 1;
	D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
	if(instanceFresh || instanceAt >= INSTANCE_RECORDS){
		instanceAt = 0;
		how = D3D11_MAP_WRITE_DISCARD;
		instanceFresh = 0;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(FAILED(d3d.context->Map(instanceStream, 0, how, 0, &mapped))){
		instanceBound = 0;
		return 0;
	}
	memcpy((uint8*)mapped.pData + instanceAt*INSTANCE_BYTES, record, INSTANCE_BYTES);
	d3d.context->Unmap(instanceStream, 0);
	UINT stride = INSTANCE_BYTES, offset = instanceAt*INSTANCE_BYTES;
	d3d.context->IASetVertexBuffers(1, 1, &instanceStream, &stride, &offset);
	instanceAt++;
	memcpy(instanceLast, record, INSTANCE_BYTES);
	instanceBound = 1;
	return 1;
}

// ===[ What may be seen through, after what may not ]===
// The game draws with the alpha test on for everything, and its pixel shaders throw a pixel
// away where the test fails. A shader that can throw pixels away keeps a GPU of level 9 from
// giving up a hidden pixel before it has worked it out: a Lumia 650 draws a picture's worth
// of hidden pixels in 0.55 ms without the test in the shader and in 3.1 with it, and in a
// street one to two pictures' worth is hidden. But changing shaders for every mesh that
// needs the test, and back, costs more than it saves there (170 microseconds each way).
// So the meshes that can fail the test (their texture or their colour has alpha) are kept,
// with everything they are drawn with, and drawn together once the geometry before them is
// done: when something that is not geometry comes (the water, an effect, the sky), or the
// camera's picture ends. Everything else is drawn at once by the shaders without the test.
// What is seen through is so drawn over what is not, as the game means it to be; among
// themselves such meshes keep their order, and against a vehicle's glass, which is drawn at
// once by its own pipeline, they come later than the game has them.
// A GPU above level 9 chooses its shader mesh by mesh (a change of shaders costs it little),
// so the opaque meshes have the shaders without the test there in any case, and the order
// would be all that is new: not shown to be worth anything on an Elite x3, and not done
// there unless asked for (device.cpp).
// With the PlayStation 2's alpha test a kept mesh is drawn in that test's two passes.
bool32 opaqueFirst;

struct Later
{
	InstanceDataHeader *header;
	InstanceData run;
	Texture *texture;
	float record[INSTANCE_BYTES/4];
	uint32 vertexAlpha, cull, zwrite, ztest, alphaFunc, alphaRef, srcBlend, destBlend, fog;
	bool32 atlas;	// its vertices say where their texture is in an atlas
};
enum { MAX_LATER = 512 };
static Later later[MAX_LATER];
static int32 numLater;
// the record of the kept draw that is being made
static const float *laterRecord;

void setupVertexInput(InstanceDataHeader *header);

static void
keepForLater(InstanceDataHeader *header, InstanceData *run, Texture *texture)
{
	if(numLater == MAX_LATER){
		drawLater();
		setupVertexInput(header);	// the geometry that is being drawn
	}
	Later *l = &later[numLater++];
	l->header = header;
	l->run = *run;
	l->texture = texture;
	l->atlas = header->atlasInput;
	fillInstance(l->record);
	l->vertexAlpha = rw::GetRenderState(rw::VERTEXALPHA);
	l->cull = rw::GetRenderState(rw::CULLMODE);
	l->zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
	l->ztest = rw::GetRenderState(rw::ZTESTENABLE);
	l->alphaFunc = rw::GetRenderState(rw::ALPHATESTFUNC);
	l->alphaRef = rw::GetRenderState(rw::ALPHATESTREF);
	l->srcBlend = rw::GetRenderState(rw::SRCBLEND);
	l->destBlend = rw::GetRenderState(rw::DESTBLEND);
	l->fog = rw::GetRenderState(rw::FOGENABLE);
}

void
drawLater(void)
{
	if(numLater == 0 || laterRecord)
		return;
	// what the game has set, to be put back
	static const int32 states[] = {
		rw::VERTEXALPHA, rw::CULLMODE, rw::ZWRITEENABLE, rw::ZTESTENABLE, rw::ALPHATESTFUNC,
		rw::ALPHATESTREF, rw::SRCBLEND, rw::DESTBLEND, rw::FOGENABLE
	};
	uint32 before[nelem(states)];
	for(uint32 k = 0; k < nelem(states); k++)
		before[k] = rw::GetRenderState(states[k]);
	TextureStage stage;
	getTextureStage(&stage);
	Shader *shader = currentShader;

	int32 n = numLater;
	for(int32 i = 0; i < n; i++){
		Later *l = &later[i];
		rw::SetRenderState(rw::CULLMODE, l->cull);
		rw::SetRenderState(rw::ZWRITEENABLE, l->zwrite);
		rw::SetRenderState(rw::ZTESTENABLE, l->ztest);
		rw::SetRenderState(rw::ALPHATESTFUNC, l->alphaFunc);
		rw::SetRenderState(rw::ALPHATESTREF, l->alphaRef);
		rw::SetRenderState(rw::SRCBLEND, l->srcBlend);
		rw::SetRenderState(rw::DESTBLEND, l->destBlend);
		rw::SetRenderState(rw::FOGENABLE, l->fog);
		setTexture(0, l->texture);
		rw::SetRenderState(rw::VERTEXALPHA, l->vertexAlpha);
		setupVertexInput(l->header);
		(l->atlas ? atlasShader : defaultShader)->use();
		laterRecord = l->record;
		// with the PlayStation 2's alpha test, in its two passes (both with this record)
		drawInst(l->header, &l->run);
	}
	laterRecord = nil;
	numLater = 0;

	for(uint32 k = 0; k < nelem(states); k++)
		rw::SetRenderState(states[k], before[k]);
	setTextureStage(&stage);
	if(shader)
		shader->use();
}

void
invalidateVertexInput(void)
{
	boundVertices = nil;
	boundIndices = nil;
	boundStride = 0;
	boundTopology = ~0u;
	instanceBound = 0;
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

void
countDraw(uint32 n)
{
	if(boundTopology == GL_TRIANGLE_STRIP)
		measure.triangles += n > 2 ? n - 2 : 0;
	else if(boundTopology != GL_POINTS && boundTopology != GL_LINES && boundTopology != GL_LINE_STRIP)
		measure.triangles += n/3;
}

bool32
readyToDraw(void)
{
	if(currentShader == nil)
		return 0;
	bindShaders();	// they may have been in use before the context was begun anew
	flushCache();
	return setInputLayout(currentFormat, currentAttribs, currentNumAttribs);
}

// Geometry is drawn: with the form of the shader in use that reads the draw's own data with
// the vertices, where it has one, and that data put where it is read from
static bool32
readyToDrawGeometry(bool32 *instanced)
{
	geometryDraw = 1;
	bool32 ready = readyToDraw();
	*instanced = instanceVectorsInUse() != 0;
	if(ready && *instanced){
		if(laterRecord)
			ready = placeInstance(laterRecord);
		else{
			float record[INSTANCE_BYTES/4];
			fillInstance(record);
			ready = placeInstance(record);
		}
	}
	geometryDraw = 0;
	return ready;
}

void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	bool32 instanced;
	countDraw(inst->numIndex);
	if(perf.enabled){
		uint64 t0 = perfNow();
		bool32 ready = readyToDrawGeometry(&instanced);
		uint64 t1 = perfNow();
		if(ready){
			if(instanced)
				d3d.context->DrawIndexedInstanced(inst->numIndex, 1, inst->offset/2, 0, 0);
			else
				d3d.context->DrawIndexed(inst->numIndex, inst->offset/2, 0);
		}
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
	if(readyToDrawGeometry(&instanced)){
		if(instanced)
			d3d.context->DrawIndexedInstanced(inst->numIndex, 1, inst->offset/2, 0, 0);
		else
			d3d.context->DrawIndexed(inst->numIndex, inst->offset/2, 0);
	}
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
	// what may be seen through after what may not (see above): where the shaders are chosen
	// by the lights (2) or by the lights and the alpha test (0), which is the same choice
	// here; not where one shader draws everything (1) or nothing is discarded (3)
	bool32 later = opaqueFirst && (fewerPrograms == 0 || fewerPrograms == 2);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	// Geometry whose vertices say where their texture is in an atlas (atlasInstanceCB): the
	// meshes that follow one another and differ in nothing but their texture, which the
	// atlas has too, are one draw. The materials are looked at here, each time: their
	// colours may have been changed since the last.
	perf.pipe = header->atlasInput || header->numAtlased ? 1 : 0;
	// REVC_ATLAS=fit (pipe.cpp): the meshes that are in an atlas have its coordinates, and are
	// drawn by the shaders everything else is drawn by; those that follow one another and
	// differ in nothing but their texture are one draw, as below
	if(header->numAtlased && !header->atlasInput){
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
			bool32 test = fewerPrograms == 3 ? 0 : fewerPrograms == 0 || later ? getAlphaTest() : 1;
			if(later && !lit && test){
				keepForLater(header, &run, run.atlas && m->texture == run.atlasFor ? run.atlas : m->texture);
				i = j;
				continue;
			}
			if(lit)
				(test ? defaultShader_fullLight : defaultShader_fullLight_noAT)->use();
			else
				(test ? defaultShader : defaultShader_noAT)->use();
			drawInst(header, &run);
			i = j;
		}
		teardownVertexInput(header);
		return;
	}
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
			// as below: with the alpha test, or by whether anything of this mesh could fail it
			bool32 test = fewerPrograms == 3 ? 0 : fewerPrograms == 0 || later ? getAlphaTest() : 1;
			if(later && !lit && test){
				keepForLater(header, &run, run.atlas && m->texture == run.atlasFor ? run.atlas : m->texture);
				i = j;
				continue;
			}
			if(lit)
				(test ? atlasShader_fullLight : atlasShader_fullLight_noAT)->use();
			else
				(test ? atlasShader : atlasShader_noAT)->use();
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

		if(later){
			// by whether this mesh could fail the alpha test; what could, and has no lights
			// to be remembered with it, is drawn later
			bool32 test = getAlphaTest();
			if(!lit && test)
				keepForLater(header, inst, m->texture);
			else{
				if(lit)
					(test ? defaultShader_fullLight : defaultShader_fullLight_noAT)->use();
				else
					defaultShader_noAT->use();
				drawInst(header, inst);
			}
			inst++;
			continue;
		}
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
