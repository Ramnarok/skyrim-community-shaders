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

struct InstanceData
{
	uint VertexPage;
	uint VertexOffset;
	uint IndexPage;
	uint IndexOffset;
	uint Stride;
	uint Flags;
	uint Pad0;
	uint Pad1;
};

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
ByteAddressBuffer MeshPages[64] : register(t0, space1);

RWTexture2D<unorm float4> DepthView : register(u0);
RWTexture2D<unorm float4> InstanceView : register(u1);
RWTexture2D<unorm float4> NormalView : register(u2);
RWTexture2D<unorm float4> DiffView : register(u3);
RWByteAddressBuffer Counters : register(u4);

static const uint kMaskStatic = 0x01;
static const uint kMaskTerrain = 0x02;
static const uint kMaskAlphaTested = 0x08;
static const uint kMaskExclusion = 0x10;

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

// InstanceData.Flags bits, mirrored in Raytracer.cpp
static const uint kInstanceTerrain = 1;
static const uint kInstanceAlphaTested = 2;

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

uint LoadIndex(ByteAddressBuffer a_buffer, uint a_byteOffset)
{
	const uint word = a_buffer.Load(a_byteOffset & ~3u);
	return (a_byteOffset & 2u) ? (word >> 16) : (word & 0xFFFFu);
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

// Geometric normal of the hit triangle from the cached mesh data (world space, facing the ray).
// Returns 0 if the instance's pages aren't bound. Also proves the index/vertex decoding the BLAS uses.
float3 GeometricNormal(InstanceData a_data, uint a_primitive, float3x4 a_objectToWorld, float3 a_direction)
{
	if (a_data.VertexPage >= 64 || a_data.IndexPage >= 64)
		return float3(0.0, 0.0, 0.0);
	const uint indexBase = a_data.IndexOffset + a_primitive * 6;
	const ByteAddressBuffer indices = MeshPages[NonUniformResourceIndex(a_data.IndexPage)];
	const uint i0 = LoadIndex(indices, indexBase);
	const uint i1 = LoadIndex(indices, indexBase + 2);
	const uint i2 = LoadIndex(indices, indexBase + 4);
	const ByteAddressBuffer vertices = MeshPages[NonUniformResourceIndex(a_data.VertexPage)];
	const float3 p0 = asfloat(vertices.Load3(a_data.VertexOffset + i0 * a_data.Stride));
	const float3 p1 = asfloat(vertices.Load3(a_data.VertexOffset + i1 * a_data.Stride));
	const float3 p2 = asfloat(vertices.Load3(a_data.VertexOffset + i2 * a_data.Stride));
	const float3 normal = normalize(mul((float3x3)a_objectToWorld, cross(p1 - p0, p2 - p0)));
	return dot(normal, a_direction) > 0.0 ? -normal : normal;
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

	RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(Scene, RAY_FLAG_NONE, kMaskStatic | kMaskTerrain | kMaskAlphaTested, ray);
	query.Proceed();
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
	//  - a mismatch whose traced hit is an alpha-tested mesh: its holes need textures (M7) -> excluded (alpha);
	//  - a mismatch where the raster surface is nearer and lies inside the bounds of geometry the TLAS doesn't
	//    contain (actors, dynamic, LOD): the game is showing that object -> excluded (occluder);
	//  - a mismatch where the raster surface is nearer and stands within ClutterHeight above the terrain directly
	//    below it: grass / ground clutter, whose bounds aren't usable -> excluded (clutter). Tested against the
	//    terrain below the raster point, not the traced hit, because blades also hang over rocks and roads.
	// Everything else (traced nearer, misses, anything else farther) is a real mismatch.
	const float relative = abs(t - rasterT) / max(rasterT, 1e-3);
	const bool candidate = !sky && !outside;
	const bool agrees = candidate && hit && relative <= C.MismatchThreshold;
	bool excludedAlpha = false;
	bool excluded = false;
	bool excludedClutter = false;
	if (candidate && !agrees) {
		excludedAlpha = hit && (hitData.Flags & kInstanceAlphaTested) != 0;
		const bool rasterNearer = !hit || t > rasterT;
		if (!excludedAlpha && rasterNearer && C.ExclusionCount > 0)
			excluded = PointInExclusion(rasterPosition, direction, max(4.0, rasterT * C.ExclusionMargin));
		if (!excludedAlpha && !excluded && rasterNearer)
			excludedClutter = AboveTerrainWithin(rasterPosition, C.ClutterHeight);
	}

	const bool counted = candidate && !excluded && !excludedAlpha && !excludedClutter;
	const bool matched = agrees;
	const bool nearer = counted && hit && !matched && t < rasterT;
	const bool farther = counted && hit && !matched && t >= rasterT;
	const bool miss = counted && !hit;

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

	// Depth view: near = bright, log scaled; misses dark blue.
	const float depthValue = 1.0 - saturate(log2(1.0 + t) / log2(1.0 + C.MaxDistance));
	DepthView[dispatchID.xy] = hit ? float4(depthValue.xxx, 1.0) : float4(0.0, 0.0, 0.25, 1.0);

	InstanceView[dispatchID.xy] = hit ? float4(HashColor(query.CommittedInstanceIndex()), 1.0) : float4(0.0, 0.0, 0.0, 1.0);

	NormalView[dispatchID.xy] = any(hitNormal != 0.0) ? float4(hitNormal * 0.5 + 0.5, 1.0) : float4(0.0, 0.0, 0.0, 1.0);

	// Diff view: black sky, dark grey outside the loaded cells, grey excluded occluder, pink excluded alpha,
	// green match, red traced nearer, blue traced farther, yellow traced miss.
	float3 diff = float3(0.0, 0.0, 0.0);
	if (outside)
		diff = float3(0.15, 0.15, 0.15);
	else if (excluded)
		diff = float3(0.45, 0.45, 0.45);
	else if (excludedAlpha)
		diff = float3(0.75, 0.5, 0.75);
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
