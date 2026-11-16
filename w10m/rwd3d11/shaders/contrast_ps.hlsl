// The game's contrast.frag
#include "common.hlsli"

cbuffer Contrast
{
	float3 u_contrastAdd;
	float3 u_contrastMult;
};

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(Simple v) : SV_Target
{
	float4 dst = tex0.Sample(smp0, v.tex0.xy);
	return float4(dst.rgb*u_contrastMult + u_contrastAdd, 1.0);
}
