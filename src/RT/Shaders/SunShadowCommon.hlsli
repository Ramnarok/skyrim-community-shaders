// SkyrimRT M5: shared declarations for the sun-shadow passes (trace, temporal, spatial).

struct ShadowConstants
{
	row_major float4x4 ViewProjInverse;  // FrameBuffer::CameraViewProjInverse (jittered, camera-relative)
	row_major float4x4 ViewProj;         // FrameBuffer::CameraViewProj (jittered, camera-relative)
	row_major float4x4 PrevViewProj;     // previous traced frame's CameraViewProj (relative to its CameraPosAdjust)
	float4 ToSun;                        // xyz: unit direction towards the sun, w: tan(cone half-angle)
	float4 PosAdjustDelta;               // xyz: CameraPosAdjust - previous CameraPosAdjust
	uint2 RenderSize;
	uint2 PrevRenderSize;
	uint FrameIndex;
	uint HistoryValid;
	uint CasterMask;
	uint Flags;  // kFlag*
	float NormalBias;     // ray origin offset along the surface normal (game units)
	float DistanceBias;   // extra offset per unit of view distance
	float MaxDistance;    // shadow ray length
	float MaxHistory;     // temporal accumulation cap (frames)
	float DepthTolerance; // temporal: relative view-depth difference still accepted as the same surface
	float SpatialRadius;  // spatial filter radius in pixels at zero history
	float PlaneTolerance; // spatial: relative distance from the centre pixel's plane still accepted
	float CompareDistance;
	uint ViewMode;  // 0 none, 1 raw, 2 denoised
	uint PointLightCount;  // M8 point-light shadows only
	uint InverseSquare;    // M8: Inverse Square Lighting loaded
	uint RoomTest;         // M8: some traced light is portal-strict: find each pixel's room with a primary ray
	float PointLightSourceFraction;  // M9: a point light's source disc radius, as a fraction of its light radius (0 = point)
	uint3 HeroLights;                // M9 phase 2: PointLights indices of the (up to) three lights with their own mask channel; ~0 = none
	uint HeroResetMask;              // ... bit k: channel k's light changed this frame (the temporal pass restarts the history)
	uint3 HeroPad;
};

static const uint kNoHeroLight = 0xFFFFFFFFu;

static const uint kFlagCompareShadowMap = 1;

// Counter slots, mirrored in SunShadows.h
static const uint kShadowTraced = 0;
static const uint kShadowShadowed = 1;
static const uint kCompareCompared = 2;
static const uint kCompareBothLit = 3;
static const uint kCompareBothShadowed = 4;
static const uint kCompareRtOnly = 5;
static const uint kCompareMapOnly = 6;

// Counter slots of the M8 point-light variant (PointLightShadowTraceCS), mirrored in SunShadows.h
static const uint kPointTraced = 0;       // non-sky pixels
static const uint kPointOccluded = 1;     // pixels whose sampled light was blocked
static const uint kPointSampled = 2;      // pixels with an RT-shadowed light in range and facing
static const uint kPointOccluderNear32 = 3;   // occluded, by the blocker's distance from the light (game units)
static const uint kPointOccluderNear64 = 4;
static const uint kPointOccluderNear128 = 5;
static const uint kPointRoomKnown = 6;        // pixels whose primary ray found an instance in a Light Limit Fix room
static const uint kPointAnyInRange = 7;       // diagnostics: a traced light is within its radius
static const uint kPointAnyFacing = 8;        // ... and in front of the surface
static const uint kPointAnyInRoom = 9;        // ... and applies in the pixel's room
static const uint kPointCandidateSum = 10;    // sum over pixels of the lights SamplePointLight picks from (in range, facing, in room)
static const uint kPointCandidates1 = 11;     // pixels with exactly 1 such light
static const uint kPointCandidates2to3 = 12;
static const uint kPointCandidates4to7 = 13;
static const uint kPointCandidates8Plus = 14;
static const uint kPointCandidateMax = 15;    // most such lights at any pixel (InterlockedMax)
// M9 phase 2: per hero light k (channel k), the pixels it reaches (in range, facing, in room) and those where it's blocked.
static const uint kPointHeroReached0 = 16;
static const uint kPointHeroOccluded0 = 19;
static const uint kPointMixedVisibility = 22;  // M9: some light reaching the pixel is blocked and another is visible

ConstantBuffer<ShadowConstants> C : register(b0);

float3 Unproject(float2 a_ndc, float a_depth)
{
	const float4 p = mul(C.ViewProjInverse, float4(a_ndc, a_depth, 1.0));
	return p.xyz / p.w;
}

// Same pixel -> NDC mapping as CS's DeferredCompositeCS (render-region pixel centres).
float2 PixelToNdc(int2 a_pixel)
{
	const float2 uv = (a_pixel + 0.5) / float2(C.RenderSize);
	return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float3 PositionAt(int2 a_pixel, float a_depth)
{
	return Unproject(PixelToNdc(a_pixel), a_depth);
}

// Point on the near plane along the pixel's ray; the view vector is taken from here as in the M4 trace.
float3 NearPointAt(int2 a_pixel)
{
	return Unproject(PixelToNdc(a_pixel), 0.0);
}

uint Hash(uint a_value)
{
	uint x = a_value;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// Per-pixel random offset advanced by the R2 sequence each frame: decorrelated across pixels, low-discrepancy over time.
float2 Random2(uint2 a_pixel, uint a_frame)
{
	const uint h = Hash(a_pixel.x | (a_pixel.y << 16));
	const float2 base = float2(h & 0xFFFFu, h >> 16) / 65536.0;
	return frac(base + float2(0.7548776662, 0.5698402910) * float(a_frame & 1023u));
}

// Uniform point on the unit disk (concentric mapping keeps the stratification of the input). The sun's cone (M5) and the
// point lights' source discs (M9).
float2 ConcentricDisk(float2 a_u)
{
	const float2 o = a_u * 2.0 - 1.0;
	if (all(o == 0.0))
		return float2(0.0, 0.0);
	float r, theta;
	if (abs(o.x) > abs(o.y)) {
		r = o.x;
		theta = 0.78539816 * (o.y / o.x);
	} else {
		r = o.y;
		theta = 1.57079633 - 0.78539816 * (o.x / o.y);
	}
	return r * float2(cos(theta), sin(theta));
}
