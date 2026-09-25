// SkyrimRT M6: diffuse GI, one cosine-weighted ray per pixel from the finished G-buffer (DXR 1.1 inline RayQuery); M8
// multi-bounce continues the path from each hit up to C.Bounces vertices, scaling each vertex's ambient by the light its
// continuation brought in (GIPath.hlsli). Output is REBLUR's noisy input. Misses carry no radiance (the composite keeps
// the game's ambient, scaled by the ambient occlusion REBLUR derives from the hit distance), except with M8 sky light
// (exteriors): a miss that reaches the sky carries the sky's radiance, and the composite scales the game's ambient by
// the traced light over the open-sky light.

#define SKYRIMRT_ALBEDO_ATLAS
#include "GIPath.hlsli"

Texture2D<float4> GBufferNormal : register(t2);  // CS's NORMALROUGHNESS: xy octahedral view-space normal, z glossiness
Texture2D<float2> GameMotionVectors : register(t3);  // kMOTION_VECTOR: prevUV - currUV, NRD's convention already
RWTexture2D<float> OutViewZ : register(u0);
RWTexture2D<float4> OutNormalRoughness : register(u1);
RWTexture2D<float2> OutMotionVectors : register(u2);
RWTexture2D<float4> OutRadianceHitDist : register(u3);
// M9 phase 5 (C.SkySplit): the sky light a ray brings over the open-sky light at the pixel (luminance), as
// 1 - ratio / kSkyRatioScale: REBLUR_DIFFUSE_OCCLUSION's input, read back by DeferredCompositeCS at t18.
RWTexture2D<float> OutSkyRatio : register(u5);
static const float kSkyRatioScale = 4.0;  // mirrored in DeferredCompositeCS and GlobalIllumination.cpp's dump

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	OutMotionVectors[pixel] = GameMotionVectors[pixel];

	const float depth = Depth[pixel];
	if (depth >= 1.0) {
		OutViewZ[pixel] = C.SkyViewZ;
		OutNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0, 0.0, 1.0), 1.0, 0.0);
		OutRadianceHitDist[pixel] = 0.0;
		if (C.SkySplit)
			OutSkyRatio[pixel] = 1.0;
		Count(kGITraced, false);
		return;
	}

	const float3 position = PositionAt(pixel, depth);
	const float3 nearPoint = NearPointAt(pixel);
	const float distance = length(position - nearPoint);
	const float3 toViewer = (nearPoint - position) / max(distance, 1e-4);
	const float viewZ = mul(C.View, float4(position, 1.0)).z;

	float3 normal = normalize(mul((float3x3)C.ViewInverse, DecodeGBufferNormal(GBufferNormal[pixel].xy)));
	if (dot(normal, toViewer) < 0.0)
		normal = -normal;
	const float3 geometricNormal = ReconstructNormal(pixel, position, toViewer);

	// Sample around the shading normal, but keep the ray above the geometric surface.
	float3 direction = CosineSampleHemisphere(normal, Random2(dispatchID.xy, C.FrameIndex));
	if (dot(direction, geometricNormal) < 0.0)
		direction = reflect(direction, geometricNormal);

	RayDesc ray;
	ray.Origin = position + geometricNormal * (C.NormalBias + distance * C.DistanceBias);
	ray.Direction = direction;
	// Skip surfaces within the depth-metric tolerance of the start: where the traced mesh sits slightly in front of
	// the drawn one (tree sway between draws, leaf flutter), rays would start inside it and darken it with full AO.
	ray.TMin = max(kSelfHitMin, distance * kSelfHitRelative);
	ray.TMax = C.HitDistParams.w;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
	PROCEED_ALPHA_TESTED(query);
	const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

	float3 radiance = 0.0;
	float hitDistance = C.HitDistParams.w;
	bool sunLit = false;
	bool lightSampled = false;
	bool lightOccluded = false;
	float occluderToLight = -1.0;
	bool texturedHit = false;
	uint deeperHits = 0;
	if (hit) {
		hitDistance = query.CommittedRayT();
		const InstanceData instance = Instances[query.CommittedInstanceID()];
		texturedHit = ((instance.Flags >> 8) & 0xFFFu) != 0;
		const PathVertex first = ShadeHit(instance, query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(),
			ray.Origin, direction, hitDistance, dispatchID.xy, 0, sunLit, lightSampled, lightOccluded, occluderToLight);
		radiance = TracePath(first, C.Bounces, dispatchID.xy, 0, deeperHits);
	}
	// M8 sky light: a miss within the GI ray length isn't sky yet. It continues, as an any-hit visibility ray, to the
	// sun's range: past it there's no geometry in the loaded cells (distant LOD isn't in the TLAS).
	bool skyVisible = false;
	if (!hit && C.SkyLight) {
		skyVisible = !Occluded(ray.Origin + direction * ray.TMax, direction, kSunRayLength);
		if (skyVisible)
			radiance = SkyRadiance(direction);
	}
	// M9 phase 5: the sky goes to its own signal, which scales the game's ambient; REBLUR keeps the bounce, which the
	// composite adds as light. Ray by ray the ratio averages to the traced over the open-sky light (1 under open sky).
	if (C.SkySplit) {
		static const float3 kLuminance = float3(0.2126, 0.7152, 0.0722);
		const float ratio = skyVisible ? dot(radiance, kLuminance) / max(dot(OpenSkyRadiance(normal), kLuminance), 1e-4) : 0.0;
		OutSkyRatio[pixel] = 1.0 - saturate(ratio / kSkyRatioScale);
		if (skyVisible)
			radiance = 0.0;
	}

	OutViewZ[pixel] = viewZ;
	OutNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, 1.0, 0.0);
	const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, C.HitDistParams.xyz, 1.0);
	OutRadianceHitDist[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist, true);

	Count(kGITraced, true);
	Count(kGIHits, hit);
	Count(kGISunLitHits, sunLit);
	Count(kGILightSampled, lightSampled);
	Count(kGILightOccluded, lightOccluded);
	// Where the blocker sits: a light's own fixture is within a few dozen units of it, a wall usually isn't.
	Count(kGIOccluderNear32, lightOccluded && occluderToLight < 32.0);
	Count(kGIOccluderNear64, lightOccluded && occluderToLight >= 32.0 && occluderToLight < 64.0);
	Count(kGIOccluderNear128, lightOccluded && occluderToLight >= 64.0 && occluderToLight < 128.0);
	Count(kGISkyVisible, skyVisible);
	Count(kGITexturedHits, texturedHit);
	CountSum(kGIDeeperHits, deeperHits);
}
