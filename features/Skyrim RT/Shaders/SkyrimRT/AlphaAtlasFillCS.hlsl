// SkyrimRT M7c: copies one diffuse texture's alpha into a 512x512 tile of the shared alpha atlas, which the RayQuery
// passes sample to alpha-test foliage. Runs on D3D11 (the game's textures can't be shared with D3D12). Samples the mip
// whose larger side is closest to the tile, so the tile holds what the game's own mip chain shows at that size.

cbuffer Params : register(b0)
{
	uint2 TileOrigin;  // top-left texel of the tile in the atlas
	uint2 Pad;
};

Texture2D<float4> Source : register(t0);
SamplerState LinearWrap : register(s0);
RWTexture2D<unorm float> Atlas : register(u0);

static const uint kTileSize = 512;  // AlphaAtlas::kTileSize

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint width, height, mips;
	Source.GetDimensions(0, width, height, mips);
	const float lod = clamp(log2(float(max(width, height)) / kTileSize), 0.0, float(mips - 1));
	const float2 uv = (dispatchID.xy + 0.5) / kTileSize;
	Atlas[TileOrigin + dispatchID.xy] = Source.SampleLevel(LinearWrap, uv, lod).a;
}
