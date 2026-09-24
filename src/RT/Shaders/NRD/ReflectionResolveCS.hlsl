// SkyrimRT M8 reflections: REBLUR_SPECULAR's denoised output into the texture DeferredCompositeCS reads at t17: rgb the
// incoming light along the reflection lobe (linear, as the composite's cubemap term), a = 1 where it was traced. The
// composite replaces its cubemap reflection with it there.
// M8 water: pixels whose reflecting surface is a water plane go to the water texture (Water.hlsl t47) instead: rgb the same
// light, a = the traced water surface's view Z, which Water.hlsl checks against its own. t17 is 0 there, so the composite
// keeps the cubemap for the (underwater) surface it shades.

#include "GICommon.hlsli"

Texture2D<float> Depth : register(t1);
Texture2D<float4> Denoised : register(t2);  // OUT_SPEC_RADIANCE_HITDIST
Texture2D<float> SpecularViewZ : register(t3);  // REBLUR_SPECULAR's IN_VIEWZ: beyond the denoising range where not traced
Texture2D<float4> Noisy : register(t4);  // IN_SPEC_RADIANCE_HITDIST (debug view)
RWTexture2D<float4> OutReflections : register(u0);
RWTexture2D<float4> OutWater : register(u1);
RWTexture2D<float> WaterViewZ : register(u2);  // written by ReflectionTraceCS, 0 where not water
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
	const float waterViewZ = C.Water ? WaterViewZ[pixel] : 0.0;
	if (C.Water)
		OutWater[pixel] = 0.0;  // overwritten below where the water was traced within the denoising range

	if ((Depth[pixel] >= 1.0 && waterViewZ == 0.0) || SpecularViewZ[pixel] >= 0.5 * C.SkyViewZ) {
		OutReflections[pixel] = 0.0;
		if (C.ViewMode >= 4)
			OutView[pixel] = float4(0.0, 0.0, 0.25, 1.0);
		return;
	}

	const float3 radiance = max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Denoised[pixel]).rgb, 0.0);
	if (waterViewZ != 0.0) {
		OutReflections[pixel] = 0.0;
		OutWater[pixel] = float4(C.WaterDebug ? float3(4.0, 0.0, 4.0) : radiance, waterViewZ);
	} else {
		OutReflections[pixel] = float4(radiance, 1.0);
	}

	if (C.ViewMode == 4)
		OutView[pixel] = float4(ToneMap(max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Noisy[pixel]).rgb, 0.0)), 1.0);
	else if (C.ViewMode == 5)
		OutView[pixel] = float4(ToneMap(radiance), 1.0);
}
