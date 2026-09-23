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
	 * @brief Records this frame's camera for the M4 trace. Call from the main deferred prepass, where CS's
	 * cached per-frame buffer holds the main camera.
	 * @param a_viewProjInverse FrameBuffer::CameraViewProjInverse (16 floats, as captured)
	 * @param a_posAdjust FrameBuffer::CameraPosAdjust.xyz
	 */
	void CaptureCamera(const float* a_viewProjInverse, const float* a_posAdjust, uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_gameFrame);

	/**
	 * @brief Runs the per-frame work (interop round trip, scene extraction, uploads; with a_trace also the
	 * BLAS/TLAS build and debug trace). Render thread only; no-op until Init succeeds.
	 */
	void OnFrame(bool a_trace);

	/** @brief Debug view (0 depth, 1 instance, 2 normal, 3 diff) written by the trace, or nullptr. */
	ID3D11ShaderResourceView* GetDebugViewSRV(uint32_t a_view);

	/** @brief Texture size of the debug views and the render region the trace covers. False if not tracing. */
	bool GetDebugViewSize(uint32_t& a_textureWidth, uint32_t& a_textureHeight, uint32_t& a_renderWidth, uint32_t& a_renderHeight);

	const TraceStats* GetTraceStats();

	/** @brief Queues a debug dump (frame_<n>.json + debug_testpattern_<n>.png), written once the GPU copy completes. */
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
