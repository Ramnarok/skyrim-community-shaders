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
	uint ViewMode;  // debug view: 0 off, 1 noisy radiance, 2 denoised radiance, 3 ambient occlusion
	uint Pad;
};

// Counter slots, mirrored in GlobalIllumination.h
static const uint kGITraced = 0;
static const uint kGIHits = 1;
static const uint kGISunLitHits = 2;

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
