// SkyrimRT M8/M9: ray-traced visibility of the game's point lights, traced before the opaque pass from the depth
// pre-pass (src/RT/Shaders/PointLightShadowTraceCS.hlsl). One ratio per pixel over every light that applies to the
// pixel's room, the shadow-mapped ones included (M9: they no longer use the game's shadow map while the mask is bound);
// 1 where no light is in range. SkyrimRT unbinds t46 whenever it isn't tracing, and then Lighting.hlsl keeps the vanilla
// path (shadow maps for the Shadow lights, the rest unshadowed).

namespace SkyrimRT
{
	Texture2D<unorm float> PointLightShadowTexture : register(t46);

	// Whether this frame's ray-traced point-light visibility is bound (it then replaces the game's point-light shadow maps).
	bool IsPointLightShadowBound()
	{
		uint width, height;
		PointLightShadowTexture.GetDimensions(width, height);  // 0 when unbound
		return width != 0;
	}

	// a_position: SV_Position (pixel centres at .5), in the dynamic-resolution render region like the mask.
	float GetPointLightShadow(float4 a_position)
	{
		return PointLightShadowTexture.Load(int3(int2(a_position.xy), 0)).x;
	}
}
