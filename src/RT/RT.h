#pragma once

#include <d3d11.h>
#include <d3d12.h>

/**
 * @brief SkyrimRT's D3D12 side. All D3D12 code lives under src/RT/; CS code only sees this interface.
 *
 * M1: identify the adapter the game renders on and probe its DXR support.
 * M2: keep a D3D12 sidecar device on that adapter and prove D3D11 <-> D3D12 interop every frame.
 */
namespace RT
{
	struct SceneStats;
	struct MeshCacheStats;
	struct TimingSeries;
	struct TraceStats;
	struct SunShadowStats;

	/** @brief Per-frame sun-shadow settings, filled by the SkyrimRT feature (M5). */
	struct SunShadowParams
	{
		float toSun[3]{ 0.0f, 0.0f, 1.0f };  ///< unit direction towards the sun (or moon), world space
		float coneHalfAngleDegrees = 0.5f;  ///< apparent angular radius of the light: penumbra width
		bool alphaTestedCasters = false;    ///< alpha-tested meshes cast as opaque (no alpha textures until M7)
		float normalBias = 1.0f;            ///< ray origin offset along the surface normal (game units)
		float distanceBias = 0.002f;        ///< extra offset per unit of view distance
		uint32_t maxHistory = 24;           ///< temporal accumulation cap (frames); 1 disables accumulation
		float spatialRadius = 3.0f;         ///< spatial filter radius in pixels; 0 disables the filter
		uint32_t viewMode = 0;              ///< debug view: 0 none, 1 raw, 2 denoised
	};
	/** @brief What the capability probe found on the game's adapter. */
	struct Capabilities
	{
		std::string adapterName;
		LUID adapterLuid{};
		D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
		bool probed = false;       ///< The probe ran to completion (adapter found, D3D12 device created).
		std::string failureReason;  ///< Why the probe or sidecar setup failed.
	};

	/** @brief Fixed-size window of timing samples (milliseconds). */
	struct TimingSeries
	{
		static constexpr uint32_t kCapacity = 300;
		std::array<float, kCapacity> samples{};
		uint32_t count = 0;
		uint32_t head = 0;

		void Add(float a_ms)
		{
			samples[head] = a_ms;
			head = (head + 1) % kCapacity;
			count = std::min(count + 1, kCapacity);
		}
		float Average() const
		{
			float sum = 0.0f;
			for (uint32_t i = 0; i < count; i++)
				sum += samples[i];
			return count ? sum / count : 0.0f;
		}
		float Max() const
		{
			float m = 0.0f;
			for (uint32_t i = 0; i < count; i++)
				m = std::max(m, samples[i]);
			return m;
		}
	};

	/** @brief Per-frame interop statistics, reported in the overlay panel and the debug dump. */
	struct InteropStats
	{
		TimingSeries d3d12DispatchMs;  ///< D3D12 GPU time of the test-pattern dispatch.
		TimingSeries roundTripMs;      ///< D3D11 GPU timeline cost of Signal -> (D3D12 work) -> Wait: the frame-time cost.
		TimingSeries cpuSubmitMs;      ///< CPU time spent in OnFrame.
		TimingSeries frameMs;          ///< CPU time between Presents: compare with a feature on and off for its real cost.
		uint32_t framesSubmitted = 0;
		uint32_t framesSkipped = 0;  ///< Frame slot still busy on the GPU, so the round trip was skipped (never CPU-waited).
		uint64_t lastSignaledFenceValue = 0;
		uint64_t lastCompletedFenceValue = 0;
		HRESULT d3d12RemovedReason = S_OK;
		HRESULT d3d11RemovedReason = S_OK;
		bool deviceRemoved = false;
	};

	/** @brief Results of the ARCHITECTURE §2 sharing spike, run once at startup. */
	struct SpikeResults
	{
		bool ran = false;
		// Frame-path texture: D3D12 shared heap -> OpenSharedResource1 in D3D11, else the reverse direction.
		HRESULT textureD3D12ToD3D11 = E_NOT_VALID_STATE;
		HRESULT textureD3D11ToD3D12 = E_NOT_VALID_STATE;
		bool textureCreatedInD3D12 = false;
		// Buffer, D3D12 -> D3D11: shared heap buffer opened as ID3D11Buffer, then data verified through a D3D11 staging copy.
		HRESULT bufferD3D12ToD3D11Open = E_NOT_VALID_STATE;
		bool bufferD3D12ToD3D11Verified = false;
		uint32_t bufferMismatches = 0;
		// Buffer, D3D11 -> D3D12: D3D11 buffer with MISC_SHARED_NTHANDLE opened via ID3D12Device::OpenSharedHandle.
		HRESULT bufferD3D11Create = E_NOT_VALID_STATE;
		HRESULT bufferD3D11ToD3D12Open = E_NOT_VALID_STATE;
	};

	struct GIStats;

	/** @brief Per-frame GI inputs, filled by the SkyrimRT feature from the same sources CS's SharedData uses (M6). */
	struct GIParams
	{
		float toSun[3]{ 0.0f, 0.0f, 1.0f };  ///< SharedData::DirLightDirection
		float sunColor[3]{};                 ///< SharedData::DirLightColor
		float ambientSH[3][4]{};             ///< SharedData::AmbientSHR/G/B
		bool linearLighting = false;         ///< Linear Lighting feature enabled (approximate conversions, untested)
		float colorGamma = 1.8f;
		float lightGamma = 1.8f;
		float ambientGamma = 1.8f;
		float ambientMult = 1.0f;
		float intensity = 1.0f;       ///< scale of the bounce light fed to the composite
		float aoStrength = 1.0f;      ///< 0 = no RT ambient occlusion
		float rayLength = 3000.0f;    ///< game units
		bool alphaTestedCasters = false;
		uint32_t maxAccumulatedFrames = 30;  ///< REBLUR history (frames)
		uint32_t viewMode = 0;               ///< overlay: 0 off, 1 noisy, 2 denoised, 3 ambient occlusion
	};

	/** @brief The three textures Screen-Space GI normally provides to DeferredCompositeCS (t10-t12). */
	struct GIOutputs
	{
		ID3D11ShaderResourceView* ao = nullptr;
		ID3D11ShaderResourceView* y = nullptr;
		ID3D11ShaderResourceView* coCg = nullptr;
	};

	/** @brief Minimum tier we require: DXR 1.1 for inline RayQuery in compute shaders. */
	inline constexpr D3D12_RAYTRACING_TIER kRequiredTier = D3D12_RAYTRACING_TIER_1_1;

	/** @brief Frame-time budget for the M2 interop round trip. */
	inline constexpr float kInteropBudgetMs = 0.5f;

	/**
	 * @brief Enables the D3D12 debug layer and DRED in debug builds; no-op in release builds.
	 * Must run before any D3D12 device exists in the process (CS may create one for frame generation),
	 * so call it from Feature::Load().
	 */
	void EnableDebugLayer();

	/**
	 * @brief Probes the game's adapter for DXR support and, when supported, creates the D3D12 sidecar.
	 * @param a_device The game's D3D11 device (globals::d3d::device).
	 * @param a_context The game's immediate context (globals::d3d::context).
	 * @return True when the adapter supports kRequiredTier and the sidecar is running.
	 */
	bool Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	/**
	 * @brief Records this frame's camera for tracing. Call from the main deferred prepass, where CS's
	 * cached per-frame buffer holds the main camera.
	 * @param a_viewProjInverse FrameBuffer::CameraViewProjInverse (16 floats, as captured)
	 * @param a_viewProj FrameBuffer::CameraViewProj (16 floats, as captured)
	 * @param a_posAdjust FrameBuffer::CameraPosAdjust.xyz
	 */
	void CaptureCamera(const float* a_viewProjInverse, const float* a_viewProj, const float* a_view, const float* a_viewInverse, const float* a_projUnjittered,
		const float* a_posAdjust, uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_gameFrame);

	/**
	 * @brief Runs this frame's round trip before the opaque pass (from Feature::Prepass, after CaptureCamera): scene
	 * extraction, BLAS/TLAS, the M4 debug trace (a_debugTrace) and M5 sun shadows (a_shadows non-null). D3D11 waits
	 * for the result on the GPU timeline; the CPU never waits. At most one round trip runs per frame.
	 */
	void OnPrepass(bool a_debugTrace, const SunShadowParams* a_shadows, bool a_buildForGI);

	/** @brief True when this build contains ray-traced GI (SKYRIMRT_NRD) and it was set up successfully. */
	bool IsGIAvailable();
	/** @brief True when this build was compiled with SKYRIMRT_NRD. */
	bool IsGICompiledIn();
	/** @brief GI can run this frame: available, and OnPrepass built the scene this frame. */
	bool CanTraceGI();

	/**
	 * @brief M6, from Deferred::DeferredPasses: traces, denoises and resolves GI for this frame (a second D3D11 <-> D3D12
	 * hand-off). Returns the composite's Screen-Space GI inputs, or all null if GI couldn't run.
	 */
	GIOutputs SubmitGI(const GIParams& a_params);

	const GIStats* GetGIStats();
	/** @brief M7 skinning stats, or nullptr. */
	const struct SkinnedStats* GetRaytracerSkinnedStats();
	/** @brief GI debug view (RGBA8) when GIParams::viewMode is set, or nullptr. */
	ID3D11ShaderResourceView* GetGIViewSRV();

	/**
	 * @brief Present-time work: an untraced round trip (uploads, test pattern) if OnPrepass didn't run this frame,
	 * and the debug-dump sequence. Render thread only; no-op until Init succeeds.
	 */
	void OnFrame();

	/** @brief True when the sidecar can trace sun shadows (running, pipelines built, no device removal). */
	bool CanTraceSunShadows();

	/** @brief True while a debug dump captures its RT-off reference frame; RT shadows must not be bound then. */
	bool IsSunShadowSuppressed();

	/** @brief SRV of the RT sun-shadow mask for this frame (R8, 1 = lit), cleared to lit if stale; nullptr if unavailable. */
	ID3D11ShaderResourceView* AcquireSunShadowMask();

	/** @brief SRV of the sun-shadow debug view (RGBA8) when SunShadowParams::viewMode is set, or nullptr. */
	ID3D11ShaderResourceView* GetSunShadowViewSRV();

	const SunShadowStats* GetSunShadowStats();

	/** @brief Debug view (0 depth, 1 instance, 2 normal, 3 diff) written by the trace, or nullptr. */
	ID3D11ShaderResourceView* GetDebugViewSRV(uint32_t a_view);

	/** @brief Texture size of the debug views and the render region the trace covers. False if not tracing. */
	bool GetDebugViewSize(uint32_t& a_textureWidth, uint32_t& a_textureHeight, uint32_t& a_renderWidth, uint32_t& a_renderHeight);

	const TraceStats* GetTraceStats();

	/**
	 * @brief Queues a debug dump: frame_<n>.json plus debug_<view>_<n>.png images (test pattern, debug views, sun-shadow
	 * masks, final frame with RT shadows on and off), written once the GPU copies complete.
	 */
	void RequestDebugDump();

	/** @brief SRV of the D3D12-written test pattern, or nullptr before the first round trip. */
	ID3D11ShaderResourceView* GetTestPatternSRV();

	/** @brief Result of the last Init() call. */
	const Capabilities& GetCapabilities();

	/** @brief True when Init() found an adapter that supports kRequiredTier. */
	bool IsSupported();

	/** @brief True when the sidecar device is running. */
	bool IsRunning();

	const InteropStats* GetInteropStats();
	const SpikeResults* GetSpikeResults();
	const SceneStats* GetSceneStats();
	const TimingSeries* GetSceneTraversalMs();
	const MeshCacheStats* GetMeshCacheStats();

	/** @brief Human-readable name for a raytracing tier, e.g. "1.1"; derived from the enum value so tiers newer than the SDK still print. */
	std::string GetTierName(D3D12_RAYTRACING_TIER a_tier);

	/** @brief Formats a LUID as "HighPart:LowPart" in hex. */
	std::string FormatLuid(const LUID& a_luid);

	/** @brief Formats an HRESULT as hex, or "S_OK". */
	std::string FormatHResult(HRESULT a_hr);

	/** @brief Folder the debug dumps go to: <Documents>/My Games/Skyrim Special Edition/SKSE/SkyrimRT. */
	std::filesystem::path GetDumpDirectory();
}
