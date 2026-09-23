#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "BufferPool.h"
#include "FrameTypes.h"
#include "MeshCache.h"
#include "RT.h"
#include "Scene.h"
#include "SharedTexture.h"
#include "SkinnedMeshes.h"
#include "SunShadows.h"

namespace RT
{
	/** @brief Counter slots written by RayQueryDebugCS.hlsl (keep in sync). */
	enum TraceCounter : uint32_t
	{
		kRenderPixels,
		kSky,
		kOutsideLoaded,
		kExcluded,
		kCounted,
		kMatched,
		kTracedNearer,
		kTracedFarther,
		kTracedMiss,
		kExcludedAlpha,
		kExcludedClutter,
		kAlphaTestedCounted,  ///< M7c: counted pixels whose traced hit passed the alpha test
		kAlphaTestedMatched,
		kExcludedWind,  ///< M7c: mismatches on wind-animated foliage (TREE_ANIM sway isn't in the TLAS)
		kCounterCount
	};

	enum class DebugView : uint32_t
	{
		kDepth,
		kInstance,
		kNormal,
		kDiff,
		kCount
	};

	struct TraceStats
	{
		bool haveResult = false;
		std::array<uint32_t, kCounterCount> counters{};
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t instances = 0;
		uint32_t exclusions = 0;
		uint32_t instancesDropped = 0;  ///< over kMaxInstances
		TimingSeries blasBuildMs;
		TimingSeries tlasBuildMs;  ///< exclusion BLAS + TLAS
		TimingSeries traceMs;
		TimingSeries mismatchPercent;
		TimingSeries coveragePercent;
		LoadedArea area;
		RE::NiPoint3 posAdjust;

		float MismatchPercent() const { return counters[kCounted] ? 100.0f * (counters[kCounted] - counters[kMatched]) / counters[kCounted] : 0.0f; }
		float CoveragePercent() const
		{
			const uint32_t candidates = counters[kRenderPixels] - counters[kSky];
			return candidates ? 100.0f * counters[kCounted] / candidates : 0.0f;
		}
	};

	/**
	 * @brief M4 ray tracing: BLAS/TLAS management and the inline-RayQuery debug trace with the depth-mismatch metric.
	 * All work is recorded into the sidecar's per-frame command list between the D3D11 -> D3D12 and D3D12 -> D3D11 fences.
	 */
	class Raytracer
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kMaxInstances = 65536;
		static constexpr uint32_t kMaxExclusions = 32768;
		static constexpr uint32_t kMeshPageSlots = 64;  // must match MeshPages[] in the shader
		static constexpr uint64_t kScratchBytes = 128ull << 20;
		static constexpr uint64_t kUploadSlotBytes = 16ull << 20;
		static constexpr float kMismatchThreshold = 0.01f;
		static constexpr float kClutterHeight = 150.0f;  ///< grass / ground clutter height above terrain (game units)

		/** @param a_alphaAtlas M7c alpha atlas (shared, resting in COMMON), or nullptr: then nothing is alpha-tested. */
		bool Init(ID3D12Device5* a_device, ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, uint32_t a_screenWidth, uint32_t a_screenHeight,
			ID3D12Resource* a_alphaAtlas);
		const std::string& GetFailureReason() const { return failureReason; }
		void SetTimestampFrequency(uint64_t a_frequency);

		/**
		 * @brief D3D11 side, before the fence signal: copy this frame's scene depth into the shared texture and, on
		 * comparison frames, the game's shadow mask.
		 */
		void CopyInputs(bool a_compareShadowMap);

		/**
		 * @brief Records BLAS builds and the TLAS build, then the M4 debug trace (a_debugTrace) and the M5 sun shadows
		 * (a_shadows non-null), with their result readbacks, into a_list.
		 */
		void Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, uint64_t a_frame, MeshCache& a_cache,
			const std::vector<GeometryCandidate>& a_candidates, const SkinnedScene& a_skinned, const std::vector<ExclusionBound>& a_exclusions,
			const LoadedArea& a_area, const FrameCamera& a_camera, bool a_debugTrace, const SunShadowParams* a_shadows,
			bool a_compareShadowMap, bool a_captureDump);

		/** @brief Reads the slot's counters and timestamps once its fence value has completed (never waits). */
		void CollectResults(uint32_t a_slot);

		/** @brief Copies the captured dump images out of the readback buffers (call once the dump's fence completed). */
		void ReadDumpImages(std::vector<DumpImage>& a_out);

		ID3D11ShaderResourceView* GetViewSRV(DebugView a_view) const { return views[static_cast<uint32_t>(a_view)].srv11.get(); }
		uint32_t GetTextureWidth() const { return width; }
		uint32_t GetTextureHeight() const { return height; }
		const TraceStats& GetStats() const { return stats; }
		/** @brief This frame's TLAS (valid after Record, until the next frame's Record). */
		D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress() const { return tlas->GetGPUVirtualAddress(); }
		/** @brief The instance data (InstanceGpu[]) Record wrote for a_slot. */
		D3D12_GPU_VIRTUAL_ADDRESS GetInstanceDataAddress(uint32_t a_slot) const;
		/** @brief The shared R32 copy of the scene depth CopyInputs wrote (resting in COMMON). */
		ID3D12Resource* GetRasterDepth() const { return rasterDepth.resource12.get(); }
		ID3D11ComputeShader* GetCopyDepthShader() const { return copyDepthCS.get(); }
		bool SunShadowsReady() const { return sunShadowsReady; }
		const SkinnedMeshes* GetSkinned() const { return skinnedReady ? &skinned : nullptr; }
		SunShadows& GetSunShadows() { return sunShadows; }
		const SunShadows& GetSunShadows() const { return sunShadows; }

	private:
		bool CreatePipeline();
		void RecordDebugTrace(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, MeshCache& a_cache, uint32_t a_instanceCount, uint32_t a_exclusionCount,
			const LoadedArea& a_area, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, bool a_captureDump);
		void UpdatePageDescriptors(const BufferPool& a_pool);
		bool Fail(std::string a_reason);

		ID3D12Device5* device = nullptr;
		ID3D11Device5* d3d11Device = nullptr;
		ID3D11DeviceContext4* d3d11Context = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;

		SharedTexture rasterDepth;
		std::array<SharedTexture, static_cast<size_t>(DebugView::kCount)> views;
		winrt::com_ptr<ID3D11ComputeShader> copyDepthCS;

		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;  // [0] raster depth SRV, [1..64] mesh pages, [65..68] view UAVs, [69] alpha atlas
		ID3D12Resource* alphaAtlas = nullptr;
		uint32_t descriptorSize = 0;
		std::array<uint64_t, kMeshPageSlots> describedPageSerials{};  // BufferPool page serial each descriptor describes

		winrt::com_ptr<ID3D12Resource> scratch;
		winrt::com_ptr<ID3D12Resource> tlas;
		winrt::com_ptr<ID3D12Resource> exclusionBlas;
		uint64_t tlasScratchBytes = 0;
		uint64_t exclusionScratchBytes = 0;

		winrt::com_ptr<ID3D12Resource> uploads[kFramesInFlight];  // instance descs | instance data | AABBs | constants
		uint8_t* uploadCpu[kFramesInFlight]{};

		winrt::com_ptr<ID3D12Resource> counters;
		winrt::com_ptr<ID3D12Resource> countersReadback;  // kFramesInFlight slots
		const uint32_t* countersCpu = nullptr;
		winrt::com_ptr<ID3D12QueryHeap> timestamps;  // 4 per slot
		winrt::com_ptr<ID3D12Resource> timestampReadback;
		const uint64_t* timestampCpu = nullptr;
		uint64_t timestampFrequency = 0;
		bool slotPending[kFramesInFlight]{};
		struct SlotInfo
		{
			bool debugTraced = false;
			uint32_t instances = 0;
			uint32_t exclusions = 0;
			uint32_t dropped = 0;
			uint32_t renderWidth = 0;
			uint32_t renderHeight = 0;
			LoadedArea area;
			RE::NiPoint3 posAdjust;
		} slotInfo[kFramesInFlight];

		winrt::com_ptr<ID3D12Resource> dumpReadback;
		uint32_t dumpRowPitch = 0;
		uint32_t dumpWidth = 0;
		uint32_t dumpHeight = 0;
		bool dumpCaptured = false;

		SunShadows sunShadows;
		bool sunShadowsReady = false;
		SkinnedMeshes skinned;
		bool skinnedReady = false;

		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
		std::vector<InstanceRecord> instances;
		uint64_t resultsCollected = 0;

		TraceStats stats;
		std::string failureReason;
	};
}
