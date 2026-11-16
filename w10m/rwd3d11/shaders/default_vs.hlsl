// librw's default.vert, the game's default_UV2.vert (UV2: a second set of texture coordinates,
// for the world's light maps) and neoRim.vert (RIM: a rim light on what faces away).
// ATLAS: the vertex says where its texture is in an atlas (the renderer's own).
// DIRECTIONALS, POINTLIGHTS, SPOTLIGHTS: the lights that are counted; without any, and without
// RIM, the vertex has no normal to give.
#include "common.hlsli"

#if defined(DIRECTIONALS) || defined(POINTLIGHTS) || defined(SPOTLIGHTS)
#define LIT
#endif

#ifdef RIM
cbuffer Rim
{
	float3 u_viewVec;
	float4 u_rampStart;
	float4 u_rampEnd;
	float3 u_rimData;
};
#endif

struct VSIn
{
	float3 pos : POSITION;
#if defined(LIT) || defined(RIM)
	float3 normal : NORMAL;
#endif
	float4 color : COLOR;
	float2 tex0 : TEXCOORD0;
#ifdef UV2
	float2 tex1 : TEXCOORD1;
#endif
#ifdef ATLAS
	float4 atlas : TEXCOORD7;
#endif
};

struct VSOut
{
#if defined(ATLAS)
	Atlas v;
#elif defined(UV2)
	TwoTextures v;
#else
	Simple v;
#endif
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
	o.pos = mul(u_proj, mul(u_view, Vertex));

	o.v.tex0.xy = Flip(i.tex0);
#ifdef UV2
	o.v.tex1 = Flip(i.tex1);
#endif
#ifdef ATLAS
	o.v.atlas = i.atlas;
#endif

	float4 color = i.color;
	color.rgb += u_ambLight.rgb*surfAmbient;
#if defined(LIT) || defined(RIM)
	float3 Normal = mul((float3x3)u_world, i.normal);
#endif
#ifdef LIT
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
#endif
#ifdef RIM
	float f = u_rimData.x - u_rimData.y*dot(Normal, u_viewVec);
	float4 rimlight = clamp(lerp(u_rampEnd, u_rampStart, f)*u_rimData.z, 0.0, 1.0);
	color.rgb += rimlight.rgb;
#endif
	color = clamp(color, 0.0, 1.0);
	o.v.color = color*u_matColor;

	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
