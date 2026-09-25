// SkyrimRT M9 phase 2: SunShadowTemporalCS for the four-channel point-light visibility (three hero lights and the rest).
// Same reprojection and depth test; the history is packed (PointShadowHistory.hlsli). A hero light that changed this
// frame (HeroResetMask) restarts the history, since its channel described another light until now.

#include "PointShadowHistory.hlsli"
#include "SunShadowCommon.hlsli"

Texture2D<float> RasterDepth : register(t1);
Texture2D<unorm float4> RawVisibility : register(t2);
Texture2D<float4> Geometry : register(t3);
Texture2D<uint4> HistoryIn : register(t4);
RWTexture2D<uint4> HistoryOut : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	const float depth = RasterDepth[pixel];
	const float4 raw = RawVisibility[pixel];
	PointShadowHistory result;
	if (depth >= 1.0) {
		result.Means = 1.0;
		result.Length = 0.0;
		result.ViewDepth = 0.0;
		HistoryOut[pixel] = PackPointShadowHistory(result);
		return;
	}

	const float3 position = PositionAt(pixel, depth);
	const float3 normal = Geometry[pixel].xyz;
	const float viewDepth = mul(C.ViewProj, float4(position, 1.0)).w;

	float4 historyMeans = raw;
	float historyLength = 0.0;
	if (C.HistoryValid && C.HeroResetMask == 0) {
		const float4 prevClip = mul(C.PrevViewProj, float4(position + C.PosAdjustDelta.xyz, 1.0));
		if (prevClip.w > 0.0) {
			const float2 prevNdc = prevClip.xy / prevClip.w;
			const float2 prevPixel = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * float2(C.PrevRenderSize) - 0.5;
			const int2 base = int2(floor(prevPixel));
			const float2 f = prevPixel - base;

			// At grazing angles neighbouring pixels differ a lot in depth, so widen the tolerance with 1 / N.V.
			const float3 toViewer = normalize(NearPointAt(pixel) - position);
			const float tolerance = C.DepthTolerance / max(abs(dot(normal, toViewer)), 0.1) * prevClip.w;

			float4 sumMeans = 0.0;
			float sumLength = 0.0, sumWeight = 0.0;
			[unroll] for (int i = 0; i < 4; i++)
			{
				const int2 offset = int2(i & 1, i >> 1);
				const int2 tap = base + offset;
				if (any(tap < 0) || any(tap >= int2(C.PrevRenderSize)))
					continue;
				const PointShadowHistory h = UnpackPointShadowHistory(HistoryIn[tap]);
				if (h.Length <= 0.0 || abs(h.ViewDepth - prevClip.w) > tolerance)
					continue;
				const float w = (offset.x ? f.x : 1.0 - f.x) * (offset.y ? f.y : 1.0 - f.y);
				sumMeans += h.Means * w;
				sumLength += h.Length * w;
				sumWeight += w;
			}
			if (sumWeight > 1e-3) {
				historyMeans = sumMeans / sumWeight;
				historyLength = sumLength / sumWeight;
			}
		}
	}

	result.Length = min(historyLength + 1.0, C.MaxHistory);
	result.Means = lerp(historyMeans, raw, 1.0 / result.Length);
	result.ViewDepth = viewDepth;
	HistoryOut[pixel] = PackPointShadowHistory(result);
}
