// SkyrimRT M6: average colour of one diffuse texture, for the ray-traced GI material table. Runs on D3D11 (the game's
// textures can't be shared with D3D12). Reads a 16x16 grid from the mip closest to 16x16, so textures without a full
// mip chain stay cheap. Values are stored as the texture holds them (Skyrim gamma, not sRGB-decoded).

cbuffer Params : register(b0)
{
	uint Slot;
	uint3 Pad;
};

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Output : register(u0);  // per slot: [0] alpha-weighted rgb + mean alpha, [1] plain rgb

static const uint kGrid = 16;
groupshared float4 sharedWeighted[64];
groupshared float3 sharedPlain[64];

[numthreads(8, 8, 1)] void main(uint3 groupThreadID : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
	uint width, height, mips;
	Source.GetDimensions(0, width, height, mips);
	const uint largest = max(width, height);
	const uint wanted = largest > kGrid ? firstbithigh(largest) - 4 : 0;  // log2(largest) - log2(16)
	const uint mip = min(wanted, mips - 1);
	const uint mipWidth = max(1u, width >> mip);
	const uint mipHeight = max(1u, height >> mip);

	float4 weighted = 0.0;
	float3 plain = 0.0;
	// 8x8 threads x 2x2 samples = the 16x16 grid.
	[unroll] for (uint i = 0; i < 4; i++)
	{
		const uint2 cell = groupThreadID.xy * 2 + uint2(i & 1, i >> 1);
		const uint2 texel = min(uint2((cell + 0.5) / kGrid * float2(mipWidth, mipHeight)), uint2(mipWidth - 1, mipHeight - 1));
		const float4 color = Source.Load(int3(texel, mip));
		weighted += float4(color.rgb * color.a, color.a);
		plain += color.rgb;
	}
	sharedWeighted[groupIndex] = weighted;
	sharedPlain[groupIndex] = plain;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride) {
			sharedWeighted[groupIndex] += sharedWeighted[groupIndex + stride];
			sharedPlain[groupIndex] += sharedPlain[groupIndex + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0) {
		const float samples = kGrid * kGrid;
		const float4 w = sharedWeighted[0];
		Output[Slot * 2 + 0] = float4(w.a > 1e-3 ? w.rgb / w.a : 0.0, w.a / samples);
		Output[Slot * 2 + 1] = float4(sharedPlain[0] / samples, 1.0);
	}
}
