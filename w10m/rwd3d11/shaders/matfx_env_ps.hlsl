// librw's matfx_env.frag
#include "common.hlsli"

cbuffer Fx
{
	float4 u_fxparams;	// shininess, disableFBA
};

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
Texture2D tex1 : register(t1);
SamplerState smp1 : register(s1);

float4 main(TwoColours v) : SV_Target
{
	float4 pass1 = v.color*tex0.Sample(smp0, v.tex0.xy);
	float4 pass2 = v.color2*u_fxparams.x*tex1.Sample(smp1, v.tex1);

	pass1.rgb = lerp(u_fogColor.rgb, pass1.rgb, v.tex0.z);
	pass2.rgb = pass2.rgb*v.tex0.z;

	float fba = max(pass1.a, u_fxparams.y);
	float4 color;
	color.rgb = pass1.rgb*pass1.a + pass2.rgb*fba;
	color.a = pass1.a;

	DoAlphaTest(color.a);
	return color;
}
