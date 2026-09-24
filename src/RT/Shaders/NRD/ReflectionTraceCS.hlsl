// SkyrimRT M8 reflections (path-tracing step 4): glossy rays sampled from the GGX lobe of the G-buffer's normal and
// roughness, as REBLUR_SPECULAR's noisy input.
//
// A pixel has a reflection term when Lighting.hlsl wrote a non-zero REFLECTANCE (Dynamic Cubemaps: F0 x A + B of the split
// sum, for True PBR, dynamic-cubemap environment maps, wet surfaces, skin and hair). DeferredCompositeCS multiplies it by
// the incoming light along the reflection lobe, which it takes from the cubemaps; this trace supplies that light instead.
// The reflected surface is shaded like a GI hit, plus one bounce more (GIPath.hlsli: its own ambient scaled by the light
// its continuation finds, as the composite scales the ambient of the surfaces on screen). A ray that finds nothing within
// the sun's range sees the sky: SkyRadiance, the radiance the GI's sky light integrates to the game's ambient.
// Pixels without a reflection term get a view Z beyond NRD's denoising range, which REBLUR skips.
//
// Full resolution: one thread and one ray per pixel. Half resolution: one thread per 2x2 block, which writes the guides of
// all four pixels and traces one ray, from each of the block's reflective pixels in turn over frames, its radiance scaled
// by their count (divided by the probability of being picked). The others carry hit distance 0, NRD's "no data", which
// REBLUR fills in with its hit-distance reconstruction and pre-pass (set up on the C++ side). A quarter of the rays.

#define SKYRIMRT_ALBEDO_ATLAS
#include "GIPath.hlsli"

Texture2D<float4> GBufferNormal : register(t2);  // CS's NORMALROUGHNESS: xy octahedral view-space normal, z glossiness
Texture2D<float3> Reflectance : register(t4);    // Deferred's REFLECTANCE: the composite's weight for its reflection term
RWTexture2D<float> OutViewZ : register(u0);
RWTexture2D<float4> OutNormalRoughness : register(u1);
RWTexture2D<float4> OutRadianceHitDist : register(u3);

static const uint kReflectionSalt = 0x68E31DA4u;  // independent of the GI ray's Random2

// Bounded VNDF sampling of isotropic Smith-GGX (Eto and Tokuyoshi 2023, the sampler NRD recommends): the reflected
// direction for a view direction a_view in tangent space (z = normal). Fewer samples than plain VNDF reflect below the
// surface.
float3 SampleReflectionBoundedVNDF(float3 a_view, float a_alpha, float2 a_u)
{
	const float3 viewStd = normalize(float3(a_view.xy * a_alpha, a_view.z));
	const float phi = 6.2831853 * a_u.x;
	const float a = saturate(a_alpha);
	const float s = 1.0 + length(a_view.xy);
	const float a2 = a * a;
	const float s2 = s * s;
	const float k = (1.0 - a2) * s2 / (s2 + a2 * a_view.z * a_view.z);
	const float b = a_view.z > 0.0 ? k * viewStd.z : viewStd.z;
	const float z = mad(1.0 - a_u.y, 1.0 + b, -b);
	const float sinTheta = sqrt(saturate(1.0 - z * z));
	const float3 offsetStd = float3(sinTheta * cos(phi), sinTheta * sin(phi), z);
	const float3 microStd = viewStd + offsetStd;
	const float3 micro = normalize(float3(microStd.xy * a_alpha, microStd.z));
	return 2.0 * dot(a_view, micro) * micro - a_view;
}

struct ReflectionPixel
{
	float3 position;
	float3 toViewer;
	float3 normal;
	float distance;
	float viewZ;
	float roughness;
};

// Writes REBLUR_SPECULAR's guides for a pixel. True when it has a reflection term within the roughness limit: then its
// radiance is still to be written (TraceReflection, or NoReflectionData for a pixel skipped at half resolution).
bool PrepareReflectionPixel(int2 a_pixel, out ReflectionPixel a_out)
{
	a_out = (ReflectionPixel)0;
	const float depth = Depth[a_pixel];
	const float4 gbuffer = GBufferNormal[a_pixel];
	a_out.roughness = saturate(1.0 - gbuffer.z);
	if (depth >= 1.0 || !any(Reflectance[a_pixel] > 0.0) || a_out.roughness > C.ReflectionMaxRoughness) {
		OutViewZ[a_pixel] = C.SkyViewZ;
		OutNormalRoughness[a_pixel] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0, 0.0, 1.0), 1.0, 0.0);
		OutRadianceHitDist[a_pixel] = 0.0;
		return false;
	}

	a_out.position = PositionAt(a_pixel, depth);
	const float3 nearPoint = NearPointAt(a_pixel);
	a_out.distance = length(a_out.position - nearPoint);
	a_out.toViewer = (nearPoint - a_out.position) / max(a_out.distance, 1e-4);
	a_out.viewZ = mul(C.View, float4(a_out.position, 1.0)).z;
	a_out.normal = normalize(mul((float3x3)C.ViewInverse, DecodeGBufferNormal(gbuffer.xy)));
	if (dot(a_out.normal, a_out.toViewer) < 0.0)
		a_out.normal = -a_out.normal;

	OutViewZ[a_pixel] = a_out.viewZ;
	OutNormalRoughness[a_pixel] = NRD_FrontEnd_PackNormalAndRoughness(a_out.normal, a_out.roughness, 0.0);
	return true;
}

// Hit distance 0: no sample here this frame (NRD's convention for a skipped lobe).
void NoReflectionData(int2 a_pixel)
{
	OutRadianceHitDist[a_pixel] = 0.0;
}

// Traces one reflection ray from a prepared pixel and writes its radiance x a_scale with the hit distance.
void TraceReflection(int2 a_pixel, ReflectionPixel a_p, float a_scale, uint2 a_seedPixel)
{
	const float3 geometricNormal = ReconstructNormal(a_pixel, a_p.position, a_p.toViewer);
	const float3 tangent = normalize(cross(a_p.normal, abs(a_p.normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
	const float3 bitangent = cross(a_p.normal, tangent);
	const float3 viewTS = float3(dot(a_p.toViewer, tangent), dot(a_p.toViewer, bitangent), max(dot(a_p.toViewer, a_p.normal), 1e-4));
	const float alpha = max(a_p.roughness * a_p.roughness, 1e-4);
	const float3 directionTS = SampleReflectionBoundedVNDF(viewTS, alpha, Random2Salted(a_seedPixel, C.FrameIndex, kReflectionSalt));
	float3 direction = normalize(tangent * directionTS.x + bitangent * directionTS.y + a_p.normal * directionTS.z);
	// Keep the ray above the geometric surface (a normal map can tilt the lobe below it), as the GI ray.
	if (dot(direction, geometricNormal) < 0.0)
		direction = reflect(direction, geometricNormal);

	RayDesc ray;
	ray.Origin = a_p.position + geometricNormal * (C.NormalBias + a_p.distance * C.DistanceBias);
	ray.Direction = direction;
	ray.TMin = max(kSelfHitMin, a_p.distance * kSelfHitRelative);
	ray.TMax = kSunRayLength;  // what's reflected can be far away; past this there's no loaded geometry
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
	PROCEED_ALPHA_TESTED(query);
	const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

	float3 radiance;
	float hitDistance;
	uint deeperHits = 0;
	if (hit) {
		hitDistance = query.CommittedRayT();
		bool sunLit, lightSampled, lightOccluded;
		float occluderToLight;
		const PathVertex first = ShadeHit(Instances[query.CommittedInstanceID()], query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
			query.CommittedObjectToWorld3x4(), ray.Origin, direction, hitDistance, a_seedPixel, kReflectionStream, sunLit, lightSampled, lightOccluded, occluderToLight);
		radiance = TracePath(first, C.Bounces + 1, a_seedPixel, kReflectionStream, deeperHits);
	} else {
		hitDistance = kSunRayLength;
		radiance = SkyRadiance(direction);
	}

	const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(hitDistance, a_p.viewZ, C.HitDistParams.xyz, a_p.roughness);
	OutRadianceHitDist[a_pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance * a_scale, normHitDist, true);

	Count(kGIReflectionRays, true);
	Count(kGIReflectionHits, hit);
	CountSum(kGIReflectionDeeperHits, deeperHits);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (!C.ReflectionHalfResolution) {
		if (any(dispatchID.xy >= C.RenderSize))
			return;
		const int2 pixel = int2(dispatchID.xy);
		ReflectionPixel p;
		const bool reflective = PrepareReflectionPixel(pixel, p);
		Count(kGIReflectionTraced, reflective);
		if (reflective)
			TraceReflection(pixel, p, 1.0, dispatchID.xy);
		return;
	}

	// Half resolution: this thread's 2x2 block.
	const int2 origin = int2(dispatchID.xy) * 2;
	if (any(origin >= int2(C.RenderSize)))
		return;
	ReflectionPixel pixels[4];
	int2 coords[4];
	uint reflectiveMask = 0;
	uint count = 0;
	[unroll] for (uint i = 0; i < 4; i++) {
		coords[i] = origin + int2(i & 1, i >> 1);
		pixels[i] = (ReflectionPixel)0;
		if (all(coords[i] < int2(C.RenderSize)) && PrepareReflectionPixel(coords[i], pixels[i])) {
			reflectiveMask |= 1u << i;
			count++;
		}
	}
	CountSum(kGIReflectionTraced, count);
	if (count == 0)
		return;

	// The block's reflective pixels take turns: the (frame mod count)-th of them traces.
	const uint pick = C.FrameIndex % count;
	uint chosen = 0;
	uint seen = 0;
	[unroll] for (uint j = 0; j < 4; j++) {
		if (reflectiveMask & (1u << j)) {
			if (seen == pick)
				chosen = j;
			else
				NoReflectionData(coords[j]);
			seen++;
		}
	}
	TraceReflection(coords[chosen], pixels[chosen], float(count), uint2(coords[chosen]));
}
