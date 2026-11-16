// The game's neoGloss.frag. Its normal is the same for every vertex, straight up, so what it
// takes of the way to the light is how far up that goes.
#include "common.hlsli"

cbuffer Gloss
{
	float4 u_reflProps;	// .x: how strong
};

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(Simple v) : SV_Target
{
	float4 color = tex0.Sample(smp0, v.tex0.xy);
	float s = 2.0*v.color.z - 1.0;
	float s2 = s*s;
	float s4 = s2*s2;
	color = s4*s4*color*v.tex0.z*u_reflProps.x;

	DoAlphaTest(color.a);
	return color;
}
