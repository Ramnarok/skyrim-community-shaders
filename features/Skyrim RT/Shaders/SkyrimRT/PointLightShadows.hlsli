// SkyrimRT M8/M9: ray-traced visibility of the game's point lights, traced before the opaque pass from the depth
// pre-pass (src/RT/Shaders/PointLightShadowTraceCS.hlsl), the shadow-mapped ones included (M9: they no longer use the
// game's shadow map while the mask is bound). M9 phase 2: the mask has four channels. The frame's three hero lights (the
// brightest near the view, chosen on the CPU) each have their own: xyz. Every other light shares w, the ratio over them.
// Each light finds its channel by its position (t48, exactly Light Limit Fix's positionWS). A channel is 1 where its
// light doesn't reach. SkyrimRT unbinds t46 and t48 whenever it isn't tracing, and then Lighting.hlsl keeps the vanilla
// path (shadow maps for the Shadow lights, the rest unshadowed).

namespace SkyrimRT
{
	Texture2D<unorm float4> PointLightShadowTexture : register(t46);
	Texture2D<float4> PointLightHeroes : register(t48);  // texel k: hero light k's camera-relative position, w = 1 when set

	struct PointLightShadows
	{
		float4 Mask;  // xyz: the hero lights, w: the rest
		float4 Hero0, Hero1, Hero2;
	};

	// Whether this frame's ray-traced point-light visibility is bound (it then replaces the game's point-light shadow maps).
	bool IsPointLightShadowBound()
	{
		uint width, height;
		PointLightShadowTexture.GetDimensions(width, height);  // 0 when unbound
		return width != 0;
	}

	// a_position: SV_Position (pixel centres at .5), in the dynamic-resolution render region like the mask. When not
	// a_bound, every light reads as lit.
	PointLightShadows LoadPointLightShadows(float4 a_position, bool a_bound)
	{
		PointLightShadows result;
		result.Mask = 1.0;
		result.Hero0 = 0.0;
		result.Hero1 = 0.0;
		result.Hero2 = 0.0;
		if (a_bound) {
			result.Mask = PointLightShadowTexture.Load(int3(int2(a_position.xy), 0));
			result.Hero0 = PointLightHeroes.Load(int3(0, 0, 0));
			result.Hero1 = PointLightHeroes.Load(int3(1, 0, 0));
			result.Hero2 = PointLightHeroes.Load(int3(2, 0, 0));
		}
		return result;
	}

	bool IsHeroLight(float4 a_hero, float3 a_lightPosition)
	{
		return a_hero.w > 0.5 && all(abs(a_hero.xyz - a_lightPosition) < 0.01);
	}

	// The visibility of the light at a_lightPosition (LightLimitFix::Light::positionWS.xyz).
	float GetPointLightShadow(PointLightShadows a_shadows, float3 a_lightPosition)
	{
		if (IsHeroLight(a_shadows.Hero0, a_lightPosition))
			return a_shadows.Mask.x;
		if (IsHeroLight(a_shadows.Hero1, a_lightPosition))
			return a_shadows.Mask.y;
		if (IsHeroLight(a_shadows.Hero2, a_lightPosition))
			return a_shadows.Mask.z;
		return a_shadows.Mask.w;
	}
}
