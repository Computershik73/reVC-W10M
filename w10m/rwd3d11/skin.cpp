// The skin pipeline: as vendor/librw/src/gl/gl3skin.cpp, with the vertices as pipe.cpp makes
// them for Direct3D (a normal and a colour for every vertex, whether the geometry has them or
// not) and the bones always as three rows of each matrix (shaders/skin_vs.hlsl).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "rwbase.h"
#include "rwerror.h"
#include "rwplg.h"
#include "rwrender.h"
#include "rwpipeline.h"
#include "rwobjects.h"
#include "rwanim.h"
#include "rwengine.h"
#include "rwplugins.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3plg.h"

#include "rwgl3impl.h"

#include "d3d11impl.h"

namespace rw {
namespace gl3 {

static Shader *skinShader, *skinShader_noAT;
static Shader *skinShader_fullLight, *skinShader_fullLight_noAT;
static int32 u_boneRows;

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	instanceVertices(geo, header, reinstance, INSTANCE_SKIN);
}

void
skinUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	assert(0 && "can't uninstance");
}

static float skinMatrices[64*16];
static float skinRows[64*12];

void
uploadSkinMatrices(Atomic *a)
{
	int i;
	Skin *skin = Skin::get(a->geometry);
	Matrix *m = (Matrix*)skinMatrices;
	HAnimHierarchy *hier = Skin::getHierarchy(a);

	if(hier){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		Matrix tmp;

		assert(skin->numBones == hier->numNodes);
		if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(i = 0; i < hier->numNodes; i++){
				invMats[i].flags = 0;
				Matrix::mult(m, &invMats[i], &hier->matrices[i]);
				m++;
			}
		}else{
			Matrix invAtmMat;
			Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
			for(i = 0; i < hier->numNodes; i++){
				invMats[i].flags = 0;
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(m, &invMats[i], &tmp);
				m++;
			}
		}
	}else{
		for(i = 0; i < skin->numBones; i++){
			m->setIdentity();
			m++;
		}
	}
	packBoneRows(skinRows, skinMatrices, hier ? hier->numNodes : skin->numBones);
	setUniform(u_boneRows, skinRows);
}

// A Matrix is right, up, at and pos, four floats each: as a shader's matrix they are its
// columns, and its fourth row is not used. The three rows that are, for u_boneRows:
void
packBoneRows(float *rows, const float *matrices, int32 numBones)
{
	for(int32 i = 0; i < numBones; i++){
		const float *f = &matrices[i*16];
		float *r = &rows[i*12];
		for(int j = 0; j < 3; j++){
			r[j*4+0] = f[0+j];
			r[j*4+1] = f[4+j];
			r[j*4+2] = f[8+j];
			r[j*4+3] = f[12+j];
		}
	}
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	perf.pipe = 3;
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	uploadSkinMatrices(atomic);

	while(n--){
		m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		if(fewerPrograms)
			skinShader_fullLight->use();
		else if((vsBits & VSLIGHT_MASK) == 0){
			if(getAlphaTest())
				skinShader->use();
			else
				skinShader_noAT->use();
		}else{
			if(getAlphaTest())
				skinShader_fullLight->use();
			else
				skinShader_fullLight_noAT->use();
		}

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}

static void*
skinOpen(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_GL3] = makeSkinPipeline();

	skinShader = Shader::create("skin", "simple");
	assert(skinShader);
	skinShader_noAT = Shader::create("skin", "simple_noAT");
	assert(skinShader_noAT);
	skinShader_fullLight = Shader::create("skin_fullLight", "simple");
	assert(skinShader_fullLight);
	skinShader_fullLight_noAT = Shader::create("skin_fullLight", "simple_noAT");
	assert(skinShader_fullLight_noAT);

	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	((ObjPipeline*)skinGlobals.pipelines[PLATFORM_GL3])->destroy();
	skinGlobals.pipelines[PLATFORM_GL3] = nil;

	skinShader->destroy();
	skinShader = nil;
	skinShader_noAT->destroy();
	skinShader_noAT = nil;
	skinShader_fullLight->destroy();
	skinShader_fullLight = nil;
	skinShader_fullLight_noAT->destroy();
	skinShader_fullLight_noAT = nil;

	return o;
}

void
initSkin(void)
{
	u_boneRows = registerUniform("u_boneRows", UNIFORM_VEC4, 64*3);

	Driver::registerPlugin(PLATFORM_GL3, 0, ID_SKIN,
	                       skinOpen, skinClose);
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = skinUninstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

}
}
