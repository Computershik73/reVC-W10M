// The game's neoWorldVC.frag: the world with its light maps
#include "common.hlsli"

cbuffer LightMap
{
	float4 u_lightMap;
};

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
Texture2D tex1 : register(t1);
SamplerState smp1 : register(s1);

float4 main(TwoTextures v) : SV_Target
{
	float4 t0 = tex0.Sample(smp0, v.tex0.xy);
	float4 t1 = tex1.Sample(smp1, v.tex1);

	float4 color = t0*v.color*(1.0 + u_lightMap*(t1 - 1.0));
	color.a = v.color.a*t0.a*u_lightMap.a;

	color.rgb = lerp(u_fogColor.rgb, color.rgb, v.tex0.z);
	DoAlphaTest(color.a);
	return color;
}
