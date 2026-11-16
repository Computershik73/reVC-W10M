// The game's colourfilterVC.frag: Vice City's colour filter over the picture
#include "common.hlsli"

cbuffer Filter
{
	float4 u_blurcolor;
};

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(Simple v) : SV_Target
{
	float a = u_blurcolor.a;
	float4 doublec = clamp(u_blurcolor*2.0, 0.0, 1.0);
	float4 dst = tex0.Sample(smp0, v.tex0.xy);
	float4 prev = dst;
	for(int i = 0; i < 5; i++){
		float4 tmp = dst*(1.0 - a) + prev*doublec*a;
		tmp += prev*u_blurcolor;
		tmp += prev*u_blurcolor;
		prev = clamp(tmp, 0.0, 1.0);
	}
	return float4(prev.rgb, 1.0);
}
