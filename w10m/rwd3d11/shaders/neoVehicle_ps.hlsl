// The game's neoVehicle.frag
#include "common.hlsli"

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
Texture2D tex1 : register(t1);
SamplerState smp1 : register(s1);

float4 main(TwoColours v) : SV_Target
{
	float4 pass1 = v.color*tex0.Sample(smp0, v.tex0.xy);
	float3 envmap = tex1.Sample(smp1, v.tex1).rgb;
	pass1.rgb = lerp(pass1.rgb, envmap, v.color2.a);
	pass1.rgb = lerp(u_fogColor.rgb, pass1.rgb, v.tex0.z);

	float3 pass2 = v.color2.rgb*v.tex0.z;

	float4 color;
	color.rgb = pass1.rgb*pass1.a + pass2;
	color.a = pass1.a;

	DoAlphaTest(color.a);
	return color;
}
