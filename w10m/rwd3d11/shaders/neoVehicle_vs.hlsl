// The game's neoVehicle.vert: a vehicle's paint, with the sky reflected in it and the lights'
// highlights
#include "common.hlsli"

cbuffer Vehicle
{
	float3 u_eye;
	float4 u_reflProps;	// fresnel, the highlights' strength, shininess, specularity
	float4 u_specDir[5];
	float4 u_specColor[5];
};

struct VSIn
{
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR;
	float2 tex0 : TEXCOORD0;
};

struct VSOut
{
	TwoColours v;
	float4 pos : SV_Position;
};

float3 DoDirLightSpec(float3 Ldir, float3 Lcol, float3 N, float3 V, float power)
{
	// nought to the power of nought has no value here, and a light that is not there is both
	return pow(max(clamp(dot(N, normalize(V + -Ldir)), 0.0, 1.0), 0.00001), max(power, 0.00001))*Lcol;
}

VSOut main(VSIn i)
{
	VSOut o;
	float4 Vertex = mul(u_world, float4(i.pos, 1.0));
	o.pos = mul(u_proj, mul(u_view, Vertex));
	float3 Normal = mul((float3x3)u_world, i.normal);
	float3 viewVec = normalize(u_eye - Vertex.xyz);

	o.v.tex0.xy = Flip(i.tex0);

	float4 color = i.color;
	color.rgb += u_ambLight.rgb*surfAmbient;
	color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse*u_reflProps.y;
	color = clamp(color, 0.0, 1.0);
	o.v.color = color*u_matColor;

	// reflect V along Normal
	float3 uv2 = Normal*dot(viewVec, Normal)*2.0 - viewVec;
	o.v.tex1 = Flip(uv2.xy*0.5 + 0.5);
	float b = 1.0 - clamp(dot(viewVec, Normal), 0.0, 1.0);
	float4 refl = float4(0.0, 0.0, 0.0, 1.0);
	refl.a = lerp(b*b*b*b*b, 1.0, u_reflProps.x)*u_reflProps.z;
	for(int k = 0; k < 5; k++)
		refl.rgb += DoDirLightSpec(u_specDir[k].xyz, u_specColor[k].rgb, Normal, viewVec, u_specDir[k].w)*u_reflProps.w*u_reflProps.y;
	o.v.color2 = refl;

	o.v.tex0.z = DoFog(o.pos.w);
	return o;
}
