// librw's im2d.vert; the game's own im2d.vert (GAME: w is 1, the fog is by z), which its
// colour filters are drawn with, and im2d_UV2.vert (UV2: a second set of texture coordinates,
// for the drops on the screen).
#include "common.hlsli"

cbuffer Xform
{
	float4 u_xform;
};

struct VSIn
{
	float4 pos : POSITION;
	float4 color : COLOR;
	float2 tex0 : TEXCOORD0;
#ifdef UV2
	float2 tex1 : TEXCOORD1;
#endif
};

struct VSOut
{
#ifdef UV2
	TwoTextures v;
#else
	Simple v;
#endif
	float4 pos : SV_Position;
};

VSOut main(VSIn i)
{
	VSOut o;
	float4 p = i.pos;
#ifdef GAME
	p.w = 1.0;
#endif
	p.xy = p.xy*u_xform.xy + u_xform.zw;
#ifdef GAME
	o.v.tex0.z = DoFog(p.z);
#else
	o.v.tex0.z = DoFog(p.w);
#endif
	p.xyz *= p.w;
	o.pos = p;
	o.v.color = i.color;
	o.v.tex0.xy = Flip(i.tex0);
#ifdef UV2
	o.v.tex1 = Flip(i.tex1);
#endif
	return o;
}
