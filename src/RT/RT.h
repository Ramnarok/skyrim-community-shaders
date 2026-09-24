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
		bool alphaTestedCasters = true;     ///< alpha-tested meshes cast (alpha-tested against the M7c atlas when enabled)
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
		bool independentDevice = false;  ///< The sidecar has its own D3D12 device, not the process-wide singleton frame generation uses.
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

	/**
	 * @brief One point light for the traces (GI bounce, M8 point-light shadows), from Light Limit Fix's per-frame light
	 * list (the lights Lighting.hlsl evaluates). Layout matches PointLight in Shaders/PointLights.hlsli.
	 */
	struct PointLight
	{
		float position[3]{};  ///< camera-relative (FrameBuffer::CameraPosAdjust origin, as the TLAS)
		float radius = 0.0f;
		float color[3]{};  ///< Color::PointLight(color) x fade: the light colour Lighting.hlsl multiplies by attenuation
		float invRadius = 0.0f;
		float fadeZone = 0.0f;  ///< Inverse Square Lighting attenuation terms (used when inverseSquare is set)
		float sizeBias = 0.0f;
		uint32_t flags = 0;  ///< LightLimitFix::LightFlags
		float pad = 0.0f;
		uint32_t roomFlags[4]{};  ///< LightData::roomFlags: bit n = Light Limit Fix room n (portal-strict lights only apply there)
	};
	static_assert(sizeof(PointLight) == 64);

	/** @brief M8: a room or portal node and its Light Limit Fix room index (LightLimitFix::roomNodes, rebuilt every frame). */
	struct RoomIndex
	{
		const void* node = nullptr;
		uint32_t index = 0;
	};

	/**
	 * @brief M8: this frame's room indices, before OnPrepass. The scene walk tags each instance with the index of its
	 * nearest room/portal ancestor, which the traces use to apply portal-strict lights only where Lighting.hlsl does.
	 */
	void SetRoomIndices(std::span<const RoomIndex> a_rooms);

	/**
	 * @brief Per-frame M8 point-light shadow settings: ray-traced visibility for the game's unshadowed point lights
	 * (portal-strict ones where their rooms are), as a ratio mask Lighting.hlsl reads at PS t46.
	 */
	struct PointShadowParams
	{
		std::span<const PointLight> lights;  ///< valid only during OnPrepass
		bool inverseSquare = false;          ///< Inverse Square Lighting loaded: its attenuation applies
		bool alphaTestedCasters = true;
		float normalBias = 1.0f;       ///< as SunShadowParams
		float distanceBias = 0.002f;
		uint32_t maxHistory = 24;
		float spatialRadius = 3.0f;
		uint32_t viewMode = 0;  ///< debug view: 0 none, 1 raw, 2 denoised
	};

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
		bool alphaTestedCasters = true;
		uint32_t maxAccumulatedFrames = 30;  ///< REBLUR history (frames)
		uint32_t viewMode = 0;               ///< overlay: 0 off, 1 noisy, 2 denoised, 3 ambient occlusion
		bool interior = false;               ///< interior cell: the directional light is unshadowed, as in Lighting.hlsl
		float directionalLightMult = 1.0f;   ///< Linear Lighting (used only when linearLighting is set)
		std::span<const PointLight> pointLights;  ///< valid only during SubmitGI (not kept in GIStats::params)
		bool pointLightShadows = true;             ///< trace a visibility ray to the sampled point light
		bool inverseSquare = false;                ///< Inverse Square Lighting loaded: its attenuation applies (Lighting.hlsl ISL)
		bool skyLight = false;  ///< M8: misses that reach the sky carry its radiance; the composite scales its ambient by the traced / open-sky ratio (exteriors)
		uint32_t bounces = 1;   ///< M8 multi-bounce: path vertices per GI ray (1 = the M6 single bounce, at most 3)
	};

	/** @brief The three textures Screen-Space GI normally provides to DeferredCompositeCS (t10-t12), plus the M8 sky flag. */
	struct GIOutputs
	{
		ID3D11ShaderResourceView* ao = nullptr;
		ID3D11ShaderResourceView* y = nullptr;
		ID3D11ShaderResourceView* coCg = nullptr;
		/// M8: bound at composite t16 when the GI carries sky light. Only its presence is read (GetDimensions): the
		/// composite then scales the game's ambient by the traced light over the open-sky light, instead of by the AO.
		ID3D11ShaderResourceView* skyLight = nullptr;
	};

	/** @brief Minimum tier we require: DXR 1.1 for inline RayQuery in compute shaders. */
	inline constexpr D3D12_RAYTRACING_TIER kRequiredTier = D3D12_RAYTRACING_TIER_1_1;

	/** @brief Frame-time budget for the M2 interop round trip. */
	inline constexpr float kInteropBudgetMs = 0.5f;

	/**
	 * @brief Enables the D3D12 debug layer (debug builds) and DRED (debug builds, or when a_hangDiagnostics is set):
	 * breadcrumbs with pass markers and page-fault reporting, logged if the device is removed.
	 * Must run before any D3D12 device exists in the process (CS may create one for frame generation),
	 * so call it from Feature::Load().
	 */
	void EnableDebugLayer(bool a_hangDiagnostics);

	/**
	 * @brief Debug: makes the next round trip's D3D12 queue wait forever, as a GPU hang would, so the hang watchdog's
	 * recovery can be tested. The game freezes for Sidecar::kStallSeconds, then carries on without ray tracing.
	 */
	void SimulateGpuHang();

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
	 * extraction, BLAS/TLAS, the M4 debug trace (a_debugTrace), M5 sun shadows (a_shadows non-null) and M8 point-light
	 * shadows (a_pointShadows non-null). D3D11 waits for the result on the GPU timeline; the CPU never waits. At most
	 * one round trip runs per frame.
	 */
	void OnPrepass(bool a_debugTrace, const SunShadowParams* a_shadows, const PointShadowParams* a_pointShadows, bool a_buildForGI);

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

	/** @brief M7c: alpha-test alpha-tested meshes (foliage) in every trace; off traces them as solid cards. */
	void SetAlphaTest(bool a_enabled);
	/** @brief M8: GI hits sample their diffuse texture (albedo atlas) instead of its average colour. */
	void SetAlbedoTextures(bool a_enabled);
	/** @brief M7: trace trees in their rest pose instead of their swaying bones' current pose. */
	void SetTreeRestPose(bool a_enabled);
	/** @brief M8: trace rest-pose trees as static instances of their bind-pose meshes instead of skinning them each frame. */
	void SetStaticTrees(bool a_enabled);
	/** @brief M8 diagnostic: leave shapes flagged kMeshLOD (their mesh's L1_/L2_ detail levels) out of the traced scene. */
	void SetSkipMeshLOD(bool a_enabled);
	/** @brief M7c alpha-atlas stats, or nullptr. */
	const struct AlphaAtlasStats* GetAlphaAtlasStats();
	/** @brief M8 albedo-atlas stats, or nullptr. */
	const struct AlbedoAtlasStats* GetAlbedoAtlasStats();
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

	/** @brief True when the sidecar can trace M8 point-light shadows. */
	bool CanTracePointLightShadows();

	/** @brief SRV of the M8 point-light shadow mask for this frame (R8, 1 = lit), cleared to lit if stale; nullptr if unavailable. */
	ID3D11ShaderResourceView* AcquirePointLightShadowMask();

	/** @brief SRV of the point-light shadow debug view (RGBA8) when PointShadowParams::viewMode is set, or nullptr. */
	ID3D11ShaderResourceView* GetPointLightShadowViewSRV();

	/** @brief M8 point-light shadow stats (counters per PointShadowCounter), or nullptr. */
	const SunShadowStats* GetPointLightShadowStats();

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
