// The game's screenDroplet.frag: the drops of rain on the screen
#include "common.hlsli"

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
Texture2D tex1 : register(t1);
SamplerState smp1 : register(s1);

float4 main(TwoTextures v) : SV_Target
{
	float4 color = v.color*tex0.Sample(smp0, v.tex0.xy);
	return color*tex1.Sample(smp1, v.tex1);
}
