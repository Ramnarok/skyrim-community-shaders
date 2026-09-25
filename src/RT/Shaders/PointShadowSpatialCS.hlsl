// SkyrimRT M9 phase 2: SunShadowSpatialCS for the four-channel point-light visibility. Same plane-aware Poisson filter
// with a radius that shrinks as the history grows, applied to all four channels; writes the RGBA8 mask shared with D3D11
// (PS t46: xyz the hero lights, w the rest) and the optional debug view.

#include "PointShadowHistory.hlsli"
#include "SunShadowCommon.hlsli"

Texture2D<float> RasterDepth : register(t1);
Texture2D<uint4> History : register(t2);
Texture2D<float4> Geometry : register(t3);
Texture2D<unorm float4> RawVisibility : register(t4);
RWTexture2D<unorm float4> Mask : register(u0);
RWTexture2D<unorm float4> View : register(u1);

static const uint kTapCount = 12;
static const float2 kPoisson[kTapCount] = {
	float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
	float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
	float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)
};

// Debug view: the hero lights as red, green and blue, darkened by the rest.
float4 ViewColour(float4 a_visibility)
{
	return float4(a_visibility.xyz * a_visibility.w, 1.0);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	const float depth = RasterDepth[pixel];
	float4 result = 1.0;
	if (depth < 1.0) {
		const PointShadowHistory center = UnpackPointShadowHistory(History[pixel]);
		const float3 normal = Geometry[pixel].xyz;
		const float3 position = PositionAt(pixel, depth);
		const float distance = length(position - NearPointAt(pixel));
		const float radius = C.SpatialRadius * lerp(1.0, 0.35, saturate(center.Length / C.MaxHistory));

		float4 sum = center.Means;
		float sumWeight = 1.0;
		if (radius >= 0.5) {
			const float angle = 6.2831853 * Random2(dispatchID.xy, C.FrameIndex).x;
			float sine, cosine;
			sincos(angle, sine, cosine);
			const float planeTolerance = C.PlaneTolerance * distance + 1.0;
			const int2 maxPixel = int2(C.RenderSize) - 1;
			[unroll] for (uint i = 0; i < kTapCount; i++)
			{
				const float2 k = kPoisson[i];
				const float2 offset = float2(k.x * cosine - k.y * sine, k.x * sine + k.y * cosine) * radius;
				const int2 tap = clamp(int2(floor(pixel + 0.5 + offset)), 0, maxPixel);
				const float tapDepth = RasterDepth[tap];
				if (all(tap == pixel) || tapDepth >= 1.0)
					continue;
				const float planeDistance = abs(dot(normal, PositionAt(tap, tapDepth) - position));
				const float w = saturate(1.0 - planeDistance / planeTolerance) *
				                pow(saturate(dot(normal, Geometry[tap].xyz)), 8.0) *
				                exp(-2.0 * dot(k, k));
				sum += UnpackPointShadowHistory(History[tap]).Means * w;
				sumWeight += w;
			}
		}
		result = sum / sumWeight;
	}
	Mask[pixel] = result;

	if (C.ViewMode != 0) {
		const float4 value = C.ViewMode == 1 ? RawVisibility[pixel] : result;
		View[pixel] = depth >= 1.0 ? float4(0.0, 0.0, 0.25, 1.0) : ViewColour(value);
	}
}
