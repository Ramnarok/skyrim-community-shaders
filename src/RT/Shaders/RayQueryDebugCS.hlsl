// SkyrimRT M4: inline-RayQuery debug trace (DXR 1.1, compiled by DXC as cs_6_5 at build time).
// Traces one camera ray per render pixel against the camera-relative TLAS, writes depth / instance /
// normal / diff debug views, and counts the depth-mismatch metric (ROADMAP M4) into Counters.

struct TraceConstants
{
	row_major float4x4 ViewProjInverse;  // FrameBuffer::CameraViewProjInverse (jittered, camera-relative)
	uint2 RenderSize;                    // dynamic-resolution render size
	uint InstanceCount;
	uint ExclusionCount;
	float4 LoadedMin;  // xy: loaded cells min (camera-relative), z: 1 if bounded (exterior)
	float4 LoadedMax;  // xy: loaded cells max (camera-relative)
	float MismatchThreshold;  // relative distance difference counted as a mismatch (0.01)
	float ExclusionMargin;    // relative inflation of exclusion bounds for the point test (min 4 units)
	float MaxDistance;        // depth view normalisation
	float ClutterHeight;      // grass / ground clutter: max height above traced terrain (game units)
};

#include "MeshData.hlsli"

struct ExclusionAabb  // D3D12_RAYTRACING_AABB layout, also the procedural BLAS input
{
	float3 Min;
	float3 Max;
};

ConstantBuffer<TraceConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> RasterDepth : register(t1);
StructuredBuffer<InstanceData> Instances : register(t2);
StructuredBuffer<ExclusionAabb> Exclusions : register(t3);

RWTexture2D<unorm float4> DepthView : register(u0);
RWTexture2D<unorm float4> InstanceView : register(u1);
RWTexture2D<unorm float4> NormalView : register(u2);
RWTexture2D<unorm float4> DiffView : register(u3);
RWByteAddressBuffer Counters : register(u4);

// Counter slots, mirrored in Raytracer.h
static const uint kRenderPixels = 0;
static const uint kSky = 1;
static const uint kOutsideLoaded = 2;
static const uint kExcluded = 3;
static const uint kCounted = 4;
static const uint kMatched = 5;
static const uint kTracedNearer = 6;
static const uint kTracedFarther = 7;
static const uint kTracedMiss = 8;
static const uint kExcludedAlpha = 9;
static const uint kExcludedClutter = 10;
static const uint kAlphaTestedCounted = 11;  // M7c: counted pixels whose traced hit passed the alpha test
static const uint kAlphaTestedMatched = 12;
static const uint kExcludedWind = 13;  // M7c: mismatches on wind-animated foliage
// M8 distant LOD: the same depth test for non-sky pixels outside the loaded cells (not in the main metric: tree LOD
// billboards and anything else the TLAS lacks out there count as mismatches).
static const uint kOutsideCounted = 14;
static const uint kOutsideMatched = 15;
static const uint kOutsideNearer = 16;
static const uint kOutsideFarther = 17;
static const uint kOutsideMiss = 18;
static const uint kOutsideHitLOD = 19;  // ... whose traced hit is a distant-LOD instance

void Count(uint a_slot, bool a_condition)
{
	const uint n = WaveActiveCountBits(a_condition);
	if (WaveIsFirstLane() && n > 0)
		Counters.InterlockedAdd(a_slot * 4, n);
}

float3 Unproject(float2 a_ndc, float a_depth)
{
	const float4 p = mul(C.ViewProjInverse, float4(a_ndc, a_depth, 1.0));
	return p.xyz / p.w;
}

float3 HashColor(uint a_value)
{
	uint x = a_value + 1;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return float3(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u) / 255.0;
}

// True if terrain lies within a_height directly below a_point (Skyrim is Z-up; camera-relative space keeps axes).
bool AboveTerrainWithin(float3 a_point, float a_height)
{
	RayDesc down;
	down.Origin = a_point;
	down.Direction = float3(0.0, 0.0, -1.0);
	down.TMin = 0.0;
	down.TMax = a_height;
	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, kMaskTerrain, down);
	query.Proceed();
	return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// True if wind-animated geometry (TREE_ANIM) lies within a_margin of a_point along a_direction, alpha holes included:
// the game's vertex shader sways it along its normals, so the raster may show it where the rest-pose TLAS doesn't.
bool NearWindAnimated(float3 a_point, float3 a_direction, float a_margin)
{
	RayDesc segment;
	segment.Origin = a_point - a_direction * a_margin;
	segment.Direction = a_direction;
	segment.TMin = 0.0;
	segment.TMax = 2.0 * a_margin;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, kMaskStatic | kMaskAlphaTested | kMaskActor, segment);
	while (query.Proceed()) {
		if ((Instances[query.CandidateInstanceID()].Flags & kInstanceWindAnimated) != 0) {
			query.Abort();
			return true;
		}
	}
	return false;
}

// True if a_point lies inside (a_margin-inflated) bounds of geometry the TLAS doesn't contain.
// A short ray segment through the point collects the candidate boxes; containment is tested exactly.
bool PointInExclusion(float3 a_point, float3 a_direction, float a_margin)
{
	RayDesc segment;
	segment.Origin = a_point - a_direction * a_margin;
	segment.Direction = a_direction;
	segment.TMin = 0.0;
	segment.TMax = 2.0 * a_margin;
	RayQuery<RAY_FLAG_SKIP_TRIANGLES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, kMaskExclusion, segment);
	while (query.Proceed()) {
		if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
			continue;
		const ExclusionAabb box = Exclusions[query.CandidatePrimitiveIndex()];
		if (all(a_point >= box.Min - a_margin) && all(a_point <= box.Max + a_margin)) {
			query.Abort();
			return true;
		}
	}
	return false;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;

	// Same pixel -> NDC mapping as CS's DeferredCompositeCS (render-region pixel centres).
	const float2 uv = (dispatchID.xy + 0.5) / float2(C.RenderSize);
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	// Standard depth: near plane = 0, far plane = 1.
	const float3 origin = Unproject(ndc, 0.0);
	const float3 farPoint = Unproject(ndc, 1.0);
	const float3 direction = normalize(farPoint - origin);
	const float maxT = length(farPoint - origin);

	const float rasterDepth = RasterDepth[dispatchID.xy];
	const bool sky = rasterDepth >= 1.0;
	const float3 rasterPosition = Unproject(ndc, rasterDepth);
	const float rasterT = length(rasterPosition - origin);
	const bool outside = !sky && C.LoadedMin.z > 0.5 && (any(rasterPosition.xy < C.LoadedMin.xy) || any(rasterPosition.xy > C.LoadedMax.xy));

	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = maxT;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, kMaskStatic | kMaskTerrain | kMaskAlphaTested | kMaskActor | kMaskDistantLOD, ray);
	PROCEED_ALPHA_TESTED(query);
	const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
	const float t = hit ? query.CommittedRayT() : maxT;
	InstanceData hitData = (InstanceData)0;
	float3 hitNormal = float3(0.0, 0.0, 0.0);
	if (hit) {
		hitData = Instances[query.CommittedInstanceID()];
		hitNormal = GeometricNormal(hitData, query.CommittedPrimitiveIndex(), query.CommittedObjectToWorld3x4(), direction);
	}

	// Which pixels can't be verified:
	//  - a match is always counted: the static surface is visibly there and correct;
	//  - a mismatch whose traced hit is an alpha-tested mesh without an alpha-atlas tile (traced as a solid card,
	//    M7c alpha test off or not loaded yet) -> excluded (alpha); alpha-tested hits with a tile are counted;
	//  - a mismatch where the raster surface is nearer and lies inside the bounds of geometry the TLAS doesn't
	//    contain (actors, dynamic, LOD): the game is showing that object -> excluded (occluder);
	//  - a mismatch where the raster surface is nearer and stands within ClutterHeight above the terrain directly
	//    below it: grass / ground clutter, whose bounds aren't usable -> excluded (clutter). Tested against the
	//    terrain below the raster point, not the traced hit, because blades also hang over rocks and roads;
	//  - a mismatch on wind-animated foliage (TREE_ANIM: trees, ferns): the traced hit is such a mesh, or the raster
	//    surface is nearer and such a mesh lies within kWindMargin of it. The game's vertex shader sways these along
	//    their normals every frame; the TLAS holds the rest pose -> excluded (wind), counted separately.
	// Everything else (traced nearer, misses, anything else farther) is a real mismatch.
	const float relative = abs(t - rasterT) / max(rasterT, 1e-3);
	const bool candidate = !sky && !outside;
	const bool agrees = candidate && hit && relative <= C.MismatchThreshold;
	bool excludedAlpha = false;
	bool excludedWind = false;
	bool excluded = false;
	bool excludedClutter = false;
	if (candidate && !agrees) {
		excludedAlpha = hit && (hitData.Flags & kInstanceAlphaTested) != 0 && (hitData.Alpha & 0xFFFu) == 0;
		const bool rasterNearer = !hit || t > rasterT;
		if (!excludedAlpha) {
			const float windMargin = max(8.0, rasterT * 0.02);
			excludedWind = (hit && (hitData.Flags & kInstanceWindAnimated) != 0) ||
			               (rasterNearer && NearWindAnimated(rasterPosition, direction, windMargin));
		}
		if (!excludedAlpha && !excludedWind && rasterNearer && C.ExclusionCount > 0)
			excluded = PointInExclusion(rasterPosition, direction, max(4.0, rasterT * C.ExclusionMargin));
		if (!excludedAlpha && !excludedWind && !excluded && rasterNearer)
			excludedClutter = AboveTerrainWithin(rasterPosition, C.ClutterHeight);
	}

	const bool counted = candidate && !excluded && !excludedAlpha && !excludedWind && !excludedClutter;
	const bool matched = agrees;
	const bool nearer = counted && hit && !matched && t < rasterT;
	const bool farther = counted && hit && !matched && t >= rasterT;
	const bool miss = counted && !hit;
	const bool alphaTestedHit = counted && hit && (hitData.Flags & kInstanceLODClip) == 0 && (hitData.Alpha & 0xFFFu) != 0;
	const bool outsideMatched = outside && hit && relative <= C.MismatchThreshold;
	const bool outsideNearer = outside && hit && !outsideMatched && t < rasterT;
	const bool outsideFarther = outside && hit && !outsideMatched && t >= rasterT;

	Count(kRenderPixels, true);
	Count(kSky, sky);
	Count(kOutsideLoaded, outside);
	Count(kExcluded, excluded);
	Count(kCounted, counted);
	Count(kMatched, matched);
	Count(kTracedNearer, nearer);
	Count(kTracedFarther, farther);
	Count(kTracedMiss, miss);
	Count(kExcludedAlpha, excludedAlpha);
	Count(kExcludedClutter, excludedClutter);
	Count(kAlphaTestedCounted, alphaTestedHit);
	Count(kAlphaTestedMatched, alphaTestedHit && matched);
	Count(kExcludedWind, excludedWind);
	Count(kOutsideCounted, outside);
	Count(kOutsideMatched, outsideMatched);
	Count(kOutsideNearer, outsideNearer);
	Count(kOutsideFarther, outsideFarther);
	Count(kOutsideMiss, outside && !hit);
	Count(kOutsideHitLOD, outside && hit && (hitData.Flags & kInstanceDistantLOD) != 0);

	// Depth view: near = bright, log scaled; misses dark blue.
	const float depthValue = 1.0 - saturate(log2(1.0 + t) / log2(1.0 + C.MaxDistance));
	DepthView[dispatchID.xy] = hit ? float4(depthValue.xxx, 1.0) : float4(0.0, 0.0, 0.25, 1.0);

	InstanceView[dispatchID.xy] = hit ? float4(HashColor(query.CommittedInstanceIndex()), 1.0) : float4(0.0, 0.0, 0.0, 1.0);

	NormalView[dispatchID.xy] = any(hitNormal != 0.0) ? float4(hitNormal * 0.5 + 0.5, 1.0) : float4(0.0, 0.0, 0.0, 1.0);

	// Diff view: black sky, dark grey outside the loaded cells, grey excluded occluder, pink excluded alpha, ochre wind,
	// green match, red traced nearer, blue traced farther, yellow traced miss.
	// M8: outside the loaded cells the same colours, darker (dark grey where nothing was hit).
	float3 diff = float3(0.0, 0.0, 0.0);
	if (outside)
		diff = outsideMatched ? float3(0.0, 0.45, 0.0) : outsideNearer ? float3(0.55, 0.0, 0.0) : outsideFarther ? float3(0.0, 0.15, 0.55) : float3(0.15, 0.15, 0.15);
	else if (excluded)
		diff = float3(0.45, 0.45, 0.45);
	else if (excludedAlpha)
		diff = float3(0.75, 0.5, 0.75);
	else if (excludedWind)
		diff = float3(0.7, 0.55, 0.2);
	else if (excludedClutter)
		diff = float3(0.4, 0.65, 0.65);
	else if (matched)
		diff = float3(0.0, 0.8, 0.0);
	else if (nearer)
		diff = float3(1.0, 0.0, 0.0);
	else if (farther)
		diff = float3(0.0, 0.3, 1.0);
	else if (miss)
		diff = float3(1.0, 0.9, 0.0);
	DiffView[dispatchID.xy] = float4(diff, 1.0);
}
