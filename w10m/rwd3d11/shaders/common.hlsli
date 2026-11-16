// What every shader of the Direct3D 11 renderer shares: the uniforms librw's shaders have
// (vendor/librw/src/gl/shaders/header.vert and header.frag), grouped into constant buffers by
// how often they change, the lights, the fog and the alpha test.
//
// The names are those of the OpenGL shaders: the renderer finds a uniform's place in a
// compiled shader by its name (tools/make_shaders.py reads them out of the compiler's listing).
//
// Textures are kept as the OpenGL renderer keeps them, their rows bottom-up (that is how the
// game's converted textures are stored, in txd.img), and sampled at (u, 1-v). The vertex
// shaders turn v over, so that a pixel shader samples at what it is handed.
//
// Everything a vertex shader hands on has a TEXCOORD semantic: at Direct3D's level 9 a COLOR
// is clamped and of low precision. The position comes last, since the pixel shader does not
// take it and has to read a leading part of what the vertex shader writes.
#ifndef COMMON_HLSLI
#define COMMON_HLSLI

#define MAX_LIGHTS 8

cbuffer State
{
	float4 u_alphaRef;	// alpha below .x, or not below .y, is not drawn
	float4 u_fogData;	// start, end, range, disable
	float4 u_fogColor;
};

cbuffer Scene
{
	float4x4 u_proj;
	float4x4 u_view;
};

cbuffer Object
{
	float4x4 u_world;
	float4 u_ambLight;
};

cbuffer Material
{
	float4 u_matColor;
	float4 u_surfProps;	// amb, spec, diff, extra
};

cbuffer Lights
{
	float4 u_lightParams[MAX_LIGHTS];	// type, radius, minusCosAngle, hardSpot
	float4 u_lightPosition[MAX_LIGHTS];
	float4 u_lightDirection[MAX_LIGHTS];
	float4 u_lightColor[MAX_LIGHTS];
};

#define surfAmbient (u_surfProps.x)
#define surfSpecular (u_surfProps.y)
#define surfDiffuse (u_surfProps.z)

// The lights after the last one have a type of nought (the renderer sees to it): there is no
// leaving the loop early in a shader of this level, each light counts or does not.
float3 DoDynamicLight(float3 V, float3 N)
{
	float3 color = float3(0.0, 0.0, 0.0);
#if defined(DIRECTIONALS) || defined(POINTLIGHTS) || defined(SPOTLIGHTS)
	[loop] for(int i = 0; i < MAX_LIGHTS; i++){
		float type = u_lightParams[i].x;
#ifdef DIRECTIONALS
		{
			float l = max(0.0, dot(N, -u_lightDirection[i].xyz));
			color += (type == 1.0 ? l : 0.0)*u_lightColor[i].rgb;
		}
#endif
#if defined(POINTLIGHTS) || defined(SPOTLIGHTS)
		{
			float3 dir = V - u_lightPosition[i].xyz;
			float dist = max(length(dir), 0.0001);
			float atten = max(0.0, 1.0 - dist/max(u_lightParams[i].y, 0.0001));
			dir /= dist;
			float l = max(0.0, dot(N, -dir));
#ifdef POINTLIGHTS
			color += (type == 2.0 ? l*atten : 0.0)*u_lightColor[i].rgb;
#endif
#ifdef SPOTLIGHTS
			float pcos = dot(dir, u_lightDirection[i].xyz);	// cos to point
			float ccos = -u_lightParams[i].z;
			float falloff = (pcos-ccos)/(1.0-ccos);
			float spot = falloff < 0.0 ? 0.0 : l*max(falloff, u_lightParams[i].w);
			color += (type == 3.0 ? spot*atten : 0.0)*u_lightColor[i].rgb;
#endif
		}
#endif
	}
#endif
	return color;
}

float DoFog(float w)
{
	return clamp((w - u_fogData.y)*u_fogData.z, u_fogData.w, 1.0);
}

void DoAlphaTest(float a)
{
#ifndef NO_ALPHATEST
	if(a < u_alphaRef.x || a >= u_alphaRef.y)
		discard;
#endif
}

// (u, v) as a texture kept bottom-up is sampled
float2 Flip(float2 uv)
{
	return float2(uv.x, 1.0 - uv.y);
}

// What the vertex shaders hand on. tex0.z is the fog.
struct Simple	// simple.frag and what is put in its place
{
	float4 color : TEXCOORD0;
	float3 tex0 : TEXCOORD1;
};

struct Atlas	// Simple, and where the vertex's texture is in an atlas
{
	float4 color : TEXCOORD0;
	float3 tex0 : TEXCOORD1;
	float4 atlas : TEXCOORD2;
};

struct TwoTextures	// a second set of texture coordinates
{
	float4 color : TEXCOORD0;
	float3 tex0 : TEXCOORD1;
	float2 tex1 : TEXCOORD2;
};

struct TwoColours	// and a second colour: environment maps
{
	float4 color : TEXCOORD0;
	float3 tex0 : TEXCOORD1;
	float2 tex1 : TEXCOORD2;
	float4 color2 : TEXCOORD3;
};

#endif
