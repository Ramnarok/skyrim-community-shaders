// SkyrimRT M9 phase 2: the point-light shadow history holds four visibility means (three hero lights and the rest), so
// it can't share the sun's RGBA16F (mean, length, view depth) layout. Packed into one R32G32B32A32_UINT texel: two 16-bit
// means per word, then the history length and the view depth as float bits.

#ifndef SKYRIMRT_POINT_SHADOW_HISTORY_HLSLI
#define SKYRIMRT_POINT_SHADOW_HISTORY_HLSLI

struct PointShadowHistory
{
	float4 Means;  // xyz: hero lights 0-2, w: the rest
	float Length;  // frames accumulated (0 = no history)
	float ViewDepth;
};

uint4 PackPointShadowHistory(PointShadowHistory a_history)
{
	const uint4 means = uint4(round(saturate(a_history.Means) * 65535.0));
	return uint4(means.x | (means.y << 16), means.z | (means.w << 16), asuint(a_history.Length), asuint(a_history.ViewDepth));
}

PointShadowHistory UnpackPointShadowHistory(uint4 a_packed)
{
	PointShadowHistory history;
	history.Means = float4(a_packed.x & 0xFFFFu, a_packed.x >> 16, a_packed.y & 0xFFFFu, a_packed.y >> 16) / 65535.0;
	history.Length = asfloat(a_packed.z);
	history.ViewDepth = asfloat(a_packed.w);
	return history;
}

#endif
