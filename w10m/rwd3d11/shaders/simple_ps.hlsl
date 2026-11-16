// librw's simple.frag (and the game's, which is the same). NO_ALPHATEST: nothing is discarded.
#include "common.hlsli"

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(Simple v) : SV_Target
{
	float4 color = v.color*tex0.Sample(smp0, v.tex0.xy);
	color.rgb = lerp(u_fogColor.rgb, color.rgb, v.tex0.z);
	DoAlphaTest(color.a);
	return color;
}
