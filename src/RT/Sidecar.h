#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "FrameCapture.h"
#include "GlobalIllumination.h"
#include "AlphaAtlas.h"
#include "MaterialTable.h"
#include "MeshCache.h"
#include "RT.h"
#include "Raytracer.h"
#include "Scene.h"

namespace RT
{
	/**
	 * @brief The persistent D3D12 device that runs next to the game's D3D11 device.
	 *
	 * Owns the queue, the shared fence and the M2 interop test: every frame D3D11 signals the
	 * fence, the D3D12 queue waits on it, dispatches a test-pattern compute shader into a texture
	 * shared with D3D11, signals back, and D3D11 waits before the overlay samples the texture.
	 * Nothing here blocks the CPU on the GPU during a frame; per-frame slots are skipped instead.
	 */
	class Sidecar
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kPatternSize = 256;

		/**
		 * @brief Creates the queue, fence, shared test texture and compute pipeline, then runs the shared-buffer spike.
		 * @param a_device The probe-verified D3D12 device on the game's adapter (ownership is taken).
		 * @return False if any required object could not be created; failureReason is set.
		 */
		bool Init(winrt::com_ptr<ID3D12Device> a_device, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context, uint32_t a_screenWidth, uint32_t a_screenHeight);

		/**
		 * @brief This game frame's interop round trip (render thread; at most one per game frame): scene extraction,
		 * D3D11 → D3D12 (test pattern; with a_debugTrace and/or a_shadows the BLAS/TLAS build, M4 debug trace and
		 * M5 sun shadows) → D3D11, then mesh uploads. SkyrimRT::Prepass calls it before the opaque pass.
		 */
		void Submit(uint32_t a_gameFrame, const FrameCamera& a_camera, bool a_debugTrace, const SunShadowParams* a_shadows,
			const PointShadowParams* a_pointShadows, bool a_buildForGI);

		/**
		 * @brief M6: the frame's second hand-off, from Deferred::DeferredPasses once the G-buffer is complete: GI trace,
		 * NRD and resolve, reusing the TLAS Submit built this frame. D3D11 waits for it on the GPU timeline.
		 * @return The composite inputs, or all null if GI couldn't run this frame (the caller falls back to SSGI's).
		 */
		GIOutputs SubmitGI(uint32_t a_gameFrame, const GIParams& a_params);

		/** @brief GI was compiled in (SKYRIMRT_NRD) and set up, and the scene was built this frame. */
		bool CanTraceGI(uint32_t a_gameFrame) const;
		bool IsGICompiledIn() const;
		const GIStats* GetGIStats() const;
		ID3D11ShaderResourceView* GetGIViewSRV() const;
		const MaterialTableStats& GetMaterialStats() const { return materialTable.GetStats(); }
		const AlphaAtlasStats& GetAlphaAtlasStats() const { return alphaAtlas.GetStats(); }
		/** @brief M7c: alpha-test foliage against the atlas (else alpha-tested meshes are traced as solid cards). */
		void SetAlphaTest(bool a_enabled) { alphaTestEnabled = a_enabled; }
		/** @brief M7: trace trees in their rest pose (their swaying bones hold whichever culling camera's pose came last). */
		void SetTreeRestPose(bool a_enabled) { treeRestPose = a_enabled; }
		/** @brief M8: Light Limit Fix's room indices for this frame (see RT::SetRoomIndices). */
		void SetRoomIndices(std::span<const RoomIndex> a_rooms)
		{
			roomIndices.clear();
			for (const auto& room : a_rooms)
				roomIndices.emplace(room.node, room.index + 1);
		}

		/** @brief Present time: an untraced round trip if none ran this frame (menus, loading), and the dump sequence. */
		void OnPresent(uint32_t a_gameFrame);

		/**
		 * @brief Queues a dump. Its data rides along with the next round trip; the final frame is then captured with RT
		 * shadows on and, kSuppressFrames later, with them off (Screen-Space Shadows in their place).
		 */
		void RequestDebugDump() { dumpRequested = true; }

		/** @brief True while a dump is capturing its RT-off reference frame (sun shadows, point-light shadows and GI all handed back). */
		bool IsSunShadowSuppressed() const { return dumpStage == DumpStage::kSuppressing; }

		bool CanTraceSunShadows() const { return !deviceRemoved && raytracerReady && raytracer.SunShadowsReady(); }

		/** @brief The RT sun-shadow mask for this frame's lighting, cleared to lit first if it wasn't traced recently. */
		ID3D11ShaderResourceView* AcquireSunShadowMask(uint32_t a_gameFrame);

		bool CanTracePointLightShadows() const { return !deviceRemoved && raytracerReady && raytracer.PointShadowsReady(); }

		/** @brief M8: the RT point-light shadow mask for this frame, cleared to lit first if it wasn't traced recently. */
		ID3D11ShaderResourceView* AcquirePointLightShadowMask(uint32_t a_gameFrame);

		ID3D11ShaderResourceView* GetTestPatternSRV() const { return framesSubmitted > 0 ? patternSRV11.get() : nullptr; }
		const InteropStats& GetStats() const { return stats; }
		const SpikeResults& GetSpikeResults() const { return spike; }
		const SceneStats& GetSceneStats() const { return sceneStats; }
		const TimingSeries& GetSceneTraversalMs() const { return sceneTraversalMs; }
		const MeshCacheStats& GetMeshCacheStats() const { return meshCache.GetStats(); }
		const Raytracer* GetRaytracer() const { return raytracerReady ? &raytracer : nullptr; }
		const SunShadows* GetSunShadows() const { return (raytracerReady && raytracer.SunShadowsReady()) ? &raytracer.GetSunShadows() : nullptr; }
		const SunShadows* GetPointShadows() const { return (raytracerReady && raytracer.PointShadowsReady()) ? &raytracer.GetPointShadows() : nullptr; }
		const std::string& GetFailureReason() const { return failureReason; }

	private:
		bool CreatePatternTexture();
		bool CreatePipeline();
		bool CreateTimingObjects();
		void RunSharedBufferSpike();
		bool WaitForFenceBlocking(uint64_t a_value);  // Init-time only, never on the frame path.
		void CheckDeviceRemoved();
		void LogDeviceRemovedDetails();
		/** @brief Starts collecting SetPassMarker names for a_list (render thread), so a DRED report can name passes. */
		void BeginMarkerLog(ID3D12GraphicsCommandList* a_list);
		void EndMarkerLog();
		std::array<PassMarkerLog, 8> markerLogs;  // the last lists recorded (a hung list is at most a few frames old)
		uint32_t nextMarkerLog = 0;
		void CollectTimings(uint32_t a_slot);
		void FinishDumpIfReady();

		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12Device5> device5;  // raytracing interface of the same device
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12CommandAllocator> allocators[kFramesInFlight];
		winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;
		winrt::com_ptr<ID3D12GraphicsCommandList> uploadList;  // mesh uploads, executed after the D3D11 handoff signal
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t fenceValue = 0;
		uint64_t slotFenceValues[kFramesInFlight]{};

		winrt::com_ptr<ID3D11Device5> d3d11Device;
		winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
		winrt::com_ptr<ID3D11Fence> d3d11Fence;

		// Test pattern: created in D3D12 with a shared heap and opened in D3D11 (or the reverse, if that fails).
		winrt::com_ptr<ID3D12Resource> patternTexture;
		winrt::com_ptr<ID3D11Texture2D> patternTexture11;
		winrt::com_ptr<ID3D11ShaderResourceView> patternSRV11;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptorHeap;
		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pipeline;

		// GPU timing: D3D12 timestamps around the dispatch, D3D11 timestamps around the fence round trip.
		winrt::com_ptr<ID3D12QueryHeap> timestampHeap;
		winrt::com_ptr<ID3D12Resource> timestampReadback;
		const uint64_t* timestampData = nullptr;
		uint64_t d3d12TimestampFrequency = 0;
		winrt::com_ptr<ID3D11Query> d3d11Disjoint[kFramesInFlight];
		winrt::com_ptr<ID3D11Query> d3d11Begin[kFramesInFlight];
		winrt::com_ptr<ID3D11Query> d3d11End[kFramesInFlight];
		bool slotHasTimings[kFramesInFlight]{};

		// Debug dump: readback of the pattern texture, then the final-frame captures (RT shadows on, then off).
		enum class DumpStage
		{
			kIdle,
			kCaptureOn,    // the dump's round trip ran this frame; capture the final frame at Present
			kSuppressing,  // RT shadows handed back to Screen-Space Shadows until the RT-off capture
			kFinishing,    // waiting for the fence and the captures, then written
		};
		static constexpr uint32_t kSuppressFrames = 3;
		winrt::com_ptr<ID3D12Resource> patternReadback;
		uint32_t patternRowPitch = 0;
		bool dumpRequested = false;
		DumpStage dumpStage = DumpStage::kIdle;
		uint64_t dumpFenceValue = 0;
		uint32_t dumpPatternFrame = 0;
		uint32_t dumpGameFrame = 0;
		uint32_t suppressUntilFrame = 0;
		bool dumpShadowsTraced = false;
		bool dumpPointShadowsTraced = false;
		FrameCapture captureOn;
		FrameCapture captureOff;

		bool dumpGITraced = false;

		// At most one round trip per game frame.
		bool haveSubmitted = false;
		uint32_t lastSubmitGameFrame = 0;

		// The frame whose TLAS / instance data are current (M6 GI reuses them), and the slot they live in.
		bool sceneBuilt = false;
		uint32_t sceneGameFrame = 0;
		uint32_t sceneSlot = 0;
		FrameCamera sceneCamera;
		uint32_t sceneRenderWidth = 0;
		uint32_t sceneRenderHeight = 0;

		// M6.
		MaterialTable materialTable;
		bool materialTableReady = false;
		// M7c.
		AlphaAtlas alphaAtlas;
		bool alphaAtlasReady = false;
		bool alphaTestEnabled = true;
		bool treeRestPose = true;
		ankerl::unordered_dense::map<const void*, uint32_t> roomIndices;  // M8: room node -> Light Limit Fix index + 1
#if defined(SKYRIMRT_NRD)
		std::unique_ptr<GlobalIllumination> gi;
#endif
		winrt::com_ptr<ID3D11Query> giDisjoint[kFramesInFlight];
		winrt::com_ptr<ID3D11Query> giBegin[kFramesInFlight];
		winrt::com_ptr<ID3D11Query> giEnd[kFramesInFlight];
		bool slotHasGITimings[kFramesInFlight]{};

		// M5: when the mask was last written, so a stale mask is cleared to lit instead of being reused.
		bool shadowTracedEver = false;
		uint32_t lastShadowGameFrame = 0;
		bool maskClearedWhileStale = false;
		// M8, the same for the point-light mask.
		bool pointShadowTracedEver = false;
		uint32_t lastPointShadowGameFrame = 0;
		bool pointMaskClearedWhileStale = false;

		LARGE_INTEGER qpcFrequency{};
		LARGE_INTEGER lastPresent{};
		LARGE_INTEGER startTime{};
		uint32_t framesSubmitted = 0;
		bool deviceRemoved = false;

		// M3 scene extraction.
		MeshCache meshCache;
		std::vector<GeometryCandidate> candidates;
		SkinnedScene skinnedScene;
		std::vector<ExclusionBound> exclusions;
		LoadedArea loadedArea;
		SceneStats sceneStats;
		TimingSeries sceneTraversalMs;
		bool inWorld = false;

		// M4 ray tracing.
		Raytracer raytracer;
		bool raytracerReady = false;
		bool dumpHasTrace = false;

		InteropStats stats;
		SpikeResults spike;
		std::string failureReason;

		// GPU hang recovery. D3D11 queues a GPU wait for each value D3D12 signals back. A removed D3D12 device's fences
		// read UINT64_MAX everywhere, so a TDR (0x887A0006) releases those waits by itself, provided the sidecar has its
		// own device (RT::CreateSidecarDevice): sharing the process-wide one also removed the device frame generation
		// presents through, which froze the game. A queue stuck on a fence never TDRs, so the watchdog thread removes
		// the device after kStallSeconds without fence progress. Ray tracing then stays off until restart.
		void StartWatchdog();
		void WatchdogLoop(std::stop_token a_stop);
		std::atomic<uint64_t> awaitedFenceValue{ 0 };  // highest value D3D11 has been told to wait on
		std::atomic<bool> hangRescued{ false };        // the watchdog saw the device removed, or removed it
		std::atomic<bool> simulateHang{ false };

	public:
		static constexpr uint32_t kStallSeconds = 5;  ///< no fence progress for this long while D3D11 waits = hung
		static constexpr uint64_t kSimulatedHangValue = 1ull << 61;  ///< never reached by the frame loop
		/** @brief Debug: the next round trip's queue waits on a value nothing signals (see RT::SimulateGpuHang). */
		void SimulateHang() { simulateHang = true; }

	private:
		std::jthread watchdog;  // last member: stopped and joined before the objects it reads are destroyed
	};
}
