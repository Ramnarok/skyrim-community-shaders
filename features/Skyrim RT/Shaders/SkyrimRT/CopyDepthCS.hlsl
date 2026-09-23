// SkyrimRT M4: copies the game's pre-water scene depth (R24_UNORM_X8 SRV, or TerrainBlending's R32)
// into an R32_FLOAT texture shared with the D3D12 sidecar. Runs on D3D11 before the fence signal.

Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> Destination : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint width, height;
	Destination.GetDimensions(width, height);
	if (dispatchID.x >= width || dispatchID.y >= height)
		return;
	Destination[dispatchID.xy] = SourceDepth[dispatchID.xy];
}
