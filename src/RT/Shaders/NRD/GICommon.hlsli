// SkyrimRT M6: shared declarations for the ray-traced GI passes (trace, resolve). Built only with SKYRIMRT_NRD.

#include "NRD.hlsli"

struct GIConstants
{
	row_major float4x4 ViewProjInverse;  // FrameBuffer::CameraViewProjInverse (jittered, camera-relative)
	row_major float4x4 View;             // FrameBuffer::CameraView: camera-relative world -> view
	row_major float4x4 ViewInverse;      // FrameBuffer::CameraViewInverse: G-buffer view-space normals -> world
	float4 ToSun;                        // xyz: unit direction towards SharedData's directional light
	float4 SunColor;                     // rgb: SharedData::DirLightColor, as Lighting.hlsl gets it
	float4 AmbientSHR;                   // SharedData::AmbientSH*: the game's directional ambient as L1 SH
	float4 AmbientSHG;
	float4 AmbientSHB;
	float4 HitDistParams;  // xyz: REBLUR hit distance normalization (A in game units, B, C), w: GI ray length
	uint2 RenderSize;
	uint FrameIndex;
	uint CasterMask;
	float NormalBias;
	float DistanceBias;
	float SkyViewZ;  // written for sky pixels, beyond CommonSettings::denoisingRange
	uint LinearLighting;
	float Intensity;   // scales the bounce light fed to the composite
	float AOStrength;  // 0 = no RT ambient occlusion, 1 = full
	float ColorGamma;  // Linear Lighting conversions (used only when LinearLighting != 0)
	float LightGamma;
	float AmbientGamma;
	float AmbientMult;
	uint ViewMode;  // debug view: 0 off, 1 noisy radiance, 2 denoised radiance, 3 ambient occlusion, 4/5 reflections noisy/denoised
	uint Interior;  // the game doesn't shadow an interior's directional light, so neither does the bounce
	uint PointLightCount;
	uint PointLightShadows;     // trace a visibility ray to the sampled point light
	uint InverseSquare;         // Inverse Square Lighting loaded: Lighting.hlsl uses its attenuation (ISL define)
	float DirectionalLightMult; // Linear Lighting
	uint SkyLight;              // M8: misses that reach the sky carry its radiance (exteriors); the composite scales the ambient
	uint Bounces;               // M8 multi-bounce: path vertices, 1 = the M6 single bounce
	uint Reflections;              // M8 reflections: trace a glossy ray from every pixel with a reflection term
	float ReflectionMaxRoughness;  // ... whose G-buffer roughness is at most this
	uint ReflectionHalfResolution; // ... one ray per 2x2 block (ReflectionTraceCS)
	uint Water;                    // M8 water: water planes (kMaskWater) in front of the G-buffer reflect too (Water.hlsl t47)
	float WaterRoughness;          // ... with this roughness (the waves are Water.hlsl's normal maps)
	row_major float4x4 ViewProjUnjittered;  // M8 water: camera-relative world -> this frame's unjittered clip
	row_major float4x4 PrevViewProj;        // ... -> the previous frame's (its view folded into this origin)
};

// Counter slots, mirrored in GlobalIllumination.h
static const uint kGITraced = 0;
static const uint kGIHits = 1;
static const uint kGISunLitHits = 2;
static const uint kGILightSampled = 3;
static const uint kGILightOccluded = 4;
static const uint kGIOccluderNear32 = 5;   // occluded samples by the blocker's distance from the light (game units)
static const uint kGIOccluderNear64 = 6;
static const uint kGIOccluderNear128 = 7;
static const uint kGISkyVisible = 8;       // misses whose continuation reached the sky (SkyLight)
static const uint kGITexturedHits = 9;     // M8: hits shaded with the texture from the albedo atlas (else the average)
static const uint kGIDeeperHits = 10;      // M8 multi-bounce: hits of continuation rays (second bounce and deeper)
static const uint kGIReflectionTraced = 11;  // M8 reflections: pixels with a reflection term (within the roughness limit)
static const uint kGIReflectionHits = 12;    // ... rays that hit geometry (the rest see the sky)
static const uint kGIReflectionDeeperHits = 13;  // ... hits of the reflected surface's continuation rays
static const uint kGIReflectionRays = 14;        // ... reflection rays traced (a quarter or so at half resolution)
static const uint kGIWaterPixels = 15;            // M8 water: pixels whose reflecting surface is a water plane (in kGIReflectionTraced too)

ConstantBuffer<GIConstants> C : register(b0);

float3 Unproject(float2 a_ndc, float a_depth)
{
	const float4 p = mul(C.ViewProjInverse, float4(a_ndc, a_depth, 1.0));
	return p.xyz / p.w;
}

float2 PixelToNdc(int2 a_pixel)
{
	const float2 uv = (a_pixel + 0.5) / float2(C.RenderSize);
	return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float3 PositionAt(int2 a_pixel, float a_depth)
{
	return Unproject(PixelToNdc(a_pixel), a_depth);
}

float3 NearPointAt(int2 a_pixel)
{
	return Unproject(PixelToNdc(a_pixel), 0.0);
}

uint Hash(uint a_value)
{
	uint x = a_value;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float2 Random2(uint2 a_pixel, uint a_frame)
{
	const uint h = Hash(a_pixel.x | (a_pixel.y << 16));
	const float2 base = float2(h & 0xFFFFu, h >> 16) / 65536.0;
	return frac(base + float2(0.7548776662, 0.5698402910) * float(a_frame & 1023u));
}

// Independent of Random2: white noise per pixel and frame.
float Random1(uint2 a_pixel, uint a_frame)
{
	return float(Hash(Hash(a_pixel.x | (a_pixel.y << 16)) ^ (a_frame * 0x9E3779B9u)) >> 8) / 16777216.0;
}

// CS's Color::SkyrimGammaToLinear (Color.hlsli).
float3 SkyrimGammaToLinear(float3 a_color)
{
	return pow(abs(a_color), 1.6);
}

// CS's SphericalHarmonics::Evaluate basis (SphericalHarmonics.hlsli) and SharedData::GetAmbient.
float4 ShEvaluate(float3 a_direction)
{
	return float4(0.28209479177387814, -0.48860251190291992 * a_direction.y, 0.48860251190291992 * a_direction.z, -0.48860251190291992 * a_direction.x);
}

float3 GetAmbient(float3 a_normal)
{
	const float4 basis = ShEvaluate(a_normal);
	return max(0.0, float3(dot(C.AmbientSHR, basis), dot(C.AmbientSHG, basis), dot(C.AmbientSHB, basis)));
}

// Independent of Random2 for each a_salt: a per-pixel offset plus the R2 sequence over frames.
float2 Random2Salted(uint2 a_pixel, uint a_frame, uint a_salt)
{
	const uint h = Hash(Hash(a_pixel.x | (a_pixel.y << 16)) ^ a_salt);
	const float2 base = float2(h & 0xFFFFu, h >> 16) / 65536.0;
	return frac(base + float2(0.7548776662, 0.5698402910) * float(a_frame & 1023u));
}

// CS's Color::RGBToYCoCg (Color.hlsli).
float3 RGBToYCoCg(float3 a_color)
{
	const float tmp = 0.25 * (a_color.r + a_color.b);
	return float3(tmp + 0.5 * a_color.g, 0.5 * (a_color.r - a_color.b), -tmp + 0.5 * a_color.g);
}

// CS's GBuffer::DecodeNormal (GBuffer.hlsli): octahedral view-space normal.
float3 DecodeGBufferNormal(float2 a_encoded)
{
	float2 f = a_encoded * 2.0 - 1.0;
	float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	const float t = saturate(-n.z);
	n.xy += select(n.xy >= 0.0, -t, t);
	return -normalize(n);
}
