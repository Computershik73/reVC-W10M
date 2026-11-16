// simple.frag for a vertex that says where its texture is in an atlas: the coordinates wrap
// inside that part, as they would over the whole of a texture of its own. A vertex that says
// nothing (a size of nought) is drawn as ever.
#include "common.hlsli"

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(Atlas v) : SV_Target
{
	float2 uv = v.tex0.xy;
	if(v.atlas.z > 0.0)
		uv = v.atlas.xy + frac(uv)*v.atlas.zw;
	float4 color = v.color*tex0.Sample(smp0, uv);
	color.rgb = lerp(u_fogColor.rgb, color.rgb, v.tex0.z);
	DoAlphaTest(color.a);
	return color;
}
