// SkyrimRT M7: linear-blend skinning of one skin partition on the D3D12 side (the game's skinned buffers can't be
// shared). Reads the cached bind-pose vertices, applies the partition's bone palette (boneWorld * skinToBone,
// camera-relative) and writes float3 positions for the BLAS build/refit and the hit-normal lookup.

cbuffer Params : register(b0)
{
	uint VertexCount;
	uint Stride;
	uint SkinningOffset;  // weights: 4 x half, then bone indices: 4 x uint8 (partition palette indices)
	uint HalfPositions;   // 1: position is 4 x half, 0: float3 (+ pad)
	uint BoneCount;
	uint DynamicStride;  // M7b: > 0 when positions come from DynamicPositions (BSDynamicTriShape) instead of Source
	uint2 Pad;
};

ByteAddressBuffer Source : register(t0);       // bind-pose vertices (mesh pool)
StructuredBuffer<float4> Palette : register(t1);  // 3 rows of a 3x4 per bone
ByteAddressBuffer DynamicPositions : register(t2);  // M7b: the game's CPU-side morphed positions, uploaded
RWByteAddressBuffer Output : register(u0);     // float3 per vertex

float3 TransformByBone(uint a_bone, float3 a_position)
{
	const float4 p = float4(a_position, 1.0);
	return float3(dot(Palette[a_bone * 3 + 0], p), dot(Palette[a_bone * 3 + 1], p), dot(Palette[a_bone * 3 + 2], p));
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint vertex = dispatchID.x;
	if (vertex >= VertexCount)
		return;
	const uint base = vertex * Stride;

	float3 position;
	if (DynamicStride > 0) {
		position = asfloat(DynamicPositions.Load3(vertex * DynamicStride));
	} else if (HalfPositions) {
		const uint2 h = Source.Load2(base);
		position = float3(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y));
	} else {
		position = asfloat(Source.Load3(base));
	}

	const uint2 packedWeights = Source.Load2(base + SkinningOffset);
	const uint indices = Source.Load(base + SkinningOffset + 8);
	const float weights[4] = { f16tof32(packedWeights.x), f16tof32(packedWeights.x >> 16), f16tof32(packedWeights.y), f16tof32(packedWeights.y >> 16) };

	float3 skinned = 0.0;
	float total = 0.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		const uint bone = (indices >> (8 * i)) & 0xFFu;
		if (weights[i] <= 0.0 || bone >= BoneCount)
			continue;
		skinned += weights[i] * TransformByBone(bone, position);
		total += weights[i];
	}
	skinned = total > 0.0 ? skinned / total : TransformByBone(0, position);

	Output.Store3(vertex * 12, asuint(skinned));
}
