// Geometry into buffers.
//
// As librw's OpenGL renderer does it (vendor/librw/src/gl/gl3pipe.cpp), with three things that
// are Direct3D's:
//  - Every vertex has a colour and a set of texture coordinates, whether its geometry has
//    them or not (black and nought, which is what OpenGL reads for an attribute that is not
//    there): Direct3D has no value for an input the vertices lack, and every shader reads
//    these two. Normals stay as they are: what has none is drawn by shaders that read none.
//  - Geometries of one vertex format lie side by side in a page, a vertex buffer of
//    pageVertices vertices and an index buffer, as before; a geometry's part of the page is
//    written by itself, and nothing else of the buffer is touched.
//  - A geometry whose vertices change (it is instanced again) has buffers of its own, made to
//    be written anew.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwanim.h"
#include "rwengine.h"
#include "rwplugins.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3impl.h"

#include "d3d11impl.h"

namespace rw {
namespace gl3 {

// ===[ Buffers shared between geometries (gl3Caps.sharedBuffers) ]===
// Free room is kept as two lists of extents, in order and joined where they touch; a page
// nobody uses is deleted.
//
// A page's buffers are dynamic ones, and a geometry's part is written into them where
// nothing is drawn from (Map with NO_OVERWRITE): the device neither waits nor copies. A part
// of a buffer that is not dynamic, written with UpdateSubresource, took 12 to 20 ms each time
// on a Lumia 650 (Direct3D level 9_3) whatever its size: over 20 ms of every frame while
// driving, when the world's models come and go.
// So what a geometry leaves is not the next one's at once: a draw of the frame before may
// still wait to read it. It is free two frames later (takeBackLeft).

struct Extent
{
	uint32 start, count;
};

struct Page
{
	Page *next;
	ID3D11Buffer *vertices, *indices;
	uint32 format;
	uint32 stride;
	int32 numUsers;
	Extent *freeVertices, *freeIndices;
	int32 numFreeVertices, numFreeIndices;
	int32 maxFreeVertices, maxFreeIndices;
};

uint32 pageVertices = 65536;
#define PAGE_INDICES (pageVertices*3)
static Page *pages;

// what geometries have left, until it is free again
struct Left
{
	Page *page;
	uint32 firstVertex, numVertices, firstIndex, numIndices;
	uint32 frame;
};
static Left *left;
static int32 numLeft, maxLeft;
uint32 frameNumber;	// counted where a frame is shown (device.cpp)

static bool32
takeExtent(Extent *free, int32 *num, uint32 count, uint32 *start)
{
	for(int32 i = 0; i < *num; i++)
		if(free[i].count >= count){
			*start = free[i].start;
			free[i].start += count;
			free[i].count -= count;
			if(free[i].count == 0){
				memmove(&free[i], &free[i+1], (*num-i-1)*sizeof(Extent));
				(*num)--;
			}
			return 1;
		}
	return 0;
}

static void
giveExtent(Extent **free, int32 *num, int32 *max, uint32 start, uint32 count)
{
	Extent *f = *free;
	int32 i;
	for(i = 0; i < *num && f[i].start < start; i++)
		;
	bool32 before = i > 0 && f[i-1].start + f[i-1].count == start;
	bool32 after = i < *num && start + count == f[i].start;
	if(before && after){
		f[i-1].count += count + f[i].count;
		memmove(&f[i], &f[i+1], (*num-i-1)*sizeof(Extent));
		(*num)--;
	}else if(before)
		f[i-1].count += count;
	else if(after){
		f[i].start = start;
		f[i].count += count;
	}else{
		if(*num == *max){
			*max = *max ? *max*2 : 16;
			f = *free = rwResizeT(Extent, f, *max, MEMDUR_EVENT | ID_GEOMETRY);
		}
		memmove(&f[i+1], &f[i], (*num-i)*sizeof(Extent));
		f[i].start = start;
		f[i].count = count;
		(*num)++;
	}
}

static ID3D11Buffer*
makeBuffer(uint32 size, uint32 bind, const void *data, bool32 rewritten)
{
	D3D11_BUFFER_DESC bd;
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = size;
	bd.BindFlags = bind;
	bd.Usage = rewritten ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
	bd.CPUAccessFlags = rewritten ? D3D11_CPU_ACCESS_WRITE : 0;
	D3D11_SUBRESOURCE_DATA initial = { data, 0, 0 };
	ID3D11Buffer *buffer = nil;
	if(data)
		d3dPerf.uploadBytes += size;
	HRESULT hr = d3d.device->CreateBuffer(&bd, data ? &initial : nil, &buffer);
	if(FAILED(hr))
		logf("d3d: no buffer of %u bytes, 0x%08X", (unsigned)size, (unsigned)hr);
	return buffer;
}

// A part of a page's buffer that nothing is drawn from: its bytes, or (base not nil) 16-bit
// indices with a number added to each
static void
writeBuffer(ID3D11Buffer *buffer, uint32 at, uint32 size, const void *data, const uint32 *base = nil)
{
	if(d3d.threaded){
		// by the drawing thread, before it makes this frame's calls: a context that only
		// keeps calls cannot write into a buffer without discarding it
		d3dPerf.uploadBytes += size;
		queueBufferWrite(buffer, at, size, data, base);
		return;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = d3d.context->Map(buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
	if(FAILED(hr)){
		static int said;
		if(said++ < 4)
			logf("d3d: a shared buffer could not be written, 0x%08X", (unsigned)hr);
		return;
	}
	d3dPerf.uploadBytes += size;
	if(base){
		// written in order, and never read: the memory may be the device's
		const uint16 *from = (const uint16*)data;
		uint16 *to = (uint16*)((uint8*)mapped.pData + at);
		uint32 add = *base;
		for(uint32 i = 0; i < size/2; i++)
			to[i] = (uint16)(from[i] + add);
	}else
		memcpy((uint8*)mapped.pData + at, data, size);
	d3d.context->Unmap(buffer, 0);
}

static Page*
createPage(InstanceDataHeader *header)
{
	Page *p = rwNewT(Page, 1, MEMDUR_EVENT | ID_GEOMETRY);
	memset(p, 0, sizeof(Page));
	p->format = header->d3dFormat;
	p->stride = header->attribDesc[0].stride;
	p->vertices = makeBuffer(pageVertices*p->stride, D3D11_BIND_VERTEX_BUFFER, nil, 1);
	p->indices = makeBuffer(PAGE_INDICES*2, D3D11_BIND_INDEX_BUFFER, nil, 1);
	if(p->vertices == nil || p->indices == nil){
		release(p->vertices);
		release(p->indices);
		rwFree(p);
		return nil;
	}

	giveExtent(&p->freeVertices, &p->numFreeVertices, &p->maxFreeVertices, 0, pageVertices);
	giveExtent(&p->freeIndices, &p->numFreeIndices, &p->maxFreeIndices, 0, PAGE_INDICES);
	p->next = pages;
	pages = p;
	return p;
}

static void
destroyPage(Page *p)
{
	Page **link;
	for(link = &pages; *link != p; link = &(*link)->next)
		;
	*link = p->next;
	// what was left in it is nobody's now
	int32 kept = 0;
	for(int32 i = 0; i < numLeft; i++)
		if(left[i].page != p)
			left[kept++] = left[i];
	numLeft = kept;
	invalidateVertexInput();
	release(p->indices);
	release(p->vertices);
	rwFree(p->freeVertices);
	rwFree(p->freeIndices);
	rwFree(p);
}

// What was left two frames ago or earlier is free: no draw that is still to be made reads it.
// Three with a drawing thread, which is a frame behind.
static void
takeBackLeft(void)
{
	int32 kept = 0;
	uint32 frames = d3d.threaded ? 3 : 2;
	for(int32 i = 0; i < numLeft; i++){
		Left *l = &left[i];
		if(frameNumber - l->frame < frames){
			left[kept++] = *l;
			continue;
		}
		Page *p = l->page;
		giveExtent(&p->freeVertices, &p->numFreeVertices, &p->maxFreeVertices, l->firstVertex, l->numVertices);
		giveExtent(&p->freeIndices, &p->numFreeIndices, &p->maxFreeIndices, l->firstIndex, l->numIndices);
	}
	numLeft = kept;
}

static bool32
enterPage(InstanceDataHeader *header)
{
	if(!gl3Caps.sharedBuffers || header->totalNumVertex == 0 || header->totalNumIndex == 0 ||
	   header->totalNumVertex > pageVertices || header->totalNumIndex > PAGE_INDICES)
		return 0;

	takeBackLeft();

	Page *p;
	uint32 firstVertex = 0, firstIndex = 0;
	for(p = pages; p; p = p->next){
		if(p->format != header->d3dFormat || p->stride != header->attribDesc[0].stride)
			continue;
		if(!takeExtent(p->freeVertices, &p->numFreeVertices, header->totalNumVertex, &firstVertex))
			continue;
		if(takeExtent(p->freeIndices, &p->numFreeIndices, header->totalNumIndex, &firstIndex))
			break;
		giveExtent(&p->freeVertices, &p->numFreeVertices, &p->maxFreeVertices,
			firstVertex, header->totalNumVertex);
	}
	if(p == nil){
		p = createPage(header);
		if(p == nil)
			return 0;
		takeExtent(p->freeVertices, &p->numFreeVertices, header->totalNumVertex, &firstVertex);
		takeExtent(p->freeIndices, &p->numFreeIndices, header->totalNumIndex, &firstIndex);
	}

	writeBuffer(p->vertices, firstVertex*p->stride, header->totalNumVertex*p->stride, header->vertexBuffer);
	// the indices count from the page's first vertex, not from the geometry's
	writeBuffer(p->indices, firstIndex*2, header->totalNumIndex*2, header->indexBuffer, &firstVertex);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++, inst++)
		inst->offset += firstIndex*2;
	header->d3dVertices = p->vertices;
	header->d3dIndices = p->indices;
	header->vbo = header->ibo = 1;
	header->page = p;
	header->firstVertex = firstVertex;
	header->firstIndex = firstIndex;
	p->numUsers++;
	return 1;
}

static void
leavePage(InstanceDataHeader *header)
{
	Page *p = (Page*)header->page;
	if(numLeft == maxLeft){
		maxLeft = maxLeft ? maxLeft*2 : 64;
		left = left ? rwResizeT(Left, left, maxLeft, MEMDUR_EVENT | ID_GEOMETRY)
		            : rwNewT(Left, maxLeft, MEMDUR_EVENT | ID_GEOMETRY);
	}
	Left *l = &left[numLeft++];
	l->page = p;
	l->firstVertex = header->firstVertex;
	l->numVertices = header->totalNumVertex;
	l->firstIndex = header->firstIndex;
	l->numIndices = header->totalNumIndex;
	l->frame = frameNumber;
	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++, inst++)
		inst->offset -= header->firstIndex*2;
	header->page = nil;
	header->d3dVertices = header->d3dIndices = nil;
	header->vbo = header->ibo = header->vao = 0;
	header->firstVertex = header->firstIndex = 0;
	if(--p->numUsers == 0)
		destroyPage(p);
}

void
uploadInstanceData(InstanceDataHeader *header, bool32 reinstance)
{
	header->d3dFormat = formatOf(header->attribDesc, header->numAttribs);
	PerfScope timed(&d3dPerf.uploadNs);
	if(!reinstance && enterPage(header))
		return;
	if(header->page)
		leavePage(header);
	uint32 size = header->totalNumVertex*header->attribDesc[0].stride;
	if(size == 0 || header->totalNumIndex == 0)
		return;
	if(header->d3dIndices == nil)
		header->d3dIndices = makeBuffer(header->totalNumIndex*2, D3D11_BIND_INDEX_BUFFER, header->indexBuffer, 0);
	ID3D11Buffer *vertices = (ID3D11Buffer*)header->d3dVertices;
	if(vertices == nil){
		// a geometry that is instanced a second time is one that changes: written anew from
		// here on
		vertices = makeBuffer(size, D3D11_BIND_VERTEX_BUFFER, reinstance ? nil : header->vertexBuffer, reinstance);
		header->d3dVertices = vertices;
		header->vao = reinstance;	// its vertex buffer can be rewritten
		if(!reinstance){
			header->vbo = header->ibo = 1;
			return;
		}
	}else if(!header->vao){
		// made to stay as it was, and now it changes
		invalidateVertexInput();
		release(vertices);
		vertices = makeBuffer(size, D3D11_BIND_VERTEX_BUFFER, nil, 1);
		header->d3dVertices = vertices;
		header->vao = 1;
	}
	header->vbo = header->ibo = 1;
	if(vertices){
		D3D11_MAPPED_SUBRESOURCE mapped;
		if(SUCCEEDED(d3d.context->Map(vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))){
			memcpy(mapped.pData, header->vertexBuffer, size);
			d3d.context->Unmap(vertices, 0);
			d3dPerf.uploadBytes += size;
		}
	}
}

void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_GL3)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	if(header->page)
		leavePage(header);
	ID3D11Buffer *vertices = (ID3D11Buffer*)header->d3dVertices;
	ID3D11Buffer *indices = (ID3D11Buffer*)header->d3dIndices;
	if(vertices || indices)
		invalidateVertexInput();
	release(vertices);
	release(indices);
	rwFree(header->indexBuffer);
	rwFree(header->vertexBuffer);
	rwFree(header->attribDesc);
	rwFree(header->inst);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static InstanceDataHeader*
instanceMesh(rw::ObjPipeline *rwpipe, Geometry *geo)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_GL3;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == 1 ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
	header->totalNumVertex = geo->numVertices;
	header->totalNumIndex = meshh->totalIndices;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	// Where meshes may be drawn several as one (gl3Caps.atlases) strips are laid out for
	// it: between two of them the last index of the one and the first of the other, which
	// make triangles of no area, and that first once more where the strip would otherwise
	// begin on an odd place and have its triangles turned round. Each mesh with what
	// follows it is then an even number of indices, wherever such a draw begins.
	bool32 joinStrips = gl3Caps.atlases && header->primType == GL_TRIANGLE_STRIP;
	header->indexBuffer = rwNewT(uint16, header->totalNumIndex + 3*header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);
	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 offset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		assert(inst->minVert != 0xFFFFFFFF);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->program = 0;
		inst->offset = offset;
		inst->atlas = nil;
		inst->atlasFor = nil;
		memcpy((uint8*)header->indexBuffer + inst->offset,
		       mesh->indices, inst->numIndex*2);
		offset += inst->numIndex*2;
		if(joinStrips && i+1 < header->numMeshes && mesh->numIndices > 0 && mesh[1].numIndices > 0){
			uint16 *join = (uint16*)((uint8*)header->indexBuffer + offset);
			join[0] = mesh->indices[mesh->numIndices-1];
			join[1] = mesh[1].indices[0];
			join[2] = mesh[1].indices[0];
			offset += (mesh->numIndices & 1 ? 3 : 2)*2;
		}
		mesh++;
		inst++;
	}
	header->totalNumIndex = offset/2;
	header->numAtlased = 0;
	header->atlasInput = 0;

	header->vertexBuffer = nil;
	header->numAttribs = 0;
	header->attribDesc = nil;
	// the buffers come with the vertices, once their format is known: uploadInstanceData()
	header->ibo = 0;
	header->vbo = 0;
	header->vao = 0;
	header->d3dVertices = nil;
	header->d3dIndices = nil;
	header->d3dFormat = 0;
	header->page = nil;
	header->firstVertex = 0;
	header->firstIndex = 0;

	return header;
}

static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// don't try to (re)instance native data
	if(geo->flags & Geometry::NATIVE)
		return;

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(geo->instData){
		// Already have instanced data, so check if we have to reinstance
		assert(header->platform == PLATFORM_GL3);
		if(header->serialNumber != geo->meshHeader->serialNum){
			// Mesh changed, so reinstance everything
			freeInstanceData(geo);
		}
	}

	// no instance or complete reinstance
	if(geo->instData == nil){
		PerfScope timed(&d3dPerf.instanceNs);
		geo->instData = instanceMesh(rwpipe, geo);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 0);
		d3dPerf.instances++;
	}else if(geo->lockedSinceInst){
		PerfScope timed(&d3dPerf.instanceNs);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 1);
		d3dPerf.reinstances++;
	}

	geo->lockedSinceInst = 0;
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	assert(0 && "can't uninstance");
}

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	PerfScope timed(&d3dPerf.renderNs);
	pipe->instance(atomic);
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_GL3);
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_GL3);
	this->impl.instance = gl3::instance;
	this->impl.uninstance = gl3::uninstance;
	this->impl.render = gl3::render;
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

// Which meshes are drawn from an atlas: those whose texture is in one. For each vertex the
// mesh that has it, or nil where no mesh is to be: none, or a vertex that two meshes use
// with two textures (it can say only one place).
static int32*
findAtlases(InstanceDataHeader *header)
{
	InstanceData *inst = header->inst;
	uint32 i;
	header->numAtlased = 0;
	for(i = 0; i < header->numMeshes; i++){
		Texture *tex = inst[i].material->texture;
		if(tex && tex->raster && GETGL3RASTEREXT(tex->raster)->atlas){
			inst[i].atlas = GETGL3RASTEREXT(tex->raster)->atlas;
			inst[i].atlasFor = tex;
			header->numAtlased++;
		}
	}
	if(header->numAtlased == 0)
		return nil;
	int32 *owner = rwNewT(int32, header->totalNumVertex, MEMDUR_FUNCTION | ID_GEOMETRY);
	for(i = 0; i < header->totalNumVertex; i++)
		owner[i] = -1;
	bool32 clash = 0;
	for(i = 0; i < header->numMeshes; i++){
		uint16 *index = (uint16*)((uint8*)header->indexBuffer + inst[i].offset);
		for(uint32 k = 0; k < inst[i].numIndex; k++){
			int32 o = owner[index[k]];
			if(o < 0)
				owner[index[k]] = i;
			else if(inst[o].atlasFor != inst[i].atlasFor)
				clash = 1;
		}
	}
	if(clash){
		for(i = 0; i < header->numMeshes; i++)
			inst[i].atlas = inst[i].atlasFor = nil;
		header->numAtlased = 0;
		rwFree(owner);
		return nil;
	}
	return owner;
}

static AttribDesc*
findAttrib(AttribDesc *attribs, uint32 index)
{
	AttribDesc *a;
	for(a = attribs; a->index != index; a++)
		;
	return a;
}

// ===[ Atlases without wrapping in the pixel shader (REVC_ATLAS=fit) ]===
// A mesh drawn from an atlas has its texture coordinates wrapped inside its texture's part of
// the atlas by the pixel shader, which a GPU of Direct3D's level 9 pays for at every pixel: a
// Lumia 650 draws a picture's worth of pixels that way in 5.2 ms and from a texture of its
// own in 3.1 (bench/d3dbench.cpp, REVC_BENCH=fill), and a texture of its own can have its
// smaller copies. But most meshes of a building do not repeat their texture at all. So this
// way only those are drawn from an atlas, with their coordinates moved into their texture's
// part of it here, once, and by the same shaders as everything else; a mesh that does repeat
// its texture is drawn from that texture, by itself, as without atlases.
static int32 atlasFit = -1;

bool32
atlasesFit(void)
{
	if(atlasFit < 0){
		const char *value = getenv("REVC_ATLAS");
		atlasFit = value != nil && strcmp(value, "fit") == 0;
	}
	return atlasFit;
}

struct MeshFit
{
	bool32 fits;
	float cellU, cellV;	// the whole numbers its coordinates lie above, as they are sampled (u, 1-v)
};

// The meshes whose textures are in an atlas and whose coordinates lie within one repeat of
// the texture: their vertices' coordinates become the atlas's. The others are not drawn from
// an atlas. The vertices are in the buffer already.
static void
fitIntoAtlases(InstanceDataHeader *header)
{
	InstanceData *inst = header->inst;
	uint32 n = header->numMeshes, i, k;
	AttribDesc *a = findAttrib(header->attribDesc, ATTRIB_TEXCOORDS0);
	uint8 *coords = header->vertexBuffer + a->offset;
	uint32 stride = a->stride;

	header->numAtlased = 0;
	MeshFit *fit = rwNewT(MeshFit, n, MEMDUR_FUNCTION | ID_GEOMETRY);
	bool32 any = 0;
	for(i = 0; i < n; i++){
		fit[i].fits = 0;
		inst[i].atlas = inst[i].atlasFor = nil;
		Texture *tex = inst[i].material->texture;
		if(tex == nil || tex->raster == nil || inst[i].numIndex == 0)
			continue;
		Gl3Raster *natras = GETGL3RASTEREXT(tex->raster);
		if(natras->atlas == nil)
			continue;
		float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
		uint16 *index = (uint16*)((uint8*)header->indexBuffer + inst[i].offset);
		for(k = 0; k < inst[i].numIndex; k++){
			float uv[2];
			memcpy(uv, coords + index[k]*stride, 8);
			float u = uv[0], v = 1.0f - uv[1];
			if(u < minU) minU = u;
			if(u > maxU) maxU = u;
			if(v < minV) minV = v;
			if(v > maxV) maxV = v;
		}
		// two texels over the edge are there in the atlas (the texture's far side again)
		float tolU = 2.0f/tex->raster->width, tolV = 2.0f/tex->raster->height;
		fit[i].cellU = floorf(minU + tolU);
		fit[i].cellV = floorf(minV + tolV);
		if(minU < fit[i].cellU - tolU || maxU > fit[i].cellU + 1.0f + tolU ||
		   minV < fit[i].cellV - tolV || maxV > fit[i].cellV + 1.0f + tolV)
			continue;
		fit[i].fits = 1;
		inst[i].atlasFor = tex;
		any = 1;
	}
	if(any){
		// A vertex two meshes use can have one pair of coordinates: the two have to agree,
		// or neither is drawn from an atlas (and then those that agreed with either)
		int32 *first = rwNewT(int32, header->totalNumVertex, MEMDUR_FUNCTION | ID_GEOMETRY);
		bool32 changed;
		do{
			changed = 0;
			for(k = 0; k < header->totalNumVertex; k++)
				first[k] = -1;
			for(i = 0; i < n; i++){
				uint16 *index = (uint16*)((uint8*)header->indexBuffer + inst[i].offset);
				for(k = 0; k < inst[i].numIndex; k++){
					int32 j = first[index[k]];
					if(j < 0){
						first[index[k]] = i;
						continue;
					}
					if((uint32)j == i || (!fit[i].fits && !fit[j].fits))
						continue;
					if(fit[i].fits && fit[j].fits && inst[i].atlasFor == inst[j].atlasFor &&
					   fit[i].cellU == fit[j].cellU && fit[i].cellV == fit[j].cellV)
						continue;
					fit[i].fits = fit[j].fits = 0;
					changed = 1;
				}
			}
		}while(changed);

		uint8 *done = rwNewT(uint8, header->totalNumVertex, MEMDUR_FUNCTION | ID_GEOMETRY);
		memset(done, 0, header->totalNumVertex);
		for(i = 0; i < n; i++){
			if(!fit[i].fits){
				inst[i].atlasFor = nil;
				continue;
			}
			Gl3Raster *natras = GETGL3RASTEREXT(inst[i].atlasFor->raster);
			inst[i].atlas = natras->atlas;
			header->numAtlased++;
			uint16 *index = (uint16*)((uint8*)header->indexBuffer + inst[i].offset);
			for(k = 0; k < inst[i].numIndex; k++){
				uint32 v = index[k];
				if(done[v])
					continue;
				done[v] = 1;
				float uv[2];
				memcpy(uv, coords + v*stride, 8);
				// as it is sampled: (u, 1-v), into the texture's part of the atlas
				float su = natras->atlasRect[0] + (uv[0] - fit[i].cellU)*natras->atlasRect[2];
				float sv = natras->atlasRect[1] + ((1.0f - uv[1]) - fit[i].cellV)*natras->atlasRect[3];
				uv[0] = su;
				uv[1] = 1.0f - sv;
				memcpy(coords + v*stride, uv, 8);
			}
		}
		rwFree(done);
		rwFree(first);
	}
	rwFree(fit);
}

// A geometry's vertices as they are drawn. flags: INSTANCE_ATLAS, with where each vertex's
// texture is in an atlas (where there are atlases); INSTANCE_SKIN, with normals whether it has
// them or not, its bones' weights and their indices.
void
instanceVertices(Geometry *geo, InstanceDataHeader *header, bool32 reinstance, uint32 flags)
{
	AttribDesc *attribs, *a;

	bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	bool hasNormals = !!(geo->flags & Geometry::NORMALS);
	bool skinned = !!(flags & INSTANCE_SKIN);
	bool withNormals = hasNormals || skinned;
	// the first set is always there
	int32 numTexCoordSets = geo->numTexCoordSets > 0 ? geo->numTexCoordSets : 1;

	if(!reinstance){
		AttribDesc tmpAttribs[14];
		uint32 stride;
		header->atlasInput = (flags & INSTANCE_ATLAS) && gl3Caps.atlases && geo->numTexCoordSets > 0 && !atlasesFit();
		int32 *atlasOwner = header->atlasInput ? findAtlases(header) : nil;

		//
		// Create attribute descriptions
		//
		a = tmpAttribs;
		stride = 0;

		// Positions
		a->index = ATTRIB_POS;
		a->size = 3;
		a->type = GL_FLOAT;
		a->normalized = GL_FALSE;
		a->offset = stride;
		stride += 12;
		a++;

		// Normals
		if(withNormals){
			a->index = ATTRIB_NORMAL;
			a->size = 3;
			a->type = GL_FLOAT;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 12;
			a++;
		}

		// Prelighting, or black
		a->index = ATTRIB_COLOR;
		a->size = 4;
		a->type = GL_UNSIGNED_BYTE;
		a->normalized = GL_TRUE;
		a->offset = stride;
		stride += 4;
		a++;

		// Texture coordinates
		for(int32 n = 0; n < numTexCoordSets; n++){
			a->index = ATTRIB_TEXCOORDS0+n;
			a->size = 2;
			a->type = GL_FLOAT;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 8;
			a++;
		}

		// Where the vertex's texture is in an atlas
		uint32 atlasOffset = stride;
		if(header->atlasInput){
			a->index = ATTRIB_ATLAS;
			a->size = 4;
			a->type = GL_FLOAT;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 16;
			a++;
		}

		if(skinned){
			// Weights
			a->index = ATTRIB_WEIGHTS;
			a->size = 4;
			a->type = GL_FLOAT;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 16;
			a++;

			// Indices
			a->index = ATTRIB_INDICES;
			a->size = 4;
			a->type = GL_UNSIGNED_BYTE;
			a->normalized = GL_FALSE;
			a->offset = stride;
			stride += 4;
			a++;
		}

		header->numAttribs = a - tmpAttribs;
		for(a = tmpAttribs; a != &tmpAttribs[header->numAttribs]; a++)
			a->stride = stride;
		header->attribDesc = rwNewT(AttribDesc, header->numAttribs, MEMDUR_EVENT | ID_GEOMETRY);
		memcpy(header->attribDesc, tmpAttribs,
		       header->numAttribs*sizeof(AttribDesc));

		//
		// Allocate vertex buffer
		//
		header->vertexBuffer = rwNewT(uint8, header->totalNumVertex*stride, MEMDUR_EVENT | ID_GEOMETRY);
		assert(header->d3dVertices == nil);
		// what the geometry does not have is nought, but for a colour's alpha
		memset(header->vertexBuffer, 0, header->totalNumVertex*stride);
		if(!isPrelit){
			a = findAttrib(header->attribDesc, ATTRIB_COLOR);
			for(uint32 v = 0; v < header->totalNumVertex; v++)
				header->vertexBuffer[v*stride + a->offset + 3] = 0xFF;
		}

		if(header->atlasInput){
			for(uint32 v = 0; v < header->totalNumVertex; v++){
				uint8 *rect = header->vertexBuffer + v*stride + atlasOffset;
				int32 o = atlasOwner ? atlasOwner[v] : -1;
				if(o >= 0 && header->inst[o].atlas)
					memcpy(rect, GETGL3RASTEREXT(header->inst[o].atlasFor->raster)->atlasRect, 16);
				else
					memset(rect, 0, 16);	// a size of nought: its own texture, as ever
			}
			if(atlasOwner)
				rwFree(atlasOwner);
		}
	}

	attribs = header->attribDesc;

	//
	// Fill vertex buffer
	//

	uint8 *verts = header->vertexBuffer;

	// Positions
	if(!reinstance || geo->lockedSinceInst&Geometry::LOCKVERTICES){
		a = findAttrib(attribs, ATTRIB_POS);
		instV3d(VERT_FLOAT3, verts + a->offset,
			geo->morphTargets[0].vertices,
			header->totalNumVertex, a->stride);
	}

	// Normals
	if(hasNormals && (!reinstance || geo->lockedSinceInst&Geometry::LOCKNORMALS)){
		a = findAttrib(attribs, ATTRIB_NORMAL);
		instV3d(VERT_FLOAT3, verts + a->offset,
			geo->morphTargets[0].normals,
			header->totalNumVertex, a->stride);
	}

	// Prelighting
	if(isPrelit && (!reinstance || geo->lockedSinceInst&Geometry::LOCKPRELIGHT)){
		a = findAttrib(attribs, ATTRIB_COLOR);
		if(skinned)
			instColor(VERT_RGBA, verts + a->offset,
				  geo->colors,
				  header->totalNumVertex, a->stride);
		else{
			int n = header->numMeshes;
			InstanceData *inst = header->inst;
			while(n--){
				assert(inst->minVert != 0xFFFFFFFF);
				inst->vertexAlpha = instColor(VERT_RGBA,
					verts + a->offset + a->stride*inst->minVert,
					geo->colors + inst->minVert,
					inst->numVertices, a->stride);
				inst++;
			}
		}
	}

	// Texture coordinates
	for(int32 n = 0; n < geo->numTexCoordSets; n++){
		if(!reinstance || geo->lockedSinceInst&(Geometry::LOCKTEXCOORDS<<n)){
			a = findAttrib(attribs, ATTRIB_TEXCOORDS0+n);
			instTexCoords(VERT_FLOAT2, verts + a->offset,
				geo->texCoords[n],
				header->totalNumVertex, a->stride);
		}
	}

	// REVC_ATLAS=fit: the meshes that do not repeat their texture get the atlas's coordinates.
	// Vertices instanced again have the geometry's own: nothing of it is drawn from an atlas then.
	if((flags & INSTANCE_ATLAS) && gl3Caps.atlases && geo->numTexCoordSets > 0 && atlasesFit()){
		if(!reinstance)
			fitIntoAtlases(header);
		else if(geo->lockedSinceInst & Geometry::LOCKTEXCOORDS){
			for(uint32 i = 0; i < header->numMeshes; i++)
				header->inst[i].atlas = header->inst[i].atlasFor = nil;
			header->numAtlased = 0;
		}
	}

	if(skinned && !reinstance){
		Skin *skin = Skin::get(geo);

		// Weights
		a = findAttrib(attribs, ATTRIB_WEIGHTS);
		float *w = skin->weights;
		instV4d(VERT_FLOAT4, verts + a->offset,
			(V4d*)w,
			header->totalNumVertex, a->stride);

		// Indices
		a = findAttrib(attribs, ATTRIB_INDICES);
		// not really colors of course but what the heck
		instColor(VERT_RGBA, verts + a->offset,
			  (RGBA*)skin->indices,
			  header->totalNumVertex, a->stride);
	}

	uploadInstanceData(header, reinstance);
}

void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	instanceVertices(geo, header, reinstance, 0);
}

void
atlasInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	instanceVertices(geo, header, reinstance, INSTANCE_ATLAS);
}

void
defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	assert(0 && "can't uninstance");
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

}
}
