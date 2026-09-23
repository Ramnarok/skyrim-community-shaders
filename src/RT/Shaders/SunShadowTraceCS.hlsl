// SkyrimRT M5: one cone-jittered shadow ray per pixel towards the sun, traced from the depth pre-pass (DXR 1.1
// inline RayQuery, DXC cs_6_5). No G-buffer exists yet at this point of the frame, so the surface normal used for
// the ray-origin offset is reconstructed from depth.

#include "SunShadowCommon.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> RasterDepth : register(t1);
Texture2D<float> GameShadowMask : register(t2);  // copy of the game's kSHADOW_MASK (dump frames only)
RWTexture2D<unorm float> RawVisibility : register(u0);
RWTexture2D<float4> Geometry : register(u1);  // xyz: reconstructed normal (camera-relative world space)
RWByteAddressBuffer Counters : register(u4);

void Count(uint a_slot, bool a_condition)
{
	const uint n = WaveActiveCountBits(a_condition);
	if (WaveIsFirstLane() && n > 0)
		Counters.InterlockedAdd(a_slot * 4, n);
}

// Picks, per axis, the neighbour closer to the centre point, so the normal doesn't bend across depth edges.
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
	// A clamped neighbour at the render edge is the pixel itself (length 0): use the other side.
	const float3 dx = (lengthLeft == 0.0 || (lengthRight > 0.0 && lengthRight < lengthLeft)) ? toRight : fromLeft;
	const float3 dy = (lengthUp == 0.0 || (lengthDown > 0.0 && lengthDown < lengthUp)) ? toDown : fromUp;

	const float3 n = cross(dx, dy);
	const float lengthN = length(n);
	if (!(lengthN > 1e-8))
		return a_toViewer;
	const float3 normal = n / lengthN;
	return dot(normal, a_toViewer) < 0.0 ? -normal : normal;
}

// Uniform point on the unit disk (concentric mapping keeps the stratification of the input).
float2 ConcentricDisk(float2 a_u)
{
	const float2 o = a_u * 2.0 - 1.0;
	if (all(o == 0.0))
		return float2(0.0, 0.0);
	float r, theta;
	if (abs(o.x) > abs(o.y)) {
		r = o.x;
		theta = 0.78539816 * (o.y / o.x);
	} else {
		r = o.y;
		theta = 1.57079633 - 0.78539816 * (o.x / o.y);
	}
	return r * float2(cos(theta), sin(theta));
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= C.RenderSize))
		return;
	const int2 pixel = int2(dispatchID.xy);

	const float depth = RasterDepth[pixel];
	const bool sky = depth >= 1.0;  // standard depth: far plane = 1

	bool shadowed = false;
	float3 normal = float3(0.0, 0.0, 0.0);
	float distance = 0.0;
	if (!sky) {
		const float3 position = PositionAt(pixel, depth);
		const float3 nearPoint = NearPointAt(pixel);
		distance = length(position - nearPoint);
		const float3 toViewer = (nearPoint - position) / max(distance, 1e-4);
		normal = ReconstructNormal(pixel, position, toViewer);

		const float3 toSun = C.ToSun.xyz;
		const float3 tangent = normalize(cross(toSun, abs(toSun.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
		const float3 bitangent = cross(toSun, tangent);
		const float2 disk = ConcentricDisk(Random2(dispatchID.xy, C.FrameIndex)) * C.ToSun.w;

		RayDesc ray;
		ray.Origin = position + normal * (C.NormalBias + distance * C.DistanceBias);
		ray.Direction = normalize(toSun + tangent * disk.x + bitangent * disk.y);
		ray.TMin = 0.0;
		ray.TMax = C.MaxDistance;

		// Single-sided culling would let light through Skyrim's many open-backed meshes, so no cull flags.
		RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
		query.TraceRayInline(Scene, RAY_FLAG_NONE, C.CasterMask, ray);
		query.Proceed();
		shadowed = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
	}

	RawVisibility[pixel] = shadowed ? 0.0 : 1.0;
	Geometry[pixel] = float4(normal, 0.0);

	Count(kShadowTraced, !sky);
	Count(kShadowShadowed, shadowed);

	if (C.Flags & kFlagCompareShadowMap) {
		// The game's cascaded shadow term is only defined near the camera, so compare within CompareDistance.
		const bool compared = !sky && distance < C.CompareDistance;
		const bool mapShadowed = GameShadowMask[pixel] < 0.5;
		Count(kCompareCompared, compared);
		Count(kCompareBothLit, compared && !shadowed && !mapShadowed);
		Count(kCompareBothShadowed, compared && shadowed && mapShadowed);
		Count(kCompareRtOnly, compared && shadowed && !mapShadowed);
		Count(kCompareMapOnly, compared && !shadowed && mapShadowed);
	}
}
