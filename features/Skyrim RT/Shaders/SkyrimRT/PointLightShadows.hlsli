// SkyrimRT M8: ray-traced visibility of the game's unshadowed point lights, traced before the opaque pass from the
// depth pre-pass (src/RT/Shaders/PointLightShadowTraceCS.hlsl). One ratio per pixel over the lights without the Shadow
// or PortalStrict flag; 1 where no such light is in range. SkyrimRT unbinds t46 whenever it isn't tracing, and an
// unbound slot reads as fully lit.

namespace SkyrimRT
{
	Texture2D<unorm float> PointLightShadowTexture : register(t46);

	// a_position: SV_Position (pixel centres at .5), in the dynamic-resolution render region like the mask.
	float GetPointLightShadow(float4 a_position)
	{
		uint width, height;
		PointLightShadowTexture.GetDimensions(width, height);  // 0 when unbound
		if (width == 0)
			return 1.0;
		return PointLightShadowTexture.Load(int3(int2(a_position.xy), 0)).x;
	}

	// Lights the ray-traced ratio covers: the game shadow-maps Shadow lights and culls PortalStrict ones by room.
	// Included after LightLimitFix.hlsli.
	bool IsPointLightRayTraced(uint a_lightFlags)
	{
		return (a_lightFlags & (LightLimitFix::LightFlags::Shadow | LightLimitFix::LightFlags::PortalStrict)) == 0;
	}
}
