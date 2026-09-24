// SkyrimRT M6: converts REBLUR's denoised diffuse output into the three textures DeferredCompositeCS reads from
// Screen-Space GI (t10 AO, t11 luminance as L1 SH, t12 CoCg chroma), so the composite needs no change.
//
// The composite evaluates il = YCoCgToRGB(SHHallucinateZH3Irradiance(Y, N), CoCg) at the G-buffer normal N and adds
// il * albedo. REBLUR's output is already the cosine-weighted mean incoming radiance at that normal, so Y carries its
// luminance in L0 (divided by the cosine-lobe L0 weight 0.886), plus a small L1 term along N: an all-zero L1 would
// make SHHallucinateZH3Irradiance normalize a zero vector. The scale below compensates for that L1 term.

#include "GICommon.hlsli"

Texture2D<float> Depth : register(t1);
Texture2D<float4> Denoised : register(t2);  // OUT_DIFF_RADIANCE_HITDIST
Texture2D<float4> NormalRoughness : register(t3);  // our IN_NORMAL_ROUGHNESS
Texture2D<float4> Noisy : register(t4);  // IN_DIFF_RADIANCE_HITDIST (debug view)
RWTexture2D<unorm float> OutAO : register(u0);  // occlusion: the composite uses 1 - this
RWTexture2D<float4> OutY : register(u1);
RWTexture2D<float2> OutCoCg : register(u2);
RWTexture2D<unorm float4> OutView : register(u3);

static const float kL0Weight = 0.8862269254527580;  // SphericalHarmonics::EvaluateCosineLobe L0
static const float kL1Weight = 1.0233267079464885;  // ... L1
static const float kL1Fraction = 0.01;

float3 ToneMap(float3 a_color)
{
	return a_color / (1.0 + a_color);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	if (Depth[pixel] >= 1.0) {
		OutAO[pixel] = 0.0;
		OutY[pixel] = 0.0;
		OutCoCg[pixel] = 0.0;
		if (C.ViewMode != 0)
			OutView[pixel] = float4(0.0, 0.0, 0.25, 1.0);
		return;
	}

	const float4 denoised = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Denoised[pixel]);
	const float3 radiance = max(denoised.rgb, 0.0) * C.Intensity;
	const float visibility = saturate(denoised.w);  // normalized hit distance: REBLUR's ambient occlusion
	const float occlusion = C.AOStrength * (1.0 - visibility);

	const float3 normal = NRD_FrontEnd_UnpackNormalAndRoughness(NormalRoughness[pixel]).xyz;
	const float3 ycocg = RGBToYCoCg(radiance);
	const float l0 = ycocg.x / (kL0Weight + kL1Fraction * kL1Weight);
	const float l1 = kL1Fraction * l0;

	OutAO[pixel] = occlusion;
	OutY[pixel] = float4(l0, -l1 * normal.y, l1 * normal.z, -l1 * normal.x);  // SphericalHarmonics::Evaluate order
	OutCoCg[pixel] = ycocg.yz;

	if (C.ViewMode == 1)
		OutView[pixel] = float4(ToneMap(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Noisy[pixel]).rgb * C.Intensity), 1.0);
	else if (C.ViewMode == 2)
		OutView[pixel] = float4(ToneMap(radiance), 1.0);
	else if (C.ViewMode == 3)
		OutView[pixel] = float4((1.0 - occlusion).xxx, 1.0);
	else if (C.ViewMode >= 4)
		OutView[pixel] = float4(0.0, 0.0, 0.0, 1.0);  // reflection views: ReflectionResolveCS writes over this when it runs
}
