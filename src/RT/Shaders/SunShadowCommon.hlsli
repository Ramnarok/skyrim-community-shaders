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
	uint Pad;
};

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
