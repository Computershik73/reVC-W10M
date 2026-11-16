// Shaders and their uniforms.
//
// The shaders are compiled when the package is built (tools/make_shaders.py, from shaders/),
// for Direct3D's feature level 9_3 and so for every level above it; the bytecode and what
// the compiler's listing says of each are in d3d11_shaders.inc.
//
// Uniforms are as in librw's OpenGL renderer: registered by name, set by number, and handed
// to whichever shader is in use when something is drawn, if they have changed since that
// shader last had them. A shader's uniforms lie in a few constant buffers, grouped by how
// often they change (shaders/common.hlsli); a buffer is handed to the device again when one
// of its uniforms has changed, and only then. A vertex shader and a pixel shader are each one
// object however many programs are made of them, so going from one program to another that
// shares one of the two changes only the other.
//
// The callers ask for a program by the GLSL sources of the OpenGL renderer (Shader::create):
// the HLSL that stands for each is found by a hash of what they hand over.
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

#include "d3d11_shaders.inc"

// what the callers put in front of their GLSL: nothing here, and the two headers are never
// read (shaders/common.hlsli is the header)
const char *shaderDecl = "";
#include "shaders/header_vs.inc"
#include "shaders/header_fs.inc"

UniformRegistry uniformRegistry;
static char nameBuffer[(MAX_UNIFORMS + MAX_BLOCKS)*32];	// static because memory system isn't up yet when we register
static uint32 nameBufPtr;
static float uniformData[1024*4];
static uint32 dataPtr;

static int uniformTypesize[] = {
	0, 4, 4, 16
};

static ShaderStage *stages[nelem(shaderCodes)];

Shader *currentShader;
static ID3D11InputLayout *boundLayout;
static ID3D11VertexShader *boundVertex;
static ID3D11PixelShader *boundPixel;

static char*
shader_strdup(const char *name)
{
	size_t len = strlen(name)+1;
	char *s = &nameBuffer[nameBufPtr];
	nameBufPtr += (uint32)len;
	assert(nameBufPtr <= nelem(nameBuffer));
	memcpy(s, name, len);
	return s;
}

static void
placeUniform(ShaderStage *s, int32 id)
{
	const char *name = uniformRegistry.uniforms[id].name;
	s->places[id].buffer = -1;
	s->serialNums[id] = 0xFFFFFFFF;
	for(int32 b = 0; b < s->code->numBuffers; b++){
		const ShaderBuffer *buf = &s->code->buffers[b];
		for(int32 v = 0; v < buf->numVars; v++)
			if(strcmp(buf->vars[v].name, name) == 0){
				s->places[id].buffer = (int16)b;
				s->places[id].offset = buf->vars[v].offset;
				s->places[id].size = (uint16)buf->vars[v].size;
				return;
			}
	}
}

int32
registerUniform(const char *name, UniformType type, int32 num)
{
	int i;
	i = findUniform(name);
	if(type == UNIFORM_NA)
		num = 0;
	if(i >= 0){
		Uniform *u = &uniformRegistry.uniforms[i];
		assert(u->type == type);
		assert(u->num == num);
		return i;
	}
	if(uniformRegistry.numUniforms+1 >= MAX_UNIFORMS){
		assert(0 && "no space for uniform");
		return -1;
	}
	Uniform *u = &uniformRegistry.uniforms[uniformRegistry.numUniforms];
	u->name = shader_strdup(name);
	u->type = type;
	u->serialNum = 0;
	if(type == UNIFORM_NA){
		u->num = 0;
		u->data = nil;
	}else{
		u->num = num;
		u->data = &uniformData[dataPtr];
		dataPtr += uniformTypesize[type]*num;
		assert(dataPtr <= nelem(uniformData));
	}
	int32 id = uniformRegistry.numUniforms++;
	// the shaders that are there already may have it
	for(uint32 k = 0; k < nelem(stages); k++)
		if(stages[k])
			placeUniform(stages[k], id);
	return id;
}

int32
findUniform(const char *name)
{
	int i;
	for(i = 0; i < uniformRegistry.numUniforms; i++)
		if(strcmp(name, uniformRegistry.uniforms[i].name) == 0)
			return i;
	return -1;
}

int32
registerBlock(const char *name)
{
	int i;
	i = findBlock(name);
	if(i >= 0) return i;
	if(uniformRegistry.numBlocks+1 >= MAX_BLOCKS)
		return -1;
	uniformRegistry.blockNames[uniformRegistry.numBlocks] = shader_strdup(name);
	return uniformRegistry.numBlocks++;
}

int32
findBlock(const char *name)
{
	int i;
	for(i = 0; i < uniformRegistry.numBlocks; i++)
		if(strcmp(name, uniformRegistry.blockNames[i]) == 0)
			return i;
	return -1;
}

void
setUniform(int32 id, void *data)
{
	Uniform *u = &uniformRegistry.uniforms[id];
	assert(u->type != UNIFORM_NA);
	if(memcmp(u->data, data, uniformTypesize[u->type]*u->num * sizeof(float)) != 0){
		memcpy(u->data, data, uniformTypesize[u->type]*u->num * sizeof(float));
		u->serialNum++;
	}
}

static void
writeUniform(ShaderStage *s, int32 id, const void *data, uint32 size)
{
	UniformPlace *p = &s->places[id];
	if(p->buffer < 0)
		return;
	if(size > p->size)
		size = p->size;
	ShaderBufferState *buf = &s->buffers[p->buffer];
	if(memcmp(buf->shadow + p->offset, data, size) != 0){
		memcpy(buf->shadow + p->offset, data, size);
		buf->dirty = 1;
	}
}

// what glUniform is to the OpenGL renderer's callers: for the shaders in use, now
void
setUniformRaw(int id, const float *data, int numFloats)
{
	if(id < 0 || id >= uniformRegistry.numUniforms || currentShader == nil)
		return;
	writeUniform(currentShader->vertex, id, data, numFloats*sizeof(float));
	writeUniform(currentShader->pixel, id, data, numFloats*sizeof(float));
}

static void
flushStage(ShaderStage *s)
{
	int32 n = uniformRegistry.numUniforms;
	for(int32 i = 0; i < n; i++){
		if(s->places[i].buffer < 0)
			continue;
		Uniform *u = &uniformRegistry.uniforms[i];
		if(u->type == UNIFORM_NA || s->serialNums[i] == u->serialNum)
			continue;
		writeUniform(s, i, u->data, uniformTypesize[u->type]*u->num*sizeof(float));
		s->serialNums[i] = u->serialNum;
	}
	for(int32 b = 0; b < s->numBuffers; b++){
		ShaderBufferState *buf = &s->buffers[b];
		if(!buf->dirty)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped;
		if(SUCCEEDED(d3d.context->Map(buf->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))){
			memcpy(mapped.pData, buf->shadow, buf->size);
			d3d.context->Unmap(buf->buffer, 0);
		}
		buf->dirty = 0;
		perf.uniforms++;
	}
}

void
flushUniforms(void)
{
	flushStage(currentShader->vertex);
	flushStage(currentShader->pixel);
}

static ShaderStage*
getStage(const ShaderCode *code)
{
	int32 at = (int32)(code - shaderCodes);
	if(stages[at])
		return stages[at];
	ShaderStage *s = rwNewT(ShaderStage, 1, MEMDUR_EVENT | ID_DRIVER);
	memset(s, 0, sizeof(ShaderStage));
	s->code = code;
	HRESULT hr = code->vertex
		? d3d.device->CreateVertexShader(code->code, code->size, nil, &s->vertex)
		: d3d.device->CreatePixelShader(code->code, code->size, nil, &s->pixel);
	if(FAILED(hr)){
		logf("d3d: the shader %s was not taken, 0x%08X", code->name, (unsigned)hr);
		rwFree(s);
		return nil;
	}
	s->numBuffers = code->numBuffers;
	assert(s->numBuffers <= (int32)nelem(s->buffers));
	for(int32 b = 0; b < s->numBuffers; b++){
		ShaderBufferState *buf = &s->buffers[b];
		buf->size = code->buffers[b].size;
		buf->slot = code->buffers[b].slot;
		buf->shadow = rwNewT(uint8, buf->size, MEMDUR_EVENT | ID_DRIVER);
		memset(buf->shadow, 0, buf->size);
		buf->dirty = 1;
		D3D11_BUFFER_DESC bd;
		memset(&bd, 0, sizeof(bd));
		bd.ByteWidth = buf->size;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = d3d.device->CreateBuffer(&bd, nil, &buf->buffer);
		if(FAILED(hr))
			logf("d3d: no constant buffer of %u bytes for %s, 0x%08X", (unsigned)buf->size, code->name, (unsigned)hr);
		assert(buf->slot < nelem(s->bound));
		s->bound[buf->slot] = buf->buffer;
		if((int32)buf->slot + 1 > s->numSlots)
			s->numSlots = buf->slot + 1;
	}
	for(int32 i = 0; i < MAX_UNIFORMS; i++){
		s->places[i].buffer = -1;
		s->serialNums[i] = 0xFFFFFFFF;
	}
	for(int32 i = 0; i < uniformRegistry.numUniforms; i++)
		placeUniform(s, i);
	stages[at] = s;
	return s;
}

static void
destroyStage(ShaderStage *s)
{
	for(int32 i = 0; i < s->numLayouts; i++)
		release(s->layouts[i].layout);
	for(int32 b = 0; b < s->numBuffers; b++){
		release(s->buffers[b].buffer);
		rwFree(s->buffers[b].shadow);
	}
	release(s->vertex);
	release(s->pixel);
	rwFree(s);
}

static const ShaderCode*
findCode(const char *name)
{
	for(uint32 i = 0; i < nelem(shaderCodes); i++)
		if(strcmp(shaderCodes[i].name, name) == 0)
			return &shaderCodes[i];
	return nil;
}

// The defines and the body of a GLSL shader, as the OpenGL renderer's callers hand them over:
// everything but what they put in front (shaderDecl) and the header
static const ShaderCode*
findCode(const char **src, bool32 vertex)
{
	uint32 hash = 0x811C9DC5;
	const char *last = "";
	for(; *src; src++){
		if(*src == shaderDecl || *src == header_vert_src || *src == header_frag_src)
			continue;
		for(const uint8 *p = (const uint8*)*src; *p; p++)
			hash = (hash ^ *p)*0x01000193;
		last = *src;
	}
	for(uint32 i = 0; i < nelem(shaderCodes); i++){
		if(!!shaderCodes[i].vertex != !!vertex)
			continue;
		for(int32 h = 0; h < shaderCodes[i].numHashes; h++)
			if(shaderCodes[i].hashes[h] == hash)
				return &shaderCodes[i];
	}
	char start[100];
	strncpy(start, last, sizeof(start)-1);
	start[sizeof(start)-1] = '\0';
	for(char *c = start; *c; c++)
		if(*c == '\n' || *c == '\t')
			*c = ' ';
	logf("d3d: no Direct3D shader stands for this GLSL %s shader (tools/make_shaders.py has the list): %s...",
		vertex ? "vertex" : "fragment", start);
	return nil;
}

static Shader*
makeShader(const ShaderCode *vs, const ShaderCode *ps)
{
	if(vs == nil || ps == nil)
		return nil;
	ShaderStage *v = getStage(vs);
	ShaderStage *p = getStage(ps);
	if(v == nil || p == nil)
		return nil;
	Shader *sh = rwNewT(Shader, 1, MEMDUR_EVENT | ID_DRIVER);
	sh->vertex = v;
	sh->pixel = p;
	sh->uniformLocations = rwNewT(int32, MAX_UNIFORMS, MEMDUR_EVENT | ID_DRIVER);
	for(int32 i = 0; i < MAX_UNIFORMS; i++)
		sh->uniformLocations[i] = i;
	return sh;
}

Shader*
Shader::create(const char **vsrc, const char **fsrc)
{
	return makeShader(findCode(vsrc, 1), findCode(fsrc, 0));
}

Shader*
Shader::create(const char *vertexName, const char *pixelName)
{
	const ShaderCode *vs = findCode(vertexName);
	const ShaderCode *ps = findCode(pixelName);
	if(vs == nil || ps == nil)
		logf("d3d: no shader called %s", vs ? pixelName : vertexName);
	return makeShader(vs, ps);
}

void
Shader::use(void)
{
	if(currentShader == this)
		return;
	if(boundVertex != this->vertex->vertex){
		boundVertex = this->vertex->vertex;
		d3d.context->VSSetShader(boundVertex, nil, 0);
		if(this->vertex->numSlots)
			d3d.context->VSSetConstantBuffers(0, this->vertex->numSlots, this->vertex->bound);
	}
	if(boundPixel != this->pixel->pixel){
		boundPixel = this->pixel->pixel;
		d3d.context->PSSetShader(boundPixel, nil, 0);
		if(this->pixel->numSlots)
			d3d.context->PSSetConstantBuffers(0, this->pixel->numSlots, this->pixel->bound);
	}
	currentShader = this;
	perf.programs++;
	perf.changed |= 4;
}

void
Shader::destroy(void)
{
	if(currentShader == this)
		currentShader = nil;
	rwFree(this->uniformLocations);
	rwFree(this);
}

// ===[ The vertices' layout ]===

uint32
formatOf(AttribDesc *attribs, int32 numAttribs)
{
	uint32 hash = 0x811C9DC5;
	for(int32 i = 0; i < numAttribs; i++){
		uint32 fields[6] = { attribs[i].index, (uint32)attribs[i].type, (uint32)attribs[i].normalized,
			(uint32)attribs[i].size, attribs[i].stride, attribs[i].offset };
		for(int32 k = 0; k < 6; k++)
			hash = (hash ^ fields[k])*0x01000193;
	}
	return hash ? hash : 1;
}

static ID3D11InputLayout*
makeLayout(ShaderStage *s, AttribDesc *attribs, int32 numAttribs)
{
	static const char *semantics[] = {
		"POSITION", "NORMAL", "COLOR", "BLENDWEIGHT", "BLENDINDICES",
		"TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD"
	};
	D3D11_INPUT_ELEMENT_DESC elements[16];
	uint32 have = 0;
	int32 n = 0;
	for(int32 i = 0; i < numAttribs && n < 16; i++){
		AttribDesc *a = &attribs[i];
		if(a->index >= nelem(semantics))
			continue;
		D3D11_INPUT_ELEMENT_DESC *e = &elements[n++];
		memset(e, 0, sizeof(*e));
		e->SemanticName = semantics[a->index];
		e->SemanticIndex = a->index >= ATTRIB_TEXCOORDS0 ? a->index - ATTRIB_TEXCOORDS0 : 0;
		if(a->type == GL_FLOAT)
			e->Format = a->size == 1 ? DXGI_FORMAT_R32_FLOAT : a->size == 2 ? DXGI_FORMAT_R32G32_FLOAT :
				a->size == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
		else
			e->Format = DXGI_FORMAT_R8G8B8A8_UNORM;	// colours, and bone indices (the shader scales them back)
		e->AlignedByteOffset = a->offset;
		e->InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		have |= 1u << a->index;
	}
	// Direct3D has no value for an attribute that is not there
	if(s->code->attribs & ~have){
		logf("d3d: the shader %s reads vertex attributes these vertices do not have (it reads 0x%X, they have 0x%X): not drawn",
			s->code->name, (unsigned)s->code->attribs, (unsigned)have);
		return nil;
	}
	ID3D11InputLayout *layout = nil;
	HRESULT hr = d3d.device->CreateInputLayout(elements, n, s->code->code, s->code->size, &layout);
	if(FAILED(hr)){
		logf("d3d: no input layout for the shader %s, 0x%08X", s->code->name, (unsigned)hr);
		return nil;
	}
	return layout;
}

bool32
setInputLayout(uint32 format, AttribDesc *attribs, int32 numAttribs)
{
	ShaderStage *s = currentShader->vertex;
	ID3D11InputLayout *layout = nil;
	int32 i;
	for(i = 0; i < s->numLayouts; i++)
		if(s->layouts[i].format == format){
			layout = s->layouts[i].layout;
			break;
		}
	if(i == s->numLayouts){
		layout = makeLayout(s, attribs, numAttribs);
		if(s->numLayouts == (int32)nelem(s->layouts)){
			// more formats than a shader is ever drawn with: the oldest goes
			if(boundLayout == s->layouts[0].layout)
				boundLayout = nil;
			release(s->layouts[0].layout);
			memmove(&s->layouts[0], &s->layouts[1], (nelem(s->layouts)-1)*sizeof(InputLayout));
			s->numLayouts--;
		}
		s->layouts[s->numLayouts].format = format;
		s->layouts[s->numLayouts].layout = layout;
		s->numLayouts++;
	}
	if(layout == nil)
		return 0;
	if(layout != boundLayout){
		boundLayout = layout;
		d3d.context->IASetInputLayout(layout);
	}
	return 1;
}

void
openShaders(void)
{
	currentShader = nil;
	boundLayout = nil;
	boundVertex = nil;
	boundPixel = nil;
}

void
closeShaders(void)
{
	currentShader = nil;
	boundLayout = nil;
	boundVertex = nil;
	boundPixel = nil;
	for(uint32 i = 0; i < nelem(stages); i++)
		if(stages[i]){
			destroyStage(stages[i]);
			stages[i] = nil;
		}
}

}
}
