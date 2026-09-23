// SkyrimRT: per-instance data and mesh-cache access shared by the RayQuery passes.

struct InstanceData  // mirrors InstanceGpu in Raytracer.cpp
{
	uint VertexPage;
	uint VertexOffset;
	uint IndexPage;
	uint IndexOffset;
	uint Stride;
	uint Flags;   // kInstance*
	uint Albedo;  // RGBA8 average diffuse as the texture stores it (M6 material table)
	uint Alpha;   // M7c alpha test, 0 = opaque: bits 0-11 atlas tile + 1, 12-19 threshold, 20-27 UV byte offset
	// M7c: where the texture coordinates live. The vertex data itself for static meshes; the bind-pose source for
	// skinned ones (trees and actors), whose Vertex* fields point at float3 skinning output.
	uint UVPage;
	uint UVOffset;
	uint UVStride;
	uint Pad;
};

// InstanceData.Flags bits, mirrored in Raytracer.cpp
static const uint kInstanceTerrain = 1;
static const uint kInstanceAlphaTested = 2;
static const uint kInstanceActor = 8;
static const uint kInstanceWindAnimated = 16;  // M7c: kTreeAnim, swayed by the game's vertex shader (not in the TLAS)

// InstanceMask bits, mirrored in Raytracer.cpp
static const uint kMaskStatic = 0x01;
static const uint kMaskTerrain = 0x02;
static const uint kMaskActor = 0x04;  // M7 skinned
static const uint kMaskAlphaTested = 0x08;
static const uint kMaskExclusion = 0x10;

// Mesh-cache pages (raw buffers); 64 slots, mirrored in Raytracer::kMeshPageSlots.
ByteAddressBuffer MeshPages[64] : register(t0, space1);

uint LoadIndex(ByteAddressBuffer a_buffer, uint a_byteOffset)
{
	const uint word = a_buffer.Load(a_byteOffset & ~3u);
	return (a_byteOffset & 2u) ? (word >> 16) : (word & 0xFFFFu);
}

// Geometric normal of the hit triangle from the cached mesh data (world space, facing against the ray).
// Returns 0 if the instance's pages aren't bound.
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

// M7c: alpha of each alpha-tested diffuse texture, one 512x512 tile per texture, filled on D3D11 (AlphaAtlas.cpp).
Texture2D<float> AlphaAtlas : register(t0, space2);
SamplerState AlphaAtlasSampler : register(s0);  // static: bilinear, clamp
static const uint kAlphaAtlasTilesPerRow = 16;  // mirrored in AlphaAtlas.h
static const float kAlphaAtlasTileSize = 512.0;

float2 LoadUV(ByteAddressBuffer a_vertices, InstanceData a_data, uint a_index, uint a_uvOffset)
{
	const uint word = a_vertices.Load(a_data.UVOffset + a_index * a_data.UVStride + a_uvOffset);  // 2 x half
	return float2(f16tof32(word & 0xFFFFu), f16tof32(word >> 16));
}

// The game's alpha test (Lighting.hlsl: discard when alpha < AlphaTestRef) for a candidate hit on an alpha-tested
// instance, with the texture's alpha from the atlas. Instances without a tile always pass (they stay opaque).
bool AlphaTestPasses(InstanceData a_data, uint a_primitive, float2 a_barycentrics)
{
	const uint tile = a_data.Alpha & 0xFFFu;
	if (tile == 0 || a_data.UVPage >= 64 || a_data.IndexPage >= 64)
		return true;
	const float threshold = float((a_data.Alpha >> 12) & 0xFFu) / 255.0;
	const uint uvOffset = (a_data.Alpha >> 20) & 0xFFu;

	const uint indexBase = a_data.IndexOffset + a_primitive * 6;
	const ByteAddressBuffer indices = MeshPages[NonUniformResourceIndex(a_data.IndexPage)];
	const ByteAddressBuffer vertices = MeshPages[NonUniformResourceIndex(a_data.UVPage)];
	const float2 uv0 = LoadUV(vertices, a_data, LoadIndex(indices, indexBase), uvOffset);
	const float2 uv1 = LoadUV(vertices, a_data, LoadIndex(indices, indexBase + 2), uvOffset);
	const float2 uv2 = LoadUV(vertices, a_data, LoadIndex(indices, indexBase + 4), uvOffset);
	const float2 uv = uv0 * (1.0 - a_barycentrics.x - a_barycentrics.y) + uv1 * a_barycentrics.x + uv2 * a_barycentrics.y;

	// Wrap addressing, then stay half a texel inside the tile so bilinear filtering never reads a neighbour.
	const uint index = tile - 1;
	const float2 origin = float2(index % kAlphaAtlasTilesPerRow, index / kAlphaAtlasTilesPerRow) * kAlphaAtlasTileSize;
	const float2 texel = origin + clamp(frac(uv) * kAlphaAtlasTileSize, 0.5, kAlphaAtlasTileSize - 0.5);
	const float alpha = AlphaAtlas.SampleLevel(AlphaAtlasSampler, texel / (kAlphaAtlasTileSize * kAlphaAtlasTilesPerRow), 0.0);
	return alpha >= threshold;
}

// Runs RayQuery a_query to completion, committing non-opaque candidates (alpha-tested instances with an atlas tile,
// flagged FORCE_NON_OPAQUE) only where the alpha test passes. The query must not use RAY_FLAG_FORCE_OPAQUE, and a
// StructuredBuffer<InstanceData> named Instances must be in scope.
#define PROCEED_ALPHA_TESTED(a_query)                                                                                   \
	while (a_query.Proceed()) {                                                                                         \
		if (a_query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&                                                 \
			AlphaTestPasses(Instances[a_query.CandidateInstanceID()], a_query.CandidatePrimitiveIndex(), a_query.CandidateTriangleBarycentrics())) \
			a_query.CommitNonOpaqueTriangleHit();                                                                       \
	}

float4 UnpackRGBA8(uint a_packed)
{
	return float4(a_packed & 0xFFu, (a_packed >> 8) & 0xFFu, (a_packed >> 16) & 0xFFu, a_packed >> 24) / 255.0;
}
