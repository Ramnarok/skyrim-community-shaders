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
	uint Pad;
};

// InstanceData.Flags bits, mirrored in Raytracer.cpp
static const uint kInstanceTerrain = 1;
static const uint kInstanceAlphaTested = 2;

// InstanceMask bits, mirrored in Raytracer.cpp
static const uint kMaskStatic = 0x01;
static const uint kMaskTerrain = 0x02;
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

float4 UnpackRGBA8(uint a_packed)
{
	return float4(a_packed & 0xFFu, (a_packed >> 8) & 0xFFu, (a_packed >> 16) & 0xFFu, a_packed >> 24) / 255.0;
}
