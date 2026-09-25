// SkyrimRT M8: ray-traced shadows for the game's unshadowed point lights, traced from the depth pre-pass like the sun
// shadows (DXR 1.1 inline RayQuery, DXC cs_6_5). Lighting.hlsl still evaluates every light itself; this pass estimates
// the ratio sum(f V) / sum(f) over the RT-shadowed lights (Heitz et al. 2018): one light is picked in proportion to its
// unshadowed contribution f, and its visibility V is the raw estimate. The sun-shadow temporal and spatial passes
// denoise it, and Lighting.hlsl multiplies those lights' shadow term by the result (PS t46).

#include "MeshData.hlsli"
#include "PointLights.hlsli"
#include "SunShadowCommon.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> RasterDepth : register(t1);
StructuredBuffer<InstanceData> Instances : register(t5);  // root SRV, M7c alpha test
StructuredBuffer<PointLight> PointLights : register(t6);  // root SRV: Light Limit Fix's lights this frame
RWTexture2D<unorm float4> RawVisibility : register(u0);  // M9 phase 2: xyz the hero lights' visibility, w the rest's
RWTexture2D<float4> Geometry : register(u1);  // xyz: reconstructed normal (camera-relative world space)
RWByteAddressBuffer Counters : register(u4);

// M9: every light, the shadow-mapped ones included (Lighting.hlsl then skips the game's shadow map for them). Portal-strict
// lights are traced where Lighting.hlsl applies them: the pixel's room comes from the instance a primary ray finds
// (PixelRoom), tested as LightLimitFix::IsLightIgnored does.
static const uint kSkippedLights = kLightFlagDisabled;
static const uint kPrimaryMask = 0x0F;  // static | terrain | actor | alpha-tested: what the depth pre-pass draws

// Light Limit Fix room of the instance drawn at the pixel (Lighting.hlsl's RoomIndex for that draw), or -1. The primary
// ray matches the raster to ~0.1% of pixels (M4 depth metric); a miss or a room-less instance gives every light.
int PixelRoom(float3 a_nearPoint, float3 a_position, float a_distance)
{
	RayDesc ray;
	ray.Origin = a_nearPoint;
	ray.Direction = (a_position - a_nearPoint) / max(a_distance, 1e-4);
	ray.TMin = 0.0;
	ray.TMax = a_distance * 1.02 + 2.0;  // just past the raster surface: the depth metric's 1% tolerance, doubled
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, kPrimaryMask, ray);
	PROCEED_ALPHA_TESTED(query);
	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		return -1;
	return InstanceRoom(Instances[query.CommittedInstanceID()]);
}

void Count(uint a_slot, bool a_condition)
{
	const uint n = WaveActiveCountBits(a_condition);
	if (WaveIsFirstLane() && n > 0)
		Counters.InterlockedAdd(a_slot * 4, n);
}

// Same depth-derived normal as the sun-shadow trace: per axis the neighbour nearer to the centre point.
float3 ReconstructNormal(int2 a_pixel, float3 a_position, float3 a_toViewer)
{
	const int2 maxPixel = int2(C.RenderSize) - 1;
	const int2 l = clamp(a_pixel - int2(1, 0), 0, maxPixel);
	const int2 r = clamp(a_pixel + int2(1, 0), 0, maxPixel);
	const int2 u = clamp(a_pixel - int2(0, 1), 0, maxPixel);
	const int2 d = clamp(a_pixel + int2(0, 1), 0, maxPixel);
	const float3 toRight = PositionAt(r, RasterDepth[r]) - a_position;
	const float3 fromLeft = a_position - PositionAt(l, RasterDepth[l]);
	const float3 toDown = PositionAt(d, RasterDepth[d]) - a_position;
	const float3 fromUp = a_position - PositionAt(u, RasterDepth[u]);
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

// White noise per pixel and frame (the GI bounce's Random1).
float Random1(uint2 a_pixel, uint a_frame)
{
	return float(Hash(Hash(a_pixel.x | (a_pixel.y << 16)) ^ (a_frame * 0x9E3779B9u)) >> 8) / 16777216.0;
}

// One visibility ray from a_origin towards a light (a_toLight: origin to its centre), stopping a_clearance short of it.
// M9: the light is a small disc facing the surface, not a point: a random point on it per pixel and frame, so the
// denoised ratio has soft shadows that widen with the distance from the blocker (as the sun's cone).
bool LightOccluded(float3 a_origin, float3 a_toLight, float a_lightRadius, float a_clearance, uint2 a_pixel, out float a_occluderToLight)
{
	a_occluderToLight = -1.0;
	float3 toLight = a_toLight;
	const float sourceRadius = C.PointLightSourceFraction * a_lightRadius;
	if (sourceRadius > 0.0) {
		const float3 axis = normalize(toLight);
		const float3 tangent = normalize(cross(axis, abs(axis.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
		const float3 bitangent = cross(axis, tangent);
		const float2 disk = ConcentricDisk(Random2(a_pixel, C.FrameIndex)) * sourceRadius;
		toLight += tangent * disk.x + bitangent * disk.y;
	}
	const float lightDistance = length(toLight);
	const float rayLength = lightDistance - a_clearance;
	if (!(rayLength > 0.0))
		return false;
	RayDesc ray;
	ray.Origin = a_origin;
	ray.Direction = toLight / lightDistance;
	ray.TMin = 0.0;
	ray.TMax = rayLength;
	// No cull flags (open-backed meshes); alpha-tested casters let light through their holes (M7c).
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
	PROCEED_ALPHA_TESTED(query);
	if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		return false;
	a_occluderToLight = lightDistance - query.CommittedRayT();
	return true;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	const float depth = RasterDepth[pixel];
	const bool sky = depth >= 1.0;

	bool sampled = false;
	bool occluded = false;
	float4 visibility = 1.0;  // M9 phase 2: xyz the hero lights, w the rest
	bool heroReached[3] = { false, false, false };
	float occluderToLight = -1.0;
	int room = -1;
	bool anyInRange = false, anyFacing = false, anyInRoom = false;
	uint candidates = 0;  // lights SamplePointLight picks from: one visibility ray stands in for all of them
	float3 normal = float3(0.0, 0.0, 0.0);
	if (!sky) {
		const float3 position = PositionAt(pixel, depth);
		const float3 nearPoint = NearPointAt(pixel);
		const float distance = length(position - nearPoint);
		const float3 toViewer = (nearPoint - position) / max(distance, 1e-4);
		normal = ReconstructNormal(pixel, position, toViewer);

		room = C.RoomTest ? PixelRoom(nearPoint, position, distance) : -1;
		// Diagnostics (a few lights, cheap): which of SamplePointLight's filters leaves a pixel without a light.
		[loop] for (uint i = 0; i < C.PointLightCount; i++)
		{
			const PointLight candidate = PointLights[i];
			if (candidate.Flags & kSkippedLights)
				continue;
			const float3 toCandidate = candidate.Position - position;
			if (dot(toCandidate, toCandidate) >= candidate.Radius * candidate.Radius)
				continue;
			anyInRange = true;
			if (dot(normal, toCandidate) <= 0.0)
				continue;
			anyFacing = true;
			if (PointLightAppliesInRoom(candidate, room)) {
				anyInRoom = true;
				candidates++;
			}
		}
		const float3 origin = position + normal * (C.NormalBias + distance * C.DistanceBias);
		// M9 phase 2: the hero lights (the frame's brightest, chosen on the CPU) each get their own visibility ray and mask
		// channel wherever they reach the pixel, so Lighting.hlsl shadows each of them separately.
		[unroll] for (uint k = 0; k < 3; k++)
		{
			const uint heroIndex = C.HeroLights[k];
			if (heroIndex >= C.PointLightCount)
				continue;
			const PointLight hero = PointLights[heroIndex];
			if ((hero.Flags & kSkippedLights) || !PointLightAppliesInRoom(hero, room))
				continue;
			const float3 toHero = hero.Position - position;
			if (dot(toHero, toHero) >= hero.Radius * hero.Radius || dot(normal, toHero) <= 0.0)
				continue;
			heroReached[k] = true;
			float heroOccluderToLight;
			if (LightOccluded(origin, hero.Position - origin, hero.Radius, PointLightClearance(hero.Radius), dispatchID.xy, heroOccluderToLight))
				visibility[k] = 0.0;
		}
		// The rest: one light picked in proportion to its unshadowed contribution, as before phase 2.
		const PointLightSample light = SamplePointLight(PointLights, C.PointLightCount, kSkippedLights, C.InverseSquare != 0, position, normal,
			Random1(dispatchID.xy, C.FrameIndex), room, C.HeroLights);
		if (light.Valid) {
			sampled = true;
			occluded = LightOccluded(origin, light.ToLight + position - origin, light.LightRadius, light.Clearance, dispatchID.xy, occluderToLight);
		}
	}

	// A channel whose light doesn't reach the pixel stays 1 (nothing to shadow).
	visibility.w = occluded ? 0.0 : 1.0;
	RawVisibility[pixel] = visibility;
	Geometry[pixel] = float4(normal, 0.0);

	Count(kPointTraced, !sky);
	Count(kPointSampled, sampled);
	Count(kPointOccluded, occluded);
	Count(kPointOccluderNear32, occluded && occluderToLight < 32.0);
	Count(kPointOccluderNear64, occluded && occluderToLight >= 32.0 && occluderToLight < 64.0);
	Count(kPointOccluderNear128, occluded && occluderToLight >= 64.0 && occluderToLight < 128.0);
	Count(kPointRoomKnown, room >= 0);
	Count(kPointAnyInRange, anyInRange);
	Count(kPointAnyFacing, anyFacing);
	Count(kPointAnyInRoom, anyInRoom);
	Count(kPointCandidates1, candidates == 1);
	Count(kPointCandidates2to3, candidates >= 2 && candidates <= 3);
	Count(kPointCandidates4to7, candidates >= 4 && candidates <= 7);
	Count(kPointCandidates8Plus, candidates >= 8);
	[unroll] for (uint h = 0; h < 3; h++)
	{
		Count(kPointHeroReached0 + h, heroReached[h]);
		Count(kPointHeroOccluded0 + h, heroReached[h] && visibility[h] < 0.5);
	}
	// Where one light reaching the pixel is blocked and another visible, phase 1's single ratio darkened both; the hero
	// channels keep the visible one (the rest's one sample stands in for all the other lights).
	bool anyBlocked = sampled && occluded, anyVisible = sampled && !occluded;
	[unroll] for (uint m = 0; m < 3; m++)
	{
		anyBlocked = anyBlocked || (heroReached[m] && visibility[m] < 0.5);
		anyVisible = anyVisible || (heroReached[m] && visibility[m] >= 0.5);
	}
	Count(kPointMixedVisibility, anyBlocked && anyVisible);
	const uint candidateSum = WaveActiveSum(candidates);
	const uint candidateMax = WaveActiveMax(candidates);
	if (WaveIsFirstLane() && candidateSum > 0) {
		Counters.InterlockedAdd(kPointCandidateSum * 4, candidateSum);
		uint previous;
		Counters.InterlockedMax(kPointCandidateMax * 4, candidateMax, previous);
	}
}
