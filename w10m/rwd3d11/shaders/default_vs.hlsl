// librw's default.vert, the game's default_UV2.vert (UV2: a second set of texture coordinates,
// for the world's light maps) and neoRim.vert (RIM: a rim light on what faces away).
// ATLAS: the vertex says where its texture is in an atlas (the renderer's own).
// DIRECTIONALS, POINTLIGHTS, SPOTLIGHTS: the lights that are counted; without any, and without
// RIM, the vertex has no normal to give.
// INSTANCE: what differs from one draw to the next (where the object is, the light it stands
// in, its material) comes with the vertices, from a second buffer that has it once for the
// draw, and not as constants. On a GPU of Direct3D's level 9 a draw whose vertex shader's
// constants are not the draw's before costs three times one whose are (a Lumia 650: 87
// microseconds against 32 this way, bench/d3dbench.cpp), and that is most draws of a street.
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
#ifdef INSTANCE
	float4 world0 : TEXCOORD2;	// u_world, its rows (the fourth is 0 0 0 1)
	float4 world1 : TEXCOORD3;
	float4 world2 : TEXCOORD4;
	float4 ambient : TEXCOORD5;	// u_ambLight.rgb*surfAmbient, and surfDiffuse
	float4 matColor : TEXCOORD6;	// u_matColor
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
#ifdef INSTANCE
	float4 p = float4(i.pos, 1.0);
	float4 Vertex = float4(dot(i.world0, p), dot(i.world1, p), dot(i.world2, p), 1.0);
#else
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
#endif
	o.pos = mul(u_proj, mul(u_view, Vertex));

	o.v.tex0.xy = Flip(i.tex0);
#ifdef UV2
	o.v.tex1 = Flip(i.tex1);
#endif
#ifdef ATLAS
	o.v.atlas = i.atlas;
#endif

	float4 color = i.color;
#ifdef INSTANCE
	color.rgb += i.ambient.rgb;
#else
	color.rgb += u_ambLight.rgb*surfAmbient;
#endif
#if defined(LIT) || defined(RIM)
#ifdef INSTANCE
	float3 Normal = float3(dot(i.world0.xyz, i.normal), dot(i.world1.xyz, i.normal), dot(i.world2.xyz, i.normal));
#else
	float3 Normal = mul((float3x3)u_world, i.normal);
#endif
#endif
#ifdef LIT
#ifdef INSTANCE
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*i.ambient.a;
#else
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
#endif
#endif
#ifdef RIM
	float f = u_rimData.x - u_rimData.y*dot(Normal, u_viewVec);
	float4 rimlight = clamp(lerp(u_rampEnd, u_rampStart, f)*u_rimData.z, 0.0, 1.0);
	color.rgb += rimlight.rgb;
#endif
	color = clamp(color, 0.0, 1.0);
#ifdef INSTANCE
	o.v.color = color*i.matColor;
#else
	o.v.color = color*u_matColor;
#endif

	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
