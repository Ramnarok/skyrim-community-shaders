// SkyrimRT M6: one-bounce diffuse GI, one cosine-weighted ray per pixel from the finished G-buffer (DXR 1.1 inline
// RayQuery). The radiance leaving each hit follows the rule CS's deferred lighting uses for the surfaces Screen-Space
// GI gathers: average albedo x (sun x N.L x sun visibility + the game's directional ambient), in Skyrim gamma, then
// Color::RadianceToLinear. Output is REBLUR's noisy input; sky and misses carry no radiance (the composite keeps the
// game's ambient, scaled by the ambient occlusion REBLUR derives from the hit distance).

#include "GICommon.hlsli"
#include "MeshData.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> GBufferNormal : register(t2);  // CS's NORMALROUGHNESS: xy octahedral view-space normal, z glossiness
Texture2D<float2> GameMotionVectors : register(t3);  // kMOTION_VECTOR: prevUV - currUV, NRD's convention already
StructuredBuffer<InstanceData> Instances : register(t5);  // root SRV
RWTexture2D<float> OutViewZ : register(u0);
RWTexture2D<float4> OutNormalRoughness : register(u1);
RWTexture2D<float2> OutMotionVectors : register(u2);
RWTexture2D<float4> OutRadianceHitDist : register(u3);
RWByteAddressBuffer Counters : register(u4);

static const float kSunRayLength = 50000.0;  // as the sun-shadow trace (SunShadows::kMaxRayDistance)

void Count(uint a_slot, bool a_condition)
{
	const uint n = WaveActiveCountBits(a_condition);
	if (WaveIsFirstLane() && n > 0)
		Counters.InterlockedAdd(a_slot * 4, n);
}

// Same depth-derived geometric normal as the sun-shadow trace: per axis the neighbour nearer to the centre point.
float3 ReconstructNormal(int2 a_pixel, float3 a_position, float3 a_toViewer)
{
	const int2 maxPixel = int2(C.RenderSize) - 1;
	const int2 l = clamp(a_pixel - int2(1, 0), 0, maxPixel);
	const int2 r = clamp(a_pixel + int2(1, 0), 0, maxPixel);
	const int2 u = clamp(a_pixel - int2(0, 1), 0, maxPixel);
	const int2 d = clamp(a_pixel + int2(0, 1), 0, maxPixel);
	const float3 toRight = PositionAt(r, Depth[r]) - a_position;
	const float3 fromLeft = a_position - PositionAt(l, Depth[l]);
	const float3 toDown = PositionAt(d, Depth[d]) - a_position;
	const float3 fromUp = a_position - PositionAt(u, Depth[u]);
	const float lengthRight = length(toRight), lengthLeft = length(fromLeft);
	const float lengthDown = length(toDown), lengthUp = length(fromUp);
	const float3 dx = (lengthLeft == 0.0 || (lengthRight > 0.0 && lengthRight < lengthLeft)) ? toRight : fromLeft;
	const float3 dy = (lengthUp == 0.0 || (lengthDown > 0.0 && lengthDown < lengthUp)) ? toDown : fromUp;
	const float3 n = cross(dx, dy);
	const float lengthN = length(n);
	if (!(lengthN > 1e-8))
		return a_toViewer;
	const float3 normal = n / lengthN;
	return dot(normal, a_toViewer) < 0.0 ? -normal : normal;
}

float3 CosineSampleHemisphere(float3 a_normal, float2 a_u)
{
	const float3 tangent = normalize(cross(a_normal, abs(a_normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
	const float3 bitangent = cross(a_normal, tangent);
	const float r = sqrt(a_u.x);
	const float phi = 6.2831853 * a_u.y;
	return normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + a_normal * sqrt(max(0.0, 1.0 - a_u.x)));
}

bool Occluded(float3 a_origin, float3 a_direction, float a_length)
{
	RayDesc ray;
	ray.Origin = a_origin;
	ray.Direction = a_direction;
	ray.TMin = 0.0;
	ray.TMax = a_length;
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
	query.Proceed();
	return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Radiance leaving a hit, in the space Screen-Space GI gathers kMAIN in (Color::RadianceToLinear).
float3 HitRadiance(float3 a_albedo, float3 a_normal, float a_sunVisibility)
{
	const float sunCosine = saturate(dot(a_normal, C.ToSun.xyz));
	if (C.LinearLighting) {
		const float3 albedo = pow(a_albedo, C.ColorGamma);
		const float3 sun = pow(max(C.SunColor.rgb, 0.0), C.LightGamma);
		const float3 ambient = pow(GetAmbient(a_normal), C.AmbientGamma) * C.AmbientMult;
		return albedo * (sun * sunCosine * a_sunVisibility + ambient);
	}
	return SkyrimGammaToLinear(a_albedo * (C.SunColor.rgb * sunCosine * a_sunVisibility + GetAmbient(a_normal)));
}

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
	ray.TMin = 0.0;
	ray.TMax = C.HitDistParams.w;
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
	query.Proceed();
	const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

	float3 radiance = 0.0;
	float hitDistance = C.HitDistParams.w;
	bool sunLit = false;
	if (hit) {
		hitDistance = query.CommittedRayT();
		const InstanceData instance = Instances[query.CommittedInstanceID()];
		float3 hitNormal = GeometricNormal(instance, query.CommittedPrimitiveIndex(), query.CommittedObjectToWorld3x4(), direction);
		if (all(hitNormal == 0.0))
			hitNormal = -direction;
		const float3 hitPosition = ray.Origin + direction * hitDistance;
		if (dot(hitNormal, C.ToSun.xyz) > 0.0 && any(C.SunColor.rgb > 0.0))
			sunLit = !Occluded(hitPosition + hitNormal * (C.NormalBias + hitDistance * C.DistanceBias), C.ToSun.xyz, kSunRayLength);
		radiance = HitRadiance(UnpackRGBA8(instance.Albedo).rgb, hitNormal, sunLit ? 1.0 : 0.0);
	}

	OutViewZ[pixel] = viewZ;
	OutNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, 1.0, 0.0);
	const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, C.HitDistParams.xyz, 1.0);
	OutRadianceHitDist[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist, true);

	Count(kGITraced, true);
	Count(kGIHits, hit);
	Count(kGISunLitHits, sunLit);
}
