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
RWTexture2D<unorm float> RawVisibility : register(u0);
RWTexture2D<float4> Geometry : register(u1);  // xyz: reconstructed normal (camera-relative world space)
RWByteAddressBuffer Counters : register(u4);

// The game shadow-maps Shadow lights. Portal-strict lights are traced where Lighting.hlsl applies them: the pixel's
// room comes from the instance a primary ray finds (PixelRoom), tested as LightLimitFix::IsLightIgnored does.
static const uint kSkippedLights = kLightFlagShadow | kLightFlagDisabled;
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
	return int(Instances[query.CommittedInstanceID()].Room) - 1;
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

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	const float depth = RasterDepth[pixel];
	const bool sky = depth >= 1.0;

	bool sampled = false;
	bool occluded = false;
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
		const PointLightSample light = SamplePointLight(PointLights, C.PointLightCount, kSkippedLights, C.InverseSquare != 0, position, normal,
			Random1(dispatchID.xy, C.FrameIndex), room);
		if (light.Valid) {
			sampled = true;
			const float3 origin = position + normal * (C.NormalBias + distance * C.DistanceBias);
			const float3 toLight = light.ToLight + position - origin;
			const float lightDistance = length(toLight);
			const float rayLength = lightDistance - light.Clearance;
			if (rayLength > 0.0) {
				RayDesc ray;
				ray.Origin = origin;
				ray.Direction = toLight / lightDistance;
				ray.TMin = 0.0;
				ray.TMax = rayLength;
				// No cull flags (open-backed meshes); alpha-tested casters let light through their holes (M7c).
				RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
				query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
				PROCEED_ALPHA_TESTED(query);
				if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
					occluded = true;
					occluderToLight = lightDistance - query.CommittedRayT();
				}
			}
		}
	}

	// No RT-shadowed light in range: nothing to shadow, and the ratio is 1.
	RawVisibility[pixel] = occluded ? 0.0 : 1.0;
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
	const uint candidateSum = WaveActiveSum(candidates);
	const uint candidateMax = WaveActiveMax(candidates);
	if (WaveIsFirstLane() && candidateSum > 0) {
		Counters.InterlockedAdd(kPointCandidateSum * 4, candidateSum);
		uint previous;
		Counters.InterlockedMax(kPointCandidateMax * 4, candidateMax, previous);
	}
}
