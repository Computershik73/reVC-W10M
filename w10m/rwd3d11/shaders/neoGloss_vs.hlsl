// The game's neoGloss.vert: the shine of a wet road
#include "common.hlsli"

cbuffer Gloss
{
	float3 u_eye;
};

struct VSIn
{
	float3 pos : POSITION;
	float2 tex0 : TEXCOORD0;
};

struct VSOut
{
	Simple v;	// color: the way to the light, packed into 0..1
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
	o.pos = mul(u_proj, mul(u_view, Vertex));

	o.v.tex0.xy = Flip(i.tex0);

	float3 viewVec = normalize(u_eye - Vertex.xyz);
	float3 Light = normalize(viewVec - u_lightDirection[0].xyz);
	o.v.color = float4(0.5*(1.0 + Light), 1.0);

	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
