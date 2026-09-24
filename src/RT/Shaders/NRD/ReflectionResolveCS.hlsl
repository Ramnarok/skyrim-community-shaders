// SkyrimRT M8 reflections: REBLUR_SPECULAR's denoised output into the texture DeferredCompositeCS reads at t17: rgb the
// incoming light along the reflection lobe (linear, as the composite's cubemap term), a = 1 where it was traced. The
// composite replaces its cubemap reflection with it there.

#include "GICommon.hlsli"

Texture2D<float> Depth : register(t1);
Texture2D<float4> Denoised : register(t2);  // OUT_SPEC_RADIANCE_HITDIST
Texture2D<float> SpecularViewZ : register(t3);  // REBLUR_SPECULAR's IN_VIEWZ: beyond the denoising range where not traced
Texture2D<float4> Noisy : register(t4);  // IN_SPEC_RADIANCE_HITDIST (debug view)
RWTexture2D<float4> OutReflections : register(u0);
RWTexture2D<unorm float4> OutView : register(u3);

float3 ToneMap(float3 a_color)
{
	return a_color / (1.0 + a_color);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	if (Depth[pixel] >= 1.0 || SpecularViewZ[pixel] >= 0.5 * C.SkyViewZ) {
		OutReflections[pixel] = 0.0;
		if (C.ViewMode >= 4)
			OutView[pixel] = float4(0.0, 0.0, 0.25, 1.0);
		return;
	}

	const float3 radiance = max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Denoised[pixel]).rgb, 0.0);
	OutReflections[pixel] = float4(radiance, 1.0);

	if (C.ViewMode == 4)
		OutView[pixel] = float4(ToneMap(max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Noisy[pixel]).rgb, 0.0)), 1.0);
	else if (C.ViewMode == 5)
		OutView[pixel] = float4(ToneMap(radiance), 1.0);
}
