// Rasters: textures, what a camera draws into, depth buffers.
//
// A texture is as librw's OpenGL renderer has it for OpenGL ES (the game's converted textures,
// txd.img, are that renderer's): its rows bottom-up, RGBA in bytes or the PC game's own DXT
// blocks, which Direct3D takes as they are (BC1, BC2, BC3). A raster is locked into memory of
// its own and handed to Direct3D when it is unlocked.
//
// What is the same whatever draws it is as in vendor/librw/src/gl/gl3raster.cpp, from which it
// is taken: images to rasters and back, the textures made half as big to measure what their
// size costs, the atlases, the textures of txd.img read and written.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwengine.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3impl.h"

#include "d3d11impl.h"
#include "rwd3d11_host.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {

void decompressDXT1(uint8 *adst, int32 w, int32 h, uint8 *src);	// image.cpp
void decompressDXT3(uint8 *adst, int32 w, int32 h, uint8 *src);
void decompressDXT5(uint8 *adst, int32 w, int32 h, uint8 *src);

namespace gl3 {

int32 nativeRasterOffset;
ID3D11ShaderResourceView *whiteTexture;
static ID3D11Texture2D *whiteTextureItself;

static bool32
isDXT(int32 internalFormat)
{
	return internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || internalFormat == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
		internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT || internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}

static uint32
getLevelSize(Raster *raster, int32 level)
{
	int i;
	Gl3Raster *natras = GETGL3RASTEREXT(raster);

	int w = raster->originalWidth;
	int h = raster->originalHeight;
	int s = raster->originalStride;
	int minDim = isDXT(natras->internalFormat) ? 4 : 1;

	for(i = 0; i < level; i++){
		if(w > minDim){
			w /= 2;
			s /= 2;
		}
		if(h > minDim)
			h /= 2;
	}

	return s*h;
}

static int32
levelsDownToOne(int32 w, int32 h)
{
	int32 n = 1;
	while(w > 1 || h > 1){
		n++;
		if(w > 1) w /= 2;
		if(h > 1) h /= 2;
	}
	return n;
}

// Gl3Raster::type, which the OpenGL renderer has for glTexImage2D, says here how the texture
// is kept in Direct3D
enum
{
	KEPT_RGBA,		// bytes, as they are locked
	KEPT_BLOCKS,		// DXT blocks, as they are locked
	KEPT_DECODED,		// locked as DXT blocks, kept as bytes: a size Direct3D takes no blocks at
	KEPT_BGRA,		// drawn into
	KEPT_DEPTH,
};

// ===[ A texture's smaller copies (its levels below the first), made here: REVC_D3D_MIPS=1 ]===
// The PC game's textures have one level each. Seen from afar, a texture is then sampled many
// texels apart for every pixel, and a phone's GPU, which fetches a texture through a small
// cache, pays for each sample as for a new part of the texture: a Lumia 650 draws a picture
// full of a 512x512 texture at 16 texels to the pixel in 16.6 ms, and in 2.7 with the smaller
// copies there (bench/d3dbench.cpp, REVC_BENCH=fill). With REVC_D3D_MIPS=1 a texture that
// comes with one level gets the others when it is handed over: decoded once, averaged two by
// two (by its alpha where it has any, so that what is cut out does not darken its edge) and
// encoded again level by level, down to four pixels on its shorter side. It is then sampled
// between two levels (flushStates), which costs that GPU what one does.
//
// Not what the package does, yet. An atlas cannot have them (its textures would run into one
// another), and most of a street is drawn from atlases; without atlases a Lumia 650's GPU
// has 5 to 8 ms less to do in a frame at 1280x720 with them, which the draws that atlases
// save are worth as much as. And they are made on the game's thread: the frame in which a
// part of the city comes in took 110 to 140 ms where it took 60 to 100.
static int32 makeLevels = -1;
static bool32 noLevelsBelow;	// for a texture that is other textures side by side (an atlas)

void encodeDXT(uint8 *dst, uint8 *rgba, int32 w, int32 h, int32 dxt, bool32 hasAlpha);

static int32
levelsOfBlocks(int32 w, int32 h)
{
	int32 n = 1;
	while(w > 4 && h > 4){
		n++;
		w /= 2;
		h /= 2;
	}
	return n;
}

static bool32
wantsLevelsBelow(int32 w, int32 h, int32 levelsItHas)
{
	if(makeLevels < 0){
		const char *value = getenv("REVC_D3D_MIPS");
		makeLevels = value != nil && atoi(value) != 0;
	}
	return makeLevels && !noLevelsBelow && levelsItHas == 1 && w >= 8 && h >= 8 &&
		(w & (w-1)) == 0 && (h & (h-1)) == 0;
}

// The levels below the first of a compressed texture, from the first as it is locked
static void
makeBlockLevelsBelow(Raster *raster, ID3D11Texture2D *texture, int32 levels)
{
	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	int32 dxt = natras->internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ? 3 :
		natras->internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? 5 : 1;
	int32 blockBytes = dxt == 1 ? 8 : 16;
	int32 w = raster->width, h = raster->height;
	uint8 *rgba = (uint8*)rwMalloc(w*h*4, MEMDUR_FUNCTION | ID_DRIVER);
	uint8 *blocks = (uint8*)rwMalloc((w/8)*(h/8)*blockBytes, MEMDUR_FUNCTION | ID_DRIVER);
	switch(dxt){
	case 1: decompressDXT1(rgba, w, h, raster->pixels); break;
	case 3: decompressDXT3(rgba, w, h, raster->pixels); break;
	default: decompressDXT5(rgba, w, h, raster->pixels); break;
	}
	bool32 hasAlpha = natras->hasAlpha;
	for(int32 level = 1; level < levels; level++){
		int32 nw = w/2, nh = h/2;
		// into the same memory: a pixel is written where the ones it is made of have been read
		for(int32 y = 0; y < nh; y++)
			for(int32 x = 0; x < nw; x++){
				uint8 *q[4] = { &rgba[(y*2*w + x*2)*4], &rgba[(y*2*w + x*2 + 1)*4],
					&rgba[((y*2 + 1)*w + x*2)*4], &rgba[((y*2 + 1)*w + x*2 + 1)*4] };
				uint8 out[4];
				if(hasAlpha){
					int32 a = q[0][3] + q[1][3] + q[2][3] + q[3][3];
					for(int32 c = 0; c < 3; c++)
						out[c] = a ? (q[0][c]*q[0][3] + q[1][c]*q[1][3] + q[2][c]*q[2][3] + q[3][c]*q[3][3] + a/2)/a : 0;
					out[3] = (a + 2)/4;
				}else
					for(int32 c = 0; c < 4; c++)
						out[c] = (q[0][c] + q[1][c] + q[2][c] + q[3][c] + 2)/4;
				memcpy(&rgba[(y*nw + x)*4], out, 4);
			}
		encodeDXT(blocks, rgba, nw, nh, dxt, hasAlpha);
		d3d.context->UpdateSubresource(texture, level, nil, blocks, (nw/4)*blockBytes, 0);
		w = nw;
		h = nh;
	}
	rwFree(blocks);
	rwFree(rgba);
}

// The Direct3D texture of a raster that is sampled, with so many levels
static bool32
makeTexture(Raster *raster, int32 levels)
{
	PerfScope timed(&d3dPerf.textureNs);
	d3dPerf.textures++;
	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof(td));
	td.Width = raster->width;
	td.Height = raster->height;
	td.ArraySize = 1;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	if(natras->type == KEPT_BLOCKS)
		switch(natras->internalFormat){
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: td.Format = DXGI_FORMAT_BC2_UNORM; break;
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: td.Format = DXGI_FORMAT_BC3_UNORM; break;
		default: td.Format = DXGI_FORMAT_BC1_UNORM; break;
		}
	// Below feature level 10 a texture whose sides are not powers of two has one level and
	// is not wrapped
	bool32 powersOfTwo = (raster->width & (raster->width-1)) == 0 && (raster->height & (raster->height-1)) == 0;
	natras->d3dClamp = false;
	if(!powersOfTwo && d3d.level < D3D_FEATURE_LEVEL_10_0){
		levels = 1;
		natras->d3dClamp = true;
	}
	int32 most = levelsDownToOne(raster->width, raster->height);
	td.MipLevels = levels < 1 ? 1 : levels > most ? most : levels;

	ID3D11Texture2D *texture = nil;
	ID3D11ShaderResourceView *resource = nil;
	HRESULT hr = d3d.device->CreateTexture2D(&td, nil, &texture);
	if(SUCCEEDED(hr))
		hr = d3d.device->CreateShaderResourceView(texture, nil, &resource);
	if(FAILED(hr)){
		logf("d3d: no texture of %dx%d with %d levels (format %d), 0x%08X", (int)raster->width, (int)raster->height,
			(int)td.MipLevels, (int)td.Format, (unsigned)hr);
		release(texture);
		return 0;
	}
	natras->d3dTexture = texture;
	natras->d3dResource = resource;
	natras->texid = 1;
	natras->filterMode = 0;
	natras->addressU = 0;
	natras->addressV = 0;
	natras->maxAnisotropy = 1;
	return 1;
}

static Raster*
rasterCreateTexture(Raster *raster)
{
	if(raster->format & (Raster::PAL4 | Raster::PAL8)){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}

	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	switch(raster->format & 0xF00){
	case Raster::C8888:
		natras->hasAlpha = 1;
		raster->depth = 32;
		break;
	case Raster::C888:
		natras->hasAlpha = 0;
		raster->depth = 24;
		break;
	case Raster::C1555:
		natras->hasAlpha = 1;
		raster->depth = 16;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	// in bytes whatever was asked for, as the OpenGL renderer has it on OpenGL ES
	natras->internalFormat = GL_RGBA8;
	natras->format = GL_RGBA;
	natras->type = KEPT_RGBA;
	natras->bpp = 4;

	raster->stride = raster->width*natras->bpp;

	if(raster->format & Raster::MIPMAP)
		natras->numLevels = levelsDownToOne(raster->width, raster->height);
	natras->autogenMipmap = (raster->format & (Raster::MIPMAP|Raster::AUTOMIPMAP)) == (Raster::MIPMAP|Raster::AUTOMIPMAP);
	if(natras->autogenMipmap)
		natras->numLevels = 1;

	// the levels that are made from the first are the texture's all the same
	if(!makeTexture(raster, natras->autogenMipmap ? levelsDownToOne(raster->width, raster->height) : natras->numLevels)){
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	return raster;
}

// A texture a camera draws into. BGRA, as the picture itself is: the picture is copied into
// such textures.
static Raster*
rasterCreateCameraTexture(Raster *raster)
{
	if(raster->format & (Raster::PAL4 | Raster::PAL8)){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}

	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	switch(raster->format & 0xF00){
	case Raster::C8888:
	case Raster::C1555:
		natras->hasAlpha = 1;
		break;
	case Raster::C888:
	default:
		natras->hasAlpha = 0;
		break;
	}
	natras->internalFormat = GL_RGBA8;
	natras->format = GL_RGBA;
	natras->type = KEPT_BGRA;
	natras->bpp = 4;

	raster->stride = raster->width*natras->bpp;

	natras->autogenMipmap = 0;

	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof(td));
	td.Width = raster->width;
	td.Height = raster->height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ID3D11Texture2D *texture = nil;
	ID3D11ShaderResourceView *resource = nil;
	ID3D11RenderTargetView *target = nil;
	HRESULT hr = d3d.device->CreateTexture2D(&td, nil, &texture);
	if(SUCCEEDED(hr))
		hr = d3d.device->CreateShaderResourceView(texture, nil, &resource);
	if(SUCCEEDED(hr))
		hr = d3d.device->CreateRenderTargetView(texture, nil, &target);
	if(FAILED(hr)){
		logf("d3d: no texture of %dx%d to draw into, 0x%08X", (int)raster->width, (int)raster->height, (unsigned)hr);
		release(resource);
		release(texture);
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	natras->d3dTexture = texture;
	natras->d3dResource = resource;
	natras->d3dTarget = target;
	natras->d3dClamp = true;	// its sides are the screen's, and it is not a texture that repeats
	natras->texid = 1;
	natras->fbo = 1;
	natras->fboMate = nil;
	natras->filterMode = 0;
	natras->addressU = 0;
	natras->addressV = 0;
	natras->maxAnisotropy = 1;
	return raster;
}

static Raster*
rasterCreateCamera(Raster *raster)
{
	Gl3Raster *natras = GETGL3RASTEREXT(raster);

	// used for locking right now
	raster->format = Raster::C888;
	natras->internalFormat = GL_RGB8;
	natras->format = GL_RGB;
	natras->type = KEPT_RGBA;
	natras->hasAlpha = 0;
	natras->bpp = 3;

	natras->autogenMipmap = 0;

	natras->texid = 0;
	natras->fbo = 0;
	natras->fboMate = nil;

	return raster;
}

static Raster*
rasterCreateZbuffer(Raster *raster)
{
	Gl3Raster *natras = GETGL3RASTEREXT(raster);

	natras->internalFormat = GL_DEPTH_STENCIL;
	natras->format = GL_DEPTH_STENCIL;
	natras->type = KEPT_DEPTH;
	natras->autogenMipmap = 0;

	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof(td));
	td.Width = raster->width;
	td.Height = raster->height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	// REVC_D3D_DEPTH16=1: 16 bits of depth and no stencil, a measurement of what the depth
	// buffer's size costs a GPU that draws in tiles
	if(getenv("REVC_D3D_DEPTH16"))
		td.Format = DXGI_FORMAT_D16_UNORM;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ID3D11Texture2D *texture = nil;
	ID3D11DepthStencilView *depth = nil;
	HRESULT hr = d3d.device->CreateTexture2D(&td, nil, &texture);
	if(SUCCEEDED(hr))
		hr = d3d.device->CreateDepthStencilView(texture, nil, &depth);
	if(FAILED(hr)){
		logf("d3d: no depth buffer of %dx%d, 0x%08X", (int)raster->width, (int)raster->height, (unsigned)hr);
		release(texture);
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	natras->d3dTexture = texture;
	natras->d3dDepth = depth;
	natras->texid = 1;
	natras->fbo = 0;
	natras->fboMate = nil;

	return raster;
}

void
allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha)
{
	assert(raster->type == Raster::TEXTURE);

	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	switch(dxt){
	case 1:
		if(hasAlpha){
			natras->internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			natras->format = GL_RGBA;
		}else{
			natras->internalFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
			natras->format = GL_RGB;
		}
		// bogus, but stride*height should be the size of the image
		// 4x4 in 8 bytes
		raster->stride = raster->width/2;
		break;
	case 3:
		natras->internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
		natras->format = GL_RGBA;
		// 4x4 in 16 bytes
		raster->stride = raster->width;
		break;
	case 5:
		natras->internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
		natras->format = GL_RGBA;
		// 4x4 in 16 bytes
		raster->stride = raster->width;
		break;
	default:
		assert(0 && "invalid DXT format");
	}
	natras->hasAlpha = hasAlpha;
	natras->bpp = 2;
	raster->depth = 16;

	natras->isCompressed = 1;
	if(raster->format & Raster::MIPMAP)
		natras->numLevels = numLevels;
	natras->autogenMipmap = (raster->format & (Raster::MIPMAP|Raster::AUTOMIPMAP)) == (Raster::MIPMAP|Raster::AUTOMIPMAP);
	if(natras->autogenMipmap)
		natras->numLevels = 1;

	// Direct3D takes blocks for a texture whose sides are whole blocks; a smaller one is
	// kept decoded
	natras->type = (raster->width & 3) == 0 && (raster->height & 3) == 0 ? KEPT_BLOCKS : KEPT_DECODED;
	int32 levels = natras->numLevels;
	if(natras->type == KEPT_BLOCKS && wantsLevelsBelow(raster->width, raster->height, natras->numLevels)){
		// its file has one level: the others are made when that one is handed over
		natras->autogenMipmap = 1;
		levels = levelsOfBlocks(raster->width, raster->height);
	}else
		natras->autogenMipmap = 0;
	makeTexture(raster, levels);

	raster->originalStride = raster->stride;

	if(needToReadBackTextures){
		// Uh oh, need to keep a copy in cpu memory
		int32 i;
		int32 size = 0;
		for(i = 0; i < natras->numLevels; i++)
			size += getLevelSize(raster, i);
		uint8 *data = (uint8*)rwNew(sizeof(RasterLevels)+sizeof(RasterLevels::Level)*(natras->numLevels-1)+size,
			MEMDUR_EVENT | ID_DRIVER);
		RasterLevels *levels = (RasterLevels*)data;
		data += sizeof(RasterLevels)+sizeof(RasterLevels::Level)*(natras->numLevels-1);
		levels->numlevels = natras->numLevels;
		levels->format = 0;	// not needed
		for(i = 0; i < natras->numLevels; i++){
			levels->levels[i].data = data;
			levels->levels[i].size = getLevelSize(raster, i);
			levels->levels[i].width = 0;	// we don't need those here, maybe later...
			levels->levels[i].height = 0;
			data += levels->levels[i].size;
		}
		natras->backingStore = levels;
	}

	raster->flags &= ~Raster::DONTALLOCATE;
}

Raster*
rasterCreate(Raster *raster)
{
	Gl3Raster *natras = GETGL3RASTEREXT(raster);

	natras->isCompressed = 0;
	natras->hasAlpha = 0;
	natras->numLevels = 1;

	Raster *ret = raster;

	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		goto ret;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		goto ret;

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
		ret = rasterCreateTexture(raster);
		break;
	case Raster::CAMERATEXTURE:
		ret = rasterCreateCameraTexture(raster);
		break;
	case Raster::ZBUFFER:
		ret = rasterCreateZbuffer(raster);
		break;
	case Raster::CAMERA:
		ret = rasterCreateCamera(raster);
		break;

	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}

ret:
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = raster->pixels;
	return ret;
}

// A level of a texture out of Direct3D, by way of a copy that can be read. Not every texture
// can be asked for at every feature level: what cannot be read comes back as noughts.
static void
readLevel(Raster *raster, int32 level, uint8 *px, uint32 size)
{
	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	memset(px, 0, size);
	ID3D11Texture2D *texture = (ID3D11Texture2D*)natras->d3dTexture;
	if(texture == nil || natras->type == KEPT_DECODED)
		return;
	D3D11_TEXTURE2D_DESC td;
	texture->GetDesc(&td);
	if((uint32)level >= td.MipLevels)
		return;
	uint32 w = td.Width, h = td.Height;
	for(int32 i = 0; i < level; i++){
		if(w > 1) w /= 2;
		if(h > 1) h /= 2;
	}
	bool32 blocks = natras->type == KEPT_BLOCKS;
	td.Width = blocks ? (w + 3) & ~3 : w;
	td.Height = blocks ? (h + 3) & ~3 : h;
	td.MipLevels = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *copy = nil;
	if(FAILED(d3d.device->CreateTexture2D(&td, nil, &copy)))
		return;
	// as it is after everything drawn and written so far, on the device's own context
	d3dPerf.readBacks++;
	syncDevice();
	d3d.immediate->CopySubresourceRegion(copy, 0, 0, 0, 0, texture, level, nil);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(SUCCEEDED(d3d.immediate->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))){
		uint32 rows = blocks ? td.Height/4 : h;
		uint32 rowBytes = rows ? size/rows : 0;
		if(rowBytes > mapped.RowPitch)
			rowBytes = mapped.RowPitch;
		for(uint32 y = 0; y < rows; y++){
			const uint8 *from = (const uint8*)mapped.pData + y*mapped.RowPitch;
			uint8 *to = px + y*(size/rows);
			memcpy(to, from, rowBytes);
			if(natras->type == KEPT_BGRA)
				for(uint32 x = 0; x + 3 < rowBytes; x += 4){
					uint8 t = to[x];
					to[x] = to[x+2];
					to[x+2] = t;
				}
		}
		d3d.immediate->Unmap(copy, 0);
	}
	copy->Release();
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	Gl3Raster *natras GETGL3RASTEREXT(raster);
	uint8 *px;
	uint32 allocSz;
	int i;

	assert(raster->privateFlags == 0);

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		for(i = 0; i < level; i++){
			if(raster->width > 1){
				raster->width /= 2;
				raster->stride /= 2;
			}
			if(raster->height > 1)
				raster->height /= 2;
		}

		allocSz = getLevelSize(raster, level);
		px = (uint8*)rwMalloc(allocSz, MEMDUR_EVENT | ID_DRIVER);
		assert(raster->pixels == nil);
		raster->pixels = px;

		if(lockMode & Raster::LOCKREAD || !(lockMode & Raster::LOCKNOFETCH)){
			if(natras->backingStore){
				assert(level < natras->backingStore->numlevels);
				assert(allocSz >= natras->backingStore->levels[level].size);
				memcpy(px, natras->backingStore->levels[level].data, allocSz);
			}else
				readLevel(raster, level, px, allocSz);
		}

		raster->privateFlags = lockMode;
		break;

	case Raster::CAMERA:
		if(lockMode & Raster::PRIVATELOCK_WRITE)
			assert(0 && "can't lock framebuffer for writing");
		raster->width = d3d.width;
		raster->height = d3d.height;
		raster->stride = raster->width*natras->bpp;
		assert(natras->bpp == 3);
		allocSz = raster->height*raster->stride;
		px = (uint8*)rwMalloc(allocSz, MEMDUR_EVENT | ID_DRIVER);
		assert(raster->pixels == nil);
		raster->pixels = px;
		{
			// the picture, bottom row first, three bytes a pixel
			uint8 *rgba = (uint8*)rwMalloc(raster->width*raster->height*4, MEMDUR_FUNCTION | ID_DRIVER);
			if(rwd3d11_read_back(raster->width, raster->height, rgba)){
				int32 n = raster->width*raster->height;
				for(i = 0; i < n; i++){
					px[i*3+0] = rgba[i*4+0];
					px[i*3+1] = rgba[i*4+1];
					px[i*3+2] = rgba[i*4+2];
				}
			}else
				memset(px, 0, allocSz);
			rwFree(rgba);
		}

		raster->privateFlags = lockMode;
		break;

	default:
		assert(0 && "cannot lock this type of raster yet");
		return nil;
	}

	return px;
}

// The levels below the first, each the average of four pixels of the one above: what
// glGenerateMipmap is to the OpenGL renderer. RGBA in bytes.
static void
makeLevelsBelow(Raster *raster, ID3D11Texture2D *texture)
{
	D3D11_TEXTURE2D_DESC td;
	texture->GetDesc(&td);
	int32 w = raster->width, h = raster->height;
	uint8 *above = raster->pixels;
	uint8 *owned = nil;
	for(uint32 level = 1; level < td.MipLevels; level++){
		int32 nw = w > 1 ? w/2 : 1, nh = h > 1 ? h/2 : 1;
		uint8 *below = (uint8*)rwMalloc(nw*nh*4, MEMDUR_FUNCTION | ID_DRIVER);
		for(int32 y = 0; y < nh; y++)
			for(int32 x = 0; x < nw; x++){
				int32 x0 = x*2 < w ? x*2 : w-1, x1 = x*2+1 < w ? x*2+1 : w-1;
				int32 y0 = y*2 < h ? y*2 : h-1, y1 = y*2+1 < h ? y*2+1 : h-1;
				uint8 *q[4] = { &above[(y0*w + x0)*4], &above[(y0*w + x1)*4], &above[(y1*w + x0)*4], &above[(y1*w + x1)*4] };
				uint8 *to = &below[(y*nw + x)*4];
				// by its alpha, so that what is see-through does not colour what is not
				int32 a = q[0][3] + q[1][3] + q[2][3] + q[3][3];
				if(a == 4*255 || a == 0)
					for(int32 c = 0; c < 3; c++)
						to[c] = (q[0][c] + q[1][c] + q[2][c] + q[3][c] + 2)/4;
				else
					for(int32 c = 0; c < 3; c++)
						to[c] = (q[0][c]*q[0][3] + q[1][c]*q[1][3] + q[2][c]*q[2][3] + q[3][c]*q[3][3] + a/2)/a;
				to[3] = (a + 2)/4;
			}
		d3d.context->UpdateSubresource(texture, level, nil, below, nw*4, 0);
		if(owned)
			rwFree(owned);
		owned = above = below;
		w = nw;
		h = nh;
	}
	if(owned)
		rwFree(owned);
}

void
rasterUnlock(Raster *raster, int32 level)
{
	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	PerfScope timed(&d3dPerf.textureNs);

	assert(raster->pixels);

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		if(raster->privateFlags & Raster::LOCKWRITE){
			ID3D11Texture2D *texture = (ID3D11Texture2D*)natras->d3dTexture;
			D3D11_TEXTURE2D_DESC td;
			memset(&td, 0, sizeof(td));
			if(texture)
				texture->GetDesc(&td);
			if(natras->isCompressed){
				uint32 size = getLevelSize(raster, level);
				if(texture && (uint32)level < td.MipLevels){
					int32 blockBytes = natras->internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
						natras->internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? 16 : 8;
					if(natras->type == KEPT_BLOCKS){
						d3d.context->UpdateSubresource(texture, level, nil, raster->pixels,
							((raster->width + 3)/4)*blockBytes, 0);
						if(level == 0 && natras->autogenMipmap && td.MipLevels > 1)
							makeBlockLevelsBelow(raster, texture, td.MipLevels);
					}else{
						// decoded: the decoders write whole blocks
						int32 w = (raster->width + 3) & ~3, h = (raster->height + 3) & ~3;
						uint8 *rgba = (uint8*)rwMalloc(w*h*4, MEMDUR_FUNCTION | ID_DRIVER);
						if(blockBytes == 8)
							decompressDXT1(rgba, w, h, raster->pixels);
						else if(natras->internalFormat == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT)
							decompressDXT3(rgba, w, h, raster->pixels);
						else
							decompressDXT5(rgba, w, h, raster->pixels);
						D3D11_BOX box = { 0, 0, 0, (UINT)raster->width, (UINT)raster->height, 1 };
						d3d.context->UpdateSubresource(texture, level, &box, rgba, w*4, 0);
						rwFree(rgba);
					}
				}
				if(natras->backingStore){
					assert(level < natras->backingStore->numlevels);
					memcpy(natras->backingStore->levels[level].data, raster->pixels,
						natras->backingStore->levels[level].size);
				}
				(void)size;
			}else if(texture && natras->type == KEPT_RGBA && (uint32)level < td.MipLevels){
				d3d.context->UpdateSubresource(texture, level, nil, raster->pixels, raster->stride, 0);
				if(level == 0 && natras->autogenMipmap)
					makeLevelsBelow(raster, texture);
			}
		}
		break;

	case Raster::CAMERA:
		// TODO: write?
		break;
	}

	rwFree(raster->pixels);
	raster->pixels = nil;
	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
	raster->privateFlags = 0;
}

int32
rasterNumLevels(Raster *raster)
{
	return GETGL3RASTEREXT(raster)->numLevels;
}

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	Gl3Raster *ras = PLUGINOFFSET(Gl3Raster, object, offset);
	ras->texid = 0;
	ras->fbo = 0;
	ras->fboMate = nil;
	ras->backingStore = nil;
	ras->atlas = nil;
	ras->atlasPixels = nil;
	ras->d3dTexture = nil;
	ras->d3dResource = nil;
	ras->d3dTarget = nil;
	ras->d3dDepth = nil;
	ras->d3dClamp = false;
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	Raster *raster = (Raster*)object;
	Gl3Raster *natras = PLUGINOFFSET(Gl3Raster, object, offset);
	evictRaster(raster);
	if(natras->d3dTarget || natras->d3dDepth){
		// not to be drawn into from here on
		invalidateStates();
		if(d3d.context){
			ID3D11RenderTargetView *nothing = nil;
			d3d.context->OMSetRenderTargets(1, &nothing, nil);
		}
	}
	ID3D11ShaderResourceView *resource = (ID3D11ShaderResourceView*)natras->d3dResource;
	ID3D11RenderTargetView *target = (ID3D11RenderTargetView*)natras->d3dTarget;
	ID3D11DepthStencilView *depth = (ID3D11DepthStencilView*)natras->d3dDepth;
	ID3D11Texture2D *texture = (ID3D11Texture2D*)natras->d3dTexture;
	release(resource);
	release(target);
	release(depth);
	release(texture);
	natras->d3dResource = natras->d3dTarget = natras->d3dDepth = natras->d3dTexture = nil;
	natras->texid = 0;
	natras->fbo = 0;
	natras->fboMate = nil;

	free(natras->backingStore);
	natras->backingStore = nil;
	if(natras->atlasPixels){
		rwFree(natras->atlasPixels);
		natras->atlasPixels = nil;
	}
	if(natras->atlas){
		// the atlas lives as long as one of its parts
		Texture *atlas = natras->atlas;
		natras->atlas = nil;
		atlas->destroy();
	}
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	Gl3Raster *d = PLUGINOFFSET(Gl3Raster, dst, offset);
	d->texid = 0;
	d->fbo = 0;
	d->fboMate = nil;
	d->backingStore = nil;
	d->atlas = nil;
	d->atlasPixels = nil;
	d->d3dTexture = nil;
	d->d3dResource = nil;
	d->d3dTarget = nil;
	d->d3dDepth = nil;
	d->d3dClamp = false;
	return dst;
}

void
openRasters(void)
{
	// what is sampled where there is no texture
	static const uint8 white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof(td));
	td.Width = td.Height = 1;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data = { white, 4, 0 };
	if(FAILED(d3d.device->CreateTexture2D(&td, &data, &whiteTextureItself)) ||
	   FAILED(d3d.device->CreateShaderResourceView(whiteTextureItself, nil, &whiteTexture)))
		logf("d3d: no white texture");
}

void
closeRasters(void)
{
	release(whiteTexture);
	release(whiteTextureItself);
}

// Almost the same as d3d9 and ps2 function
bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pWidth, int32 *pHeight, int32 *pDepth, int32 *pFormat)
{
	int32 width, height, depth, format;

	assert((type&0xF) == Raster::TEXTURE);

//	for(width = 1; width < img->width; width <<= 1);
//	for(height = 1; height < img->height; height <<= 1);
	// Perhaps non-power-of-2 textures are acceptable?
	width = img->width;
	height = img->height;

	depth = img->depth;

	if(depth <= 8)
		depth = 32;

	switch(depth){
	case 32:
		if(img->hasAlpha())
			format = Raster::C8888;
		else{
			format = Raster::C888;
			depth = 24;
		}
		break;
	case 24:
		format = Raster::C888;
		break;
	case 16:
		format = Raster::C1555;
		break;

	case 8:
	case 4:
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	format |= type;

	*pWidth = width;
	*pHeight = height;
	*pDepth = depth;
	*pFormat = format;

	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if((raster->type&0xF) != Raster::TEXTURE)
		return 0;

	void (*conv)(uint8 *out, uint8 *in) = nil;

	// Unpalettize image if necessary but don't change original
	Image *truecolimg = nil;
	if(image->depth <= 8){
		truecolimg = Image::create(image->width, image->height, image->depth);
		truecolimg->pixels = image->pixels;
		truecolimg->stride = image->stride;
		truecolimg->palette = image->palette;
		truecolimg->unpalettize();
		image = truecolimg;
	}

	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	int32 format = raster->format&0xF00;
	assert(!natras->isCompressed);
	switch(image->depth){
	case 32:
		if(gl3Caps.gles)
			conv = conv_RGBA8888_from_RGBA8888;
		else if(format == Raster::C8888)
			conv = conv_RGBA8888_from_RGBA8888;
		else if(format == Raster::C888)
			conv = conv_RGB888_from_RGB888;
		else
			goto err;
		break;
	case 24:
		if(gl3Caps.gles)
			conv = conv_RGBA8888_from_RGB888;
		else if(format == Raster::C8888)
			conv = conv_RGBA8888_from_RGB888;
		else if(format == Raster::C888)
			conv = conv_RGB888_from_RGB888;
		else
			goto err;
		break;
	case 16:
		if(gl3Caps.gles)
			conv = conv_RGBA8888_from_ARGB1555;
		else if(format == Raster::C1555)
			conv = conv_RGBA5551_from_ARGB1555;
		else
			goto err;
		break;

	case 8:
	case 4:
	default:
	err:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	natras->hasAlpha = image->hasAlpha();

	bool unlock = false;
	if(raster->pixels == nil){
		raster->lock(0, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		unlock = true;
	}

	uint8 *pixels = raster->pixels;
	assert(pixels);
	uint8 *imgpixels = image->pixels + (image->height-1)*image->stride;

	int x, y;
	assert(image->width == raster->width);
	assert(image->height == raster->height);
	for(y = 0; y < image->height; y++){
		uint8 *imgrow = imgpixels;
		uint8 *rasrow = pixels;
		for(x = 0; x < image->width; x++){
			conv(rasrow, imgrow);
			imgrow += image->bpp;
			rasrow += natras->bpp;
		}
		imgpixels -= image->stride;
		pixels += raster->stride;
	}
	if(unlock)
		raster->unlock(0);

	if(truecolimg)
		truecolimg->destroy();

	return 1;
}

Image*
rasterToImage(Raster *raster)
{
	int32 depth;
	Image *image;

	bool unlock = false;
	if(raster->pixels == nil){
		raster->lock(0, Raster::LOCKREAD);
		unlock = true;
	}

	Gl3Raster *natras = GETGL3RASTEREXT(raster);
	if(natras->isCompressed){
		// TODO
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	void (*conv)(uint8 *out, uint8 *in) = nil;
	switch(raster->format & 0xF00){
	case Raster::C1555:
		depth = 16;
		conv = conv_ARGB1555_from_RGBA5551;
		break;
	case Raster::C8888:
		depth = 32;
		conv = conv_RGBA8888_from_RGBA8888;
		break;
	case Raster::C888:
		depth = 24;
		conv = conv_RGB888_from_RGB888;
		break;

	default:
	case Raster::C555:
	case Raster::C565:
	case Raster::C4444:
	case Raster::LUM8:
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	if(raster->format & Raster::PAL4 ||
	   raster->format & Raster::PAL8){
		RWERROR((ERR_INVRASTER));
		return nil;
	}
		
	uint8 *in, *out;
	image = Image::create(raster->width, raster->height, depth);
	image->allocate();

	uint8 *imgpixels = image->pixels + (image->height-1)*image->stride;
	uint8 *pixels = raster->pixels;

	int x, y;
	assert(image->width == raster->width);
	assert(image->height == raster->height);
	for(y = 0; y < image->height; y++){
		uint8 *imgrow = imgpixels;
		uint8 *rasrow = pixels;
		for(x = 0; x < image->width; x++){
			conv(imgrow, rasrow);
			imgrow += image->bpp;
			rasrow += natras->bpp;
		}
		imgpixels -= image->stride;
		pixels += raster->stride;
	}

	if(unlock)
		raster->unlock(0);

	return image;
}

// ===[ LIBRW_HALF_TEXTURES=1: what the textures' size is worth ]===
// A texture of 64 pixels and more each way is made half as wide and half as high as its file
// has it: the level below the first where there is one, or the first decoded, averaged two by
// two and encoded again. Not for looks: to measure what the size of textures costs in frames
// and in the time a new part of the city takes to come in.

static bool32 halfTextures = -1;

static void
encodeColorBlock(uint8 *dst, uint8 (*px)[4], bool32 punchThrough)
{
	int32 lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
	bool32 holes = 0;
	int32 i, c;
	for(i = 0; i < 16; i++){
		if(punchThrough && px[i][3] < 128){
			holes = 1;
			continue;
		}
		for(c = 0; c < 3; c++){
			if(px[i][c] < lo[c]) lo[c] = px[i][c];
			if(px[i][c] > hi[c]) hi[c] = px[i][c];
		}
	}
	if(lo[0] > hi[0])
		lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
	uint32 c0 = (hi[0]>>3)<<11 | (hi[1]>>2)<<5 | hi[2]>>3;
	uint32 c1 = (lo[0]>>3)<<11 | (lo[1]>>2)<<5 | lo[2]>>3;
	// four colours where the first is the greater, three and a hole where it is not
	if(holes ? c0 > c1 : c0 < c1){
		uint32 t = c0; c0 = c1; c1 = t;
	}
	int32 pal[4][3];
	pal[0][0] = (c0>>11)*255/31; pal[0][1] = ((c0>>5)&63)*255/63; pal[0][2] = (c0&31)*255/31;
	pal[1][0] = (c1>>11)*255/31; pal[1][1] = ((c1>>5)&63)*255/63; pal[1][2] = (c1&31)*255/31;
	int32 n = 4;
	if(holes || c0 == c1){
		n = c0 == c1 ? 1 : 3;
		for(c = 0; c < 3; c++)
			pal[2][c] = (pal[0][c] + pal[1][c])/2;
	}else
		for(c = 0; c < 3; c++){
			pal[2][c] = (2*pal[0][c] + pal[1][c])/3;
			pal[3][c] = (pal[0][c] + 2*pal[1][c])/3;
		}
	uint32 bits = 0;
	for(i = 0; i < 16; i++){
		int32 best = 0, bestd = 0x7FFFFFFF;
		if(punchThrough && px[i][3] < 128)
			best = 3;
		else
			for(int32 k = 0; k < n; k++){
				int32 d = 0;
				for(c = 0; c < 3; c++)
					d += (px[i][c] - pal[k][c])*(px[i][c] - pal[k][c]);
				if(d < bestd){
					bestd = d;
					best = k;
				}
			}
		bits |= (uint32)best << (2*i);
	}
	dst[0] = c0; dst[1] = c0>>8;
	dst[2] = c1; dst[3] = c1>>8;
	dst[4] = bits; dst[5] = bits>>8; dst[6] = bits>>16; dst[7] = bits>>24;
}

// RGBA to DXT1, 3 or 5; w and h are multiples of four
void
encodeDXT(uint8 *dst, uint8 *rgba, int32 w, int32 h, int32 dxt, bool32 hasAlpha)
{
	uint8 px[16][4];
	for(int32 y = 0; y < h; y += 4)
		for(int32 x = 0; x < w; x += 4){
			for(int32 i = 0; i < 16; i++)
				memcpy(px[i], &rgba[((y + i/4)*w + x + i%4)*4], 4);
			if(dxt == 3){
				for(int32 i = 0; i < 8; i++)
					dst[i] = px[i*2][3]>>4 | (px[i*2+1][3]>>4)<<4;
				dst += 8;
			}else if(dxt == 5){
				int32 a0 = 0, a1 = 255;
				for(int32 i = 0; i < 16; i++){
					if(px[i][3] > a0) a0 = px[i][3];
					if(px[i][3] < a1) a1 = px[i][3];
				}
				dst[0] = a0;
				dst[1] = a1;
				// eight steps from the greater to the lesser: 0, 2..7, 1
				uint64 bits = 0;
				for(int32 i = 0; i < 16 && a0 > a1; i++){
					int32 step = ((a0 - px[i][3])*7 + (a0 - a1)/2)/(a0 - a1);
					uint64 index = step == 0 ? 0 : step == 7 ? 1 : step + 1;
					bits |= index << (3*i);
				}
				for(int32 i = 0; i < 6; i++)
					dst[2+i] = bits >> (8*i);
				dst += 8;
			}
			encodeColorBlock(dst, px, dxt == 1 && hasAlpha);
			dst += 8;
		}
}

// what is read of a level that is to be made smaller; nil where the texture stays as it is
static uint8*
halfOfLevel(uint8 *src, uint32 size, int32 width, int32 height, int32 compression, bool32 hasAlpha, uint32 *halfSize)
{
	int32 w = width/2, h = height/2;
	uint8 *dst;
	if(compression){
		uint8 *rgba = rwNewT(uint8, width*height*4, MEMDUR_FUNCTION | ID_DRIVER);
		switch(compression){
		case 1: decompressDXT1(rgba, width, height, src); break;
		case 3: decompressDXT3(rgba, width, height, src); break;
		default: decompressDXT5(rgba, width, height, src); break;
		}
		for(int32 y = 0; y < h; y++)
			for(int32 x = 0; x < w; x++)
				for(int32 c = 0; c < 4; c++){
					uint8 *q = &rgba[(y*2*width + x*2)*4 + c];
					rgba[(y*w + x)*4 + c] = (q[0] + q[4] + q[width*4] + q[width*4 + 4] + 2)/4;
				}
		*halfSize = compression == 1 ? w*h/2 : w*h;
		dst = rwNewT(uint8, *halfSize, MEMDUR_FUNCTION | ID_DRIVER);
		encodeDXT(dst, rgba, w, h, compression, hasAlpha);
		rwFree(rgba);
		return dst;
	}
	// not compressed: whole bytes to a pixel, rows one after another
	int32 bpp = size/(width*height);
	if(bpp < 1 || bpp > 4 || (uint32)(bpp*width*height) != size)
		return nil;
	*halfSize = w*h*bpp;
	dst = rwNewT(uint8, *halfSize, MEMDUR_FUNCTION | ID_DRIVER);
	for(int32 y = 0; y < h; y++)
		for(int32 x = 0; x < w; x++){
			uint8 *q = &src[(y*2*width + x*2)*bpp];
			if(bpp >= 3)
				for(int32 c = 0; c < bpp; c++)
					dst[(y*w + x)*bpp + c] = (q[c] + q[bpp + c] + q[width*bpp + c] + q[width*bpp + bpp + c] + 2)/4;
			else
				memcpy(&dst[(y*w + x)*bpp], q, bpp);	// packed colours: every other pixel
		}
	return dst;
}

// ===[ Atlases (gl3Caps.atlases) ]===
// An atomic of the game is a few meshes that differ in nothing but their texture, and each is
// a draw of its own with a change of texture before it: two thirds of the draws of a frame.
// The textures of a dictionary are therefore also put together into bigger ones, each with
// its edge blocks repeated around it; a vertex of geometry instanced for it says where its
// texture is in the atlas (in_atlas), the fragment shader wraps the coordinates inside that
// part, and defaultRenderCB draws the meshes that follow one another as one.
// DXT textures only, of one format in one atlas, whole blocks copied as they are; textures
// that are wrapped both ways (the shader does the wrapping) and no bigger than 512.

bool32 collectForAtlases;

struct AtlasItem
{
	Raster *raster;
	Gl3Raster *natras;
	int32 x, y;	// where it is, its edge included, in pixels
};

// An atlas of the first of the n textures, as many as it takes; how many that was. They are
// in the order of their heights, tallest first, and laid out in rows.
static int32
makeAtlas(AtlasItem *items, int32 n, int32 dxt, bool32 hasAlpha)
{
	enum { EDGE = 4, MAXSIZE = 2048 };
	int32 i, area = 0, widest = 0;
	for(i = 0; i < n; i++){
		int32 w = items[i].raster->width + 2*EDGE;
		area += w*(items[i].raster->height + 2*EDGE);
		if(w > widest)
			widest = w;
	}
	int32 width = 256;
	while(width < MAXSIZE && (width*width < area || width < widest))
		width *= 2;

	int32 x = 0, y = 0, row = 0, placed = 0;
	for(i = 0; i < n; i++){
		int32 w = items[i].raster->width + 2*EDGE, h = items[i].raster->height + 2*EDGE;
		if(x + w > width){
			x = 0;
			y += row;
			row = 0;
		}
		if(y + h > MAXSIZE)
			break;
		items[i].x = x;
		items[i].y = y;
		x += w;
		if(h > row)
			row = h;
		placed++;
	}
	if(placed < 2)
		return 1;	// one texture alone shares nothing
	int32 height = 4;
	while(height < y + row)
		height *= 2;

	Raster *first = items[0].raster;
	Raster *raster = Raster::create(width, height, first->depth,
		(first->format & 0x0F00) | Raster::TEXTURE | Raster::DONTALLOCATE, PLATFORM_GL3);
	// with its one level: smaller copies of it would run its textures into one another
	noLevelsBelow = 1;
	allocateDXT(raster, dxt, 1, hasAlpha);
	noLevelsBelow = 0;
	int32 blockBytes = dxt == 1 ? 8 : 16;
	uint8 *px = raster->lock(0, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
	memset(px, 0, (width/4)*(height/4)*blockBytes);
	for(i = 0; i < placed; i++){
		// with a block of its far side before its near side each way: what a texture
		// that repeats has there, for the filter to take
		int32 sw = items[i].raster->width/4, sh = items[i].raster->height/4;
		uint8 *src = items[i].natras->atlasPixels;
		for(int32 by = -1; by <= sh; by++)
			for(int32 bx = -1; bx <= sw; bx++)
				memcpy(px + ((items[i].y/4 + 1 + by)*(width/4) + items[i].x/4 + 1 + bx)*blockBytes,
				       src + (((by + sh) % sh)*sw + (bx + sw) % sw)*blockBytes, blockBytes);
	}
	raster->unlock(0);

	Texture *atlas = Texture::create(raster);
	strcpy(atlas->name, "atlas");
	atlas->filterAddressing = Texture::WRAP<<12 | Texture::WRAP<<8 | Texture::LINEAR;
	for(i = 0; i < placed; i++){
		Gl3Raster *natras = items[i].natras;
		natras->atlas = atlas;
		atlas->addRef();
		natras->atlasRect[0] = (items[i].x + EDGE)/(float)width;
		natras->atlasRect[1] = (items[i].y + EDGE)/(float)height;
		natras->atlasRect[2] = items[i].raster->width/(float)width;
		natras->atlasRect[3] = items[i].raster->height/(float)height;
	}
	atlas->destroy();	// its parts keep it
	return placed;
}

void
makeAtlases(TexDictionary *dict)
{
	static const int32 formats[4] = {
		GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
		GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
	};
	static const int32 dxt[4] = { 1, 1, 3, 5 };
	enum { MAXITEMS = 256 };
	static AtlasItem items[MAXITEMS];

	if(dict == nil)
		return;
	for(int32 f = 0; f < 4 && gl3Caps.atlases; f++){
		int32 n = 0;
		FORLIST(lnk, dict->textures){
			Texture *tex = Texture::fromDict(lnk);
			Raster *r = tex->raster;
			if(r == nil || n == MAXITEMS)
				continue;
			Gl3Raster *natras = GETGL3RASTEREXT(r);
			if(natras->atlasPixels == nil || natras->atlas || natras->internalFormat != formats[f] ||
			   natras->hasAlpha != (f != 0))
				continue;
			if(tex->getAddressU() != Texture::WRAP || tex->getAddressV() != Texture::WRAP)
				continue;
			if(r->width < 8 || r->height < 8 || r->width > 512 || r->height > 512 ||
			   (r->width & 3) || (r->height & 3))
				continue;
			int32 at = n++;
			for(; at > 0 && items[at-1].raster->height < r->height; at--)
				items[at] = items[at-1];
			items[at].raster = r;
			items[at].natras = natras;
		}
		AtlasItem *rest = items;
		while(n >= 2){
			int32 taken = makeAtlas(rest, n, dxt[f], f != 0);
			rest += taken;
			n -= taken;
		}
	}
	FORLIST(lnk, dict->textures){
		Texture *tex = Texture::fromDict(lnk);
		if(tex->raster == nil)
			continue;
		Gl3Raster *natras = GETGL3RASTEREXT(tex->raster);
		if(natras->atlasPixels){
			rwFree(natras->atlasPixels);
			natras->atlasPixels = nil;
		}
	}
}

Texture*
readNativeTexture(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_GL3){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	// Texture
	tex->filterAddressing = stream->readU32();
	stream->read8(tex->name, 32);
	stream->read8(tex->mask, 32);

	// Raster
	uint32 format = stream->readU32();
	int32 width = stream->readI32();
	int32 height = stream->readI32();
	int32 depth = stream->readI32();
	int32 numLevels = stream->readI32();

	// Native raster
	int32 subplatform = stream->readI32();
	int32 flags = stream->readI32();
	int32 compression = stream->readI32();

	if(subplatform != gl3Caps.gles){
		tex->destroy();
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}

	if(halfTextures < 0)
		halfTextures = getenv("LIBRW_HALF_TEXTURES") != nil;
	uint8 *half = nil;
	uint32 halfSize = 0;
	if(halfTextures && width >= 64 && height >= 64 && (width & 7) == 0 && (height & 7) == 0){
		// the first level is read here: left out where there are more, made smaller otherwise
		uint32 size = stream->readU32();
		uint8 *first = rwNewT(uint8, size, MEMDUR_FUNCTION | ID_DRIVER);
		stream->read8(first, size);
		if(numLevels > 1){
			numLevels--;
			width /= 2;
			height /= 2;
			rwFree(first);
		}else{
			half = halfOfLevel(first, size, width, height, flags & 2 ? compression : 0, flags & 1, &halfSize);
			if(half){
				width /= 2;
				height /= 2;
				rwFree(first);
			}else{
				half = first;	// as it is
				halfSize = size;
			}
		}
	}

	Raster *raster;
	Gl3Raster *natras;
	if(flags & 2){
		if(!gl3Caps.dxtSupported){
			tex->destroy();
			RWERROR((ERR_FORMAT_UNSUPPORTED));
			return nil;
		}
		raster = Raster::create(width, height, depth, format | Raster::TEXTURE | Raster::DONTALLOCATE, PLATFORM_GL3);
		allocateDXT(raster, compression, numLevels, flags & 1);
	}else{
		// one level in its file: the others are made from it here, as for a compressed one
		if(numLevels == 1 && !(format & Raster::MIPMAP) && wantsLevelsBelow(width, height, 1))
			format |= Raster::MIPMAP | Raster::AUTOMIPMAP;
		raster = Raster::create(width, height, depth, format | Raster::TEXTURE, PLATFORM_GL3);
	}
	assert(raster);
	natras = GETGL3RASTEREXT(raster);
	tex->raster = raster;

	uint32 size;
	uint8 *data;
	for(int32 i = 0; i < numLevels; i++){
		data = raster->lock(i, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		if(i == 0 && half){
			size = halfSize;
			memcpy(data, half, halfSize);
			rwFree(half);
		}else{
			size = stream->readU32();
			stream->read8(data, size);
		}
		if(i == 0 && collectForAtlases && gl3Caps.atlases && natras->isCompressed){
			natras->atlasPixels = rwNewT(uint8, size, MEMDUR_EVENT | ID_DRIVER);
			memcpy(natras->atlasPixels, data, size);
		}
		raster->unlock(i);
	}
	return tex;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	Raster *raster = tex->raster;
	Gl3Raster *natras = GETGL3RASTEREXT(raster);

	int32 chunksize = getSizeNativeTexture(tex);
	writeChunkHeader(stream, ID_STRUCT, chunksize-12);
	stream->writeU32(PLATFORM_GL3);

	// Texture
	stream->writeU32(tex->filterAddressing);
	stream->write8(tex->name, 32);
	stream->write8(tex->mask, 32);

	// Raster
	int32 numLevels = natras->numLevels;
	stream->writeI32(raster->format);
	stream->writeI32(raster->width);
	stream->writeI32(raster->height);
	stream->writeI32(raster->depth);
	stream->writeI32(numLevels);

	// Native raster
	int32 flags = 0;
	int32 compression = 0;
	if(natras->hasAlpha)
		flags |= 1;
	if(natras->isCompressed){
		flags |= 2;
		switch(natras->internalFormat){
#ifdef RW_OPENGL
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
			compression = 1;
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
			compression = 3;
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			compression = 5;
			break;
#endif
		default:
			assert(0 && "unknown compression");
		}
	}
	stream->writeI32(gl3Caps.gles);
	stream->writeI32(flags);
	stream->writeI32(compression);
	// TODO: auto mipmaps?

	uint32 size;
	uint8 *data;
	for(int32 i = 0; i < numLevels; i++){
		size = getLevelSize(raster, i);
		stream->writeU32(size);
		data = raster->lock(i, Raster::LOCKREAD);
		stream->write8(data, size);
		raster->unlock(i);
	}
}

uint32
getSizeNativeTexture(Texture *tex)
{
	uint32 size = 12 + 72 + 32;
	int32 levels = tex->raster->getNumLevels();
	for(int32 i = 0; i < levels; i++)
		size += 4 + getLevelSize(tex->raster, i);
	return size;
}



void registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(Gl3Raster),
	                                            ID_RASTERGL3,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

}
}
