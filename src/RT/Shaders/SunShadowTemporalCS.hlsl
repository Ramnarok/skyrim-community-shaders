// SkyrimRT M5: temporal accumulation of the 1-ray-per-pixel sun visibility. Each pixel is reprojected into the
// previous traced frame; the 2x2 history taps whose stored view depth matches the reprojected depth are blended
// bilinearly, anything else is a disocclusion and restarts the history.

#include "SunShadowCommon.hlsli"

Texture2D<float> RasterDepth : register(t1);
Texture2D<unorm float> RawVisibility : register(t2);
Texture2D<float4> Geometry : register(t3);
Texture2D<float4> HistoryIn : register(t4);  // x: mean visibility, y: history length, z: view depth
RWTexture2D<float4> HistoryOut : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	const float depth = RasterDepth[pixel];
	const float raw = RawVisibility[pixel];
	if (depth >= 1.0) {
		HistoryOut[pixel] = float4(1.0, 0.0, 0.0, 0.0);
		return;
	}

	const float3 position = PositionAt(pixel, depth);
	const float3 normal = Geometry[pixel].xyz;
	const float viewDepth = mul(C.ViewProj, float4(position, 1.0)).w;

	float historyMean = raw;
	float historyLength = 0.0;
	if (C.HistoryValid) {
		const float4 prevClip = mul(C.PrevViewProj, float4(position + C.PosAdjustDelta.xyz, 1.0));
		if (prevClip.w > 0.0) {
			const float2 prevNdc = prevClip.xy / prevClip.w;
			const float2 prevPixel = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * float2(C.PrevRenderSize) - 0.5;
			const int2 base = int2(floor(prevPixel));
			const float2 f = prevPixel - base;

			// At grazing angles neighbouring pixels differ a lot in depth, so widen the tolerance with 1 / N.V.
			const float3 toViewer = normalize(NearPointAt(pixel) - position);
			const float tolerance = C.DepthTolerance / max(abs(dot(normal, toViewer)), 0.1) * prevClip.w;

			float sumMean = 0.0, sumLength = 0.0, sumWeight = 0.0;
			[unroll] for (int i = 0; i < 4; i++)
			{
				const int2 offset = int2(i & 1, i >> 1);
				const int2 tap = base + offset;
				if (any(tap < 0) || any(tap >= int2(C.PrevRenderSize)))
					continue;
				const float4 h = HistoryIn[tap];
				if (h.y <= 0.0 || abs(h.z - prevClip.w) > tolerance)
					continue;
				const float w = (offset.x ? f.x : 1.0 - f.x) * (offset.y ? f.y : 1.0 - f.y);
				sumMean += h.x * w;
				sumLength += h.y * w;
				sumWeight += w;
			}
			if (sumWeight > 1e-3) {
				historyMean = sumMean / sumWeight;
				historyLength = sumLength / sumWeight;
			}
		}
	}

	const float newLength = min(historyLength + 1.0, C.MaxHistory);
	const float mean = lerp(historyMean, raw, 1.0 / newLength);
	HistoryOut[pixel] = float4(mean, newLength, viewDepth, 0.0);
}
