// SkyrimRT: copies one diffuse texture into a tile of a shared atlas the RayQuery passes sample (TextureAtlas.cpp). Runs
// on D3D11 (the game's textures can't be shared with D3D12). Samples the mip whose larger side is closest to the tile, so
// the tile holds what the game's own mip chain shows at that size.
//   default: the alpha channel, for the M7c alpha test (R8 atlas, 512² tiles)
//   ALBEDO:  the colour as the texture stores it (Skyrim gamma), for M8 materials at GI hits (RGBA8 atlas, 128² tiles)

cbuffer Params : register(b0)
{
	uint2 TileOrigin;  // top-left texel of the tile in the atlas
	uint TileSize;
	uint Pad;
};

Texture2D<float4> Source : register(t0);
SamplerState LinearWrap : register(s0);
#if defined(ALBEDO)
RWTexture2D<unorm float4> Atlas : register(u0);
#else
RWTexture2D<unorm float> Atlas : register(u0);
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= TileSize))
		return;
	uint width, height, mips;
	Source.GetDimensions(0, width, height, mips);
	const float lod = clamp(log2(float(max(width, height)) / TileSize), 0.0, float(mips - 1));
	const float2 uv = (dispatchID.xy + 0.5) / TileSize;
	const float4 texel = Source.SampleLevel(LinearWrap, uv, lod);
#if defined(ALBEDO)
	Atlas[TileOrigin + dispatchID.xy] = float4(texel.rgb, 1.0);
#else
	Atlas[TileOrigin + dispatchID.xy] = texel.a;
#endif
}
