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
#ifdef INSTANCE
	// what a draw has of its own, with the vertices (default_vs.hlsl says why)
	float4 world0 : TEXCOORD2;
	float4 world1 : TEXCOORD3;
	float4 world2 : TEXCOORD4;
	float4 ambient : TEXCOORD5;	// u_ambLight.rgb*surfAmbient, and surfDiffuse
	float4 matColor : TEXCOORD6;
	float4 envColor : COLOR1;	// u_envColor
#endif
};

struct VSOut
{
	TwoColours v;
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
#ifdef INSTANCE
	float4 p = float4(i.pos, 1.0);
	float4 Vertex = float4(dot(i.world0, p), dot(i.world1, p), dot(i.world2, p), 1.0);
	float3 Normal = float3(dot(i.world0.xyz, i.normal), dot(i.world1.xyz, i.normal), dot(i.world2.xyz, i.normal));
#else
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
	float3 Normal = mul((float3x3)u_world, i.normal);
#endif
	o.pos = mul(u_proj, mul(u_view, Vertex));

	o.v.tex0.xy = Flip(i.tex0);
	o.v.tex1 = Flip(mul(u_texMatrix, float4(Normal, 1.0)).xy);

	float4 color = i.color;
#ifdef INSTANCE
	color.rgb += i.ambient.rgb;
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*i.ambient.a;
	color = clamp(color, 0.0, 1.0);
	o.v.color2 = max(color, u_colorClamp)*i.envColor;
	o.v.color = color*i.matColor;
#else
	color.rgb += u_ambLight.rgb*surfAmbient;
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
	color = clamp(color, 0.0, 1.0);
	o.v.color2 = max(color, u_colorClamp)*u_envColor;
	o.v.color = color*u_matColor;
#endif

	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
