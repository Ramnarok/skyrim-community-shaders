// SkyrimRT M6: diffuse GI, one cosine-weighted ray per pixel from the finished G-buffer (DXR 1.1 inline RayQuery); M8
// multi-bounce continues the path from each hit up to C.Bounces vertices, scaling each vertex's ambient by the light its
// continuation brought in. The radiance leaving each hit follows the rule CS's deferred lighting uses for the surfaces Screen-Space
// GI gathers: albedo x (sun x N.L x sun visibility + point lights + the game's directional ambient), in Skyrim gamma, then
// Color::RadianceToLinear. The albedo is the hit's texture x vertex colour from the M8 albedo atlas, or the texture's
// average (M6) until its tile is filled. Output is REBLUR's noisy input. Misses carry no radiance (the composite keeps
// the game's ambient, scaled by the ambient occlusion REBLUR derives from the hit distance), except with M8 sky light
// (exteriors): a miss that reaches the sky carries the sky's radiance, and the composite scales the game's ambient by
// the traced light over the open-sky light.

#define SKYRIMRT_ALBEDO_ATLAS
#include "GICommon.hlsli"
#include "MeshData.hlsli"
#include "PointLights.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> GBufferNormal : register(t2);  // CS's NORMALROUGHNESS: xy octahedral view-space normal, z glossiness
Texture2D<float2> GameMotionVectors : register(t3);  // kMOTION_VECTOR: prevUV - currUV, NRD's convention already
StructuredBuffer<InstanceData> Instances : register(t5);  // root SRV
StructuredBuffer<PointLight> PointLights : register(t6);  // root SRV: Light Limit Fix's lights this frame
RWTexture2D<float> OutViewZ : register(u0);
RWTexture2D<float4> OutNormalRoughness : register(u1);
RWTexture2D<float2> OutMotionVectors : register(u2);
RWTexture2D<float4> OutRadianceHitDist : register(u3);
RWByteAddressBuffer Counters : register(u4);

static const float kSunRayLength = 50000.0;  // as the sun-shadow trace (SunShadows::kMaxRayDistance)
static const float kSelfHitMin = 2.0;         // game units
static const float kSelfHitRelative = 0.01;   // of view distance: Raytracer::kMismatchThreshold
static const uint kMaxBounces = 3;            // M8 multi-bounce: path vertices (GIParams::bounces is clamped to it)
static const float kMaxAmbientScale = 4.0;    // as the composite's sky-light ratio

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

// Distance along the ray to an occluder (any, not the nearest), or -1 when the segment is clear.
float OccluderDistance(float3 a_origin, float3 a_direction, float a_length)
{
	RayDesc ray;
	ray.Origin = a_origin;
	ray.Direction = a_direction;
	ray.TMin = 0.0;
	ray.TMax = a_length;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
	PROCEED_ALPHA_TESTED(query);
	return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : -1.0;
}

bool Occluded(float3 a_origin, float3 a_direction, float a_length)
{
	return OccluderDistance(a_origin, a_direction, a_length) >= 0.0;
}

// Point-light irradiance at a hit (N.L x colour x attenuation, as Lighting.hlsl sums it), estimated with one light
// (SamplePointLight) and one visibility ray to it.
float3 SamplePointLights(float3 a_position, float3 a_normal, float3 a_origin, float a_u, int a_room, out bool a_sampled, out bool a_occluded, out float a_occluderToLight)
{
	a_sampled = false;
	a_occluded = false;
	a_occluderToLight = -1.0;
	const PointLightSample light = SamplePointLight(PointLights, C.PointLightCount, 0u, C.InverseSquare != 0, a_position, a_normal, a_u, a_room);
	if (!light.Valid)
		return 0.0;

	a_sampled = true;
	if (C.PointLightShadows) {
		const float3 toLight = light.ToLight + a_position - a_origin;
		const float distance = length(toLight);
		const float rayLength = distance - light.Clearance;
		const float occluder = rayLength > 0.0 ? OccluderDistance(a_origin, toLight / distance, rayLength) : -1.0;
		if (occluder >= 0.0) {
			a_occluded = true;
			a_occluderToLight = distance - occluder;
			return 0.0;
		}
	}
	return light.Irradiance;
}

// One path vertex (a GI hit), split the way Lighting.hlsl builds the light leaving a surface: albedo x (direct + ambient)
// in its colour space, converted at the end (Color::RadianceToLinear). With more than one bounce the ambient term is
// scaled by the light the path traced onward (PathVertexRadiance).
struct PathVertex
{
	float3 albedo;   // colour space of the sum: Skyrim gamma, or Linear Lighting's colour gamma
	float3 direct;   // sun x N.L x visibility + point lights
	float3 ambient;  // the game's directional ambient at the normal
	float3 openSky;  // linear light an unoccluded hemisphere returns at the normal (SkyRadiance integrated)
	float3 normal;   // geometric, facing the incoming ray
	float3 origin;   // hit position offset along the normal: where continuation and visibility rays start
};

// Radiance leaving a vertex, in the space Screen-Space GI gathers kMAIN in (Color::RadianceToLinear). a_ambientScale is 1
// for the path's last vertex (the game's ambient stands in for every further bounce), else the traced incoming light over
// a_vertex.openSky: the composite's sky-light ratio, applied at the hit.
float3 PathVertexRadiance(PathVertex a_vertex, float3 a_ambientScale)
{
	const float3 sum = a_vertex.direct + a_vertex.ambient * a_ambientScale;
	return C.LinearLighting ? a_vertex.albedo * sum : SkyrimGammaToLinear(a_vertex.albedo * sum);
}

// M8 sky light: radiance arriving from the sky in a_direction, such that its cosine-weighted mean over an unoccluded
// hemisphere is the game's directional ambient at that normal. The ambient is L0 + L1.n as a function of the normal;
// cosine convolution scales L1 by 2/3, so the radiance carries L0 + 1.5 L1. The conversion matches the composite's for
// its ambient term (DeferredCompositeCS: Color::Ambient x albedo, to linear, against il x IrradianceToLinear(albedo /
// PBRLightingScale)): outside Linear Lighting that is SkyrimGammaToLinear with the 0.65 PBRLightingScale folded in.
float3 SkyRadiance(float3 a_direction)
{
	float4 basis = ShEvaluate(a_direction);
	basis.yzw *= 1.5;
	const float3 ambient = max(0.0, float3(dot(C.AmbientSHR, basis), dot(C.AmbientSHG, basis), dot(C.AmbientSHB, basis)));
	if (C.LinearLighting)
		return pow(ambient, C.AmbientGamma) * C.AmbientMult;
	static const float kPBRLightingScale = 0.65;  // Color::PBRLightingScale without Linear Lighting
	return SkyrimGammaToLinear(ambient * kPBRLightingScale);
}

// M8 multi-bounce: SkyRadiance integrated over an unoccluded hemisphere at a_normal: the game's ambient there, converted
// like SkyRadiance (the composite's openSky).
float3 OpenSkyRadiance(float3 a_normal)
{
	const float3 ambient = GetAmbient(a_normal);
	if (C.LinearLighting)
		return pow(ambient, C.AmbientGamma) * C.AmbientMult;
	return SkyrimGammaToLinear(ambient * 0.65);
}

// M8 multi-bounce: what a continuation ray that found nothing within the GI ray length sees. Outdoors with sky light,
// the sky if the ray gets clear to the sun's range (else distant blocked terrain: dark). Otherwise the neutral
// environment, the game's ambient as sky radiance.
float3 MissRadiance(float3 a_end, float3 a_direction)
{
	if (C.SkyLight)
		return Occluded(a_end, a_direction, kSunRayLength) ? 0.0 : SkyRadiance(a_direction);
	return SkyRadiance(a_direction);
}

// White noise for the deeper bounces, independent per pixel, frame and bounce.
float2 RandomBounce(uint2 a_pixel, uint a_frame, uint a_bounce)
{
	const uint h = Hash(Hash(a_pixel.x | (a_pixel.y << 16)) ^ Hash(a_frame * 0x9E3779B9u + a_bounce * 0x85EBCA6Bu));
	return float2(h & 0xFFFFu, h >> 16) / 65536.0;
}

float RandomLight(uint2 a_pixel, uint a_frame, uint a_bounce)
{
	return a_bounce == 0 ? Random1(a_pixel, a_frame) : float(Hash(Hash(a_pixel.x | (a_pixel.y << 16)) ^ Hash(a_frame * 0xC2B2AE35u + a_bounce)) >> 8) / 16777216.0;
}

// Shades a committed hit into a path vertex: albedo (texture x vertex colour), sun with a visibility ray, one sampled
// point light with its visibility ray, and the game's ambient. The out parameters are the per-hit diagnostics.
PathVertex ShadeHit(InstanceData a_instance, uint a_primitive, float2 a_barycentrics, float3x4 a_objectToWorld, float3 a_rayOrigin,
	float3 a_direction, float a_t, uint2 a_pixel, uint a_bounce, out bool a_sunLit, out bool a_lightSampled, out bool a_lightOccluded, out float a_occluderToLight)
{
	PathVertex vertex;
	float3 normal = GeometricNormal(a_instance, a_primitive, a_objectToWorld, a_direction);
	if (all(normal == 0.0))
		normal = -a_direction;
	const float3 position = a_rayOrigin + a_direction * a_t;
	vertex.normal = normal;
	vertex.origin = position + normal * (C.NormalBias + a_t * C.DistanceBias);

	// Lighting.hlsl shadows the directional light only through the sun's shadow mask (exteriors); indoors it is
	// unshadowed, and a visibility ray would always hit the ceiling.
	a_sunLit = false;
	if (dot(normal, C.ToSun.xyz) > 0.0 && any(C.SunColor.rgb > 0.0))
		a_sunLit = C.Interior || !Occluded(vertex.origin, C.ToSun.xyz, kSunRayLength);
	const float3 pointLights = SamplePointLights(position, normal, vertex.origin, RandomLight(a_pixel, C.FrameIndex, a_bounce), int(a_instance.Room) - 1,
		a_lightSampled, a_lightOccluded, a_occluderToLight);
	const float sun = saturate(dot(normal, C.ToSun.xyz)) * (a_sunLit ? 1.0 : 0.0);
	const float3 albedo = HitAlbedo(a_instance, a_primitive, a_barycentrics);
	if (C.LinearLighting) {
		// Color::DirectionalLight's pi cancels Color::VanillaNormalization's 1/pi in the diffuse term.
		vertex.albedo = pow(albedo, C.ColorGamma);
		vertex.direct = pow(max(C.SunColor.rgb, 0.0), C.LightGamma) * C.DirectionalLightMult * sun + pointLights;
		vertex.ambient = pow(GetAmbient(normal), C.AmbientGamma) * C.AmbientMult;
	} else {
		vertex.albedo = albedo;
		vertex.direct = C.SunColor.rgb * sun + pointLights;
		vertex.ambient = GetAmbient(normal);
	}
	vertex.openSky = OpenSkyRadiance(normal);
	return vertex;
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

		// M8 multi-bounce: the path continues from each hit (a cosine-weighted ray around its geometric normal) up to
		// C.Bounces vertices, then folds back from the last one.
		PathVertex vertices[kMaxBounces];
		vertices[0] = ShadeHit(instance, query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(),
			ray.Origin, direction, hitDistance, dispatchID.xy, 0, sunLit, lightSampled, lightOccluded, occluderToLight);
		uint count = 1;
		float3 incoming = 0.0;    // light arriving at the last vertex from a continuation that missed
		bool continued = false;  // the last vertex traced a continuation (it missed)
		const uint bounces = clamp(C.Bounces, 1u, kMaxBounces);
		[loop] while (count < bounces)
		{
			RayDesc next;
			next.Origin = vertices[count - 1].origin;
			next.Direction = CosineSampleHemisphere(vertices[count - 1].normal, RandomBounce(dispatchID.xy, C.FrameIndex, count));
			next.TMin = kSelfHitMin;
			next.TMax = C.HitDistParams.w;
			RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> nextQuery;
			nextQuery.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, next);
			PROCEED_ALPHA_TESTED(nextQuery);
			if (nextQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
				incoming = MissRadiance(next.Origin + next.Direction * next.TMax, next.Direction);
				continued = true;
				break;
			}
			bool deeperSunLit, deeperSampled, deeperOccluded;
			float deeperOccluderToLight;
			vertices[count] = ShadeHit(Instances[nextQuery.CommittedInstanceID()], nextQuery.CommittedPrimitiveIndex(), nextQuery.CommittedTriangleBarycentrics(),
				nextQuery.CommittedObjectToWorld3x4(), next.Origin, next.Direction, nextQuery.CommittedRayT(), dispatchID.xy, count,
				deeperSunLit, deeperSampled, deeperOccluded, deeperOccluderToLight);
			count++;
			deeperHits++;
		}

		// Fold back: each vertex's ambient is scaled by what its continuation brought in over the open sky's light; the
		// last vertex keeps the game's ambient unless its continuation missed.
		[loop] for (int i = int(count) - 1; i >= 0; i--)
		{
			const float3 scale = continued ? clamp(incoming / max(vertices[i].openSky, 1e-4), 0.0, kMaxAmbientScale) : 1.0;
			incoming = PathVertexRadiance(vertices[i], scale);
			continued = true;
		}
		radiance = incoming;
	}
	// M8 sky light: a miss within the GI ray length isn't sky yet. It continues, as an any-hit visibility ray, to the
	// sun's range: past it there's no geometry in the loaded cells (distant LOD isn't in the TLAS).
	bool skyVisible = false;
	if (!hit && C.SkyLight) {
		skyVisible = !Occluded(ray.Origin + direction * ray.TMax, direction, kSunRayLength);
		if (skyVisible)
			radiance = SkyRadiance(direction);
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
	const uint deeperSum = WaveActiveSum(deeperHits);
	if (WaveIsFirstLane() && deeperSum > 0)
		Counters.InterlockedAdd(kGIDeeperHits * 4, deeperSum);
}
