// librw's simple.frag (and the game's, which is the same). NO_ALPHATEST: nothing is discarded.
// MASKED: the alpha test without throwing a pixel away. A pixel that fails it comes out as
// nothing at all (black, alpha nought), which is the same picture where it is added to what
// is there or blended over it by its alpha, and writes no depth: the sky drawn behind the
// world (immed.cpp). A shader that throws pixels away keeps a GPU of level 9 from giving a
// hidden pixel up before it has worked it out.
#include "common.hlsli"

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(Simple v) : SV_Target
{
	float4 color = v.color*tex0.Sample(smp0, v.tex0.xy);
	color.rgb = lerp(u_fogColor.rgb, color.rgb, v.tex0.z);
#ifdef MASKED
	return color*((color.a < u_alphaRef.x || color.a >= u_alphaRef.y) ? 0.0 : 1.0);
#else
	DoAlphaTest(color.a);
	return color;
#endif
}
