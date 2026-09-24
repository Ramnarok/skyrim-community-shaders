// SkyrimRT M8: Light Limit Fix's point lights as the traces see them (RT::PointLight), with the falloff Lighting.hlsl
// applies and the light sampling shared by the GI bounce and the point-light shadow trace.

#ifndef SKYRIMRT_POINT_LIGHTS_HLSLI
#define SKYRIMRT_POINT_LIGHTS_HLSLI

struct PointLight
{
	float3 Position;  // camera-relative, like the TLAS
	float Radius;
	float3 Color;  // Color::PointLight(color) x fade
	float InvRadius;
	float FadeZone;
	float SizeBias;
	uint Flags;  // LightLimitFix::LightFlags
	float Pad;
	uint4 RoomFlags;  // bit n of word n / 32: Light Limit Fix room n (portal-strict lights only light those rooms)
};

static const uint kLightFlagPortalStrict = 1u << 0;  // LightLimitFix::LightFlags
static const uint kLightFlagShadow = 1u << 1;
static const uint kLightFlagDisabled = 1u << 9;
static const uint kLightFlagInverseSquare = 1u << 10;

// LightLimitFix::IsLightIgnored for a portal-strict light: it applies only to geometry in one of its rooms, except that
// geometry without a known room (a_room < 0) gets every light, as Lighting.hlsl's RoomIndex = -1.
bool PointLightAppliesInRoom(PointLight a_light, int a_room)
{
	if (!(a_light.Flags & kLightFlagPortalStrict) || a_room < 0 || a_room >= 128)
		return true;
	return ((a_light.RoomFlags[a_room >> 5] >> (a_room & 31)) & 1u) != 0;
}

// Visibility rays stop short of the light: lights sit inside their own fixture (lantern frames, sconces, braziers), which
// would otherwise shadow every surface they light. Measured in M8: 16 units covers lanterns (under 2.5% of occluded
// samples blocked within 32 units in the Riverwood Trader), but not a hearth: Dragonsreach's fire light (radius 622)
// sits above its log pile, and the logs 93-128 units from it shadowed the floor around the pit (0.15 x radius left an
// arc-shaped dark band). The clearance therefore grows with the light's radius: ~47 units for a candle, ~128 for a
// torch, ~155-200 for a hearth.
static const float kLightClearance = 16.0;               // game units, minimum
static const float kLightClearanceRadiusFraction = 0.25;

float PointLightClearance(float a_radius)
{
	return max(kLightClearance, kLightClearanceRadiusFraction * a_radius);
}

// Lighting.hlsl's point-light falloff: InverseSquareLighting::GetAttenuation when that feature is loaded, else
// 1 - (d / r)^2.
float PointLightAttenuation(float a_distance, PointLight a_light, bool a_inverseSquare)
{
	const float falloff = saturate(a_distance * a_light.InvRadius);
	const float regular = 1.0 - falloff * falloff;
	if (!a_inverseSquare)
		return regular;
	static const float kScaledUnitsSq = 0.8 * 70.0 * 70.0;  // InverseSquareLighting::SCALED_UNITS_SQ
	const float t = saturate((a_light.Radius - a_distance) * a_light.FadeZone);
	const float inverseSquare = kScaledUnitsSq / (a_distance * a_distance + a_light.SizeBias) * (t * t * (3.0 - 2.0 * t));
	const float enabled = (a_light.Flags & kLightFlagDisabled) ? 0.0 : 1.0;
	return ((a_light.Flags & kLightFlagInverseSquare) ? inverseSquare : regular) * enabled;
}

struct PointLightSample
{
	bool Valid;          // some light is in range and facing the surface
	float3 ToLight;      // from the surface position to the chosen light
	float3 Irradiance;   // the chosen light's unshadowed irradiance, divided by its selection probability
	float Clearance;     // how far short of the chosen light its visibility ray stops (PointLightClearance)
};

// Picks one light in proportion to its unshadowed contribution (luminance of colour x attenuation x N.L) in a single
// streaming pass (weighted reservoir with one random number), skipping lights with any of a_skipFlags and portal-strict
// lights of other rooms (a_room: the surface's Light Limit Fix room, -1 if none). With the estimate Irradiance x V, the
// result is exact wherever every light is visible; visibility is the only noise.
PointLightSample SamplePointLight(StructuredBuffer<PointLight> a_lights, uint a_count, uint a_skipFlags, bool a_inverseSquare,
	float3 a_position, float3 a_normal, float a_u, int a_room)
{
	PointLightSample result;
	result.Valid = false;
	result.ToLight = 0.0;
	result.Irradiance = 0.0;
	result.Clearance = kLightClearance;
	float total = 0.0;
	float u = a_u;
	float chosenWeight = 0.0;
	[loop] for (uint i = 0; i < a_count; i++)
	{
		const PointLight light = a_lights[i];
		if ((light.Flags & a_skipFlags) || !PointLightAppliesInRoom(light, a_room))
			continue;
		const float3 toLight = light.Position - a_position;
		const float distanceSq = dot(toLight, toLight);
		if (distanceSq >= light.Radius * light.Radius)
			continue;
		const float cosine = dot(a_normal, toLight);  // x distance
		if (cosine <= 0.0)
			continue;
		const float distance = sqrt(distanceSq);
		const float3 irradiance = max(light.Color, 0.0) * (PointLightAttenuation(distance, light, a_inverseSquare) * cosine / max(distance, 1e-4));
		const float weight = dot(irradiance, float3(0.2126, 0.7152, 0.0722));
		if (!(weight > 0.0))
			continue;
		total += weight;
		const float p = weight / total;
		if (u < p) {
			result.Irradiance = irradiance;
			result.ToLight = toLight;
			result.Clearance = PointLightClearance(light.Radius);
			chosenWeight = weight;
			u /= p;
		} else {
			u = (u - p) / (1.0 - p);
		}
	}
	if (chosenWeight > 0.0) {
		result.Valid = true;
		result.Irradiance *= total / chosenWeight;
	}
	return result;
}

#endif
