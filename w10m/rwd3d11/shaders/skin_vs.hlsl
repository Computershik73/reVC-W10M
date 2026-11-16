// librw's skin.vert and the game's neoRimSkin.vert (RIM). The bones are three rows of each
// matrix: a vertex shader of Direct3D's level 9 has 256 constants, and 64 whole matrices are
// that many alone. A vertex's bone indices come as bytes scaled to 0..1.
#include "common.hlsli"

cbuffer Bones
{
	float4 u_boneRows[192];
};

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
	float3 normal : NORMAL;
	float4 color : COLOR;
	float2 tex0 : TEXCOORD0;
	float4 weights : BLENDWEIGHT;
	float4 indices : BLENDINDICES;
};

struct VSOut
{
	Simple v;
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
	float3 SkinVertex = float3(0.0, 0.0, 0.0);
	float3 SkinNormal = float3(0.0, 0.0, 0.0);
	float4 p = float4(i.pos, 1.0);
	for(int k = 0; k < 4; k++){
		int b = (int)(i.indices[k]*255.0 + 0.5)*3;
		SkinVertex += float3(dot(u_boneRows[b], p), dot(u_boneRows[b+1], p), dot(u_boneRows[b+2], p))*i.weights[k];
		SkinNormal += float3(dot(u_boneRows[b].xyz, i.normal), dot(u_boneRows[b+1].xyz, i.normal),
			dot(u_boneRows[b+2].xyz, i.normal))*i.weights[k];
	}

	float4 Vertex = mul(u_world, float4(SkinVertex, 1.0));
	o.pos = mul(u_proj, mul(u_view, Vertex));
	float3 Normal = mul((float3x3)u_world, SkinNormal);

	o.v.tex0.xy = Flip(i.tex0);

	float4 color = i.color;
	color.rgb += u_ambLight.rgb*surfAmbient;
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
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
