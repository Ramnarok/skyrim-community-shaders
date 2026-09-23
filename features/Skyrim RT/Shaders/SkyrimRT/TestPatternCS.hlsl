// SkyrimRT M2 interop test pattern, dispatched on the D3D12 sidecar queue.
// Writes an animated pattern into a texture shared with D3D11 so the overlay can show it.

cbuffer TestPatternCB : register(b0)
{
	float Time;
	uint FrameIndex;
	uint2 Size;
};

RWTexture2D<unorm float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (any(dispatchID.xy >= Size))
		return;

	float2 uv = (dispatchID.xy + 0.5) / float2(Size);

	// Scrolling colour bars.
	float3 bars = 0.5 + 0.5 * cos(6.2831853 * (uv.x + Time * 0.25) + float3(0.0, 2.094, 4.189));

	// Orbiting circle: proves the texture changes every frame.
	float2 centre = 0.5 + 0.3 * float2(cos(Time * 2.0), sin(Time * 2.0));
	float circle = step(length(uv - centre), 0.12);

	// Frame-parity checker in one corner: flips every frame, so a stale texture is obvious.
	bool corner = all(uv < 0.15);
	float checker = ((dispatchID.x / 8 + dispatchID.y / 8 + FrameIndex) & 1) ? 1.0 : 0.0;

	float3 colour = lerp(bars, float3(1.0, 1.0, 1.0), circle);
	colour = corner ? checker.xxx : colour;

	Output[dispatchID.xy] = float4(colour, 1.0);
}
