// librw's im3d.vert
#include "common.hlsli"

struct VSIn
{
	float3 pos : POSITION;
	float4 color : COLOR;
	float2 tex0 : TEXCOORD0;
};

struct VSOut
{
	Simple v;
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
	o.pos = mul(u_proj, mul(u_view, Vertex));
	o.v.color = i.color;
	o.v.tex0.xy = Flip(i.tex0);
	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
