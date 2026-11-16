// librw's matfx_env.vert: an environment map over the texture
#include "common.hlsli"

cbuffer Env
{
	float4x4 u_texMatrix;
	float4 u_colorClamp;
	float4 u_envColor;
};

struct VSIn
{
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR;
	float2 tex0 : TEXCOORD0;
};

struct VSOut
{
	TwoColours v;
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
	o.pos = mul(u_proj, mul(u_view, Vertex));
	float3 Normal = mul((float3x3)u_world, i.normal);

	o.v.tex0.xy = Flip(i.tex0);
	o.v.tex1 = Flip(mul(u_texMatrix, float4(Normal, 1.0)).xy);

	float4 color = i.color;
	color.rgb += u_ambLight.rgb*surfAmbient;
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
	color = clamp(color, 0.0, 1.0);
	o.v.color2 = max(color, u_colorClamp)*u_envColor;
	o.v.color = color*u_matColor;

	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
