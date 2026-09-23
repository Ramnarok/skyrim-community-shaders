#pragma once

#include <d3d12.h>
#include <winrt/base.h>

#include "BufferPool.h"
#include "MeshCache.h"
#include "RT.h"
#include "Scene.h"

namespace RT
{
	struct SkinnedStats
	{
		uint32_t instances = 0;          ///< skinned partitions in the TLAS this frame
		uint32_t skinnedLastFrame = 0;   ///< partitions skinned this frame
		uint32_t dynamicPartitions = 0;  ///< M7b: of those, with BSDynamicTriShape positions
		uint32_t dynamicUploadsLastFrame = 0;
		uint64_t dynamicUploadBytesLastFrame = 0;
		uint32_t dynamicWaiting = 0;     ///< never uploaded yet (budget): left out this frame
		uint32_t waitingForMesh = 0;     ///< bind-pose data not resident yet
		uint32_t blasBuiltLastFrame = 0;
		uint32_t blasRefitLastFrame = 0;
		uint32_t blasSkippedScratch = 0;  ///< builds/refits deferred (scratch budget): stale pose this frame
		uint32_t entries = 0;
		uint64_t verticesLastFrame = 0;
		uint64_t outputBytes = 0;
		uint64_t blasBytes = 0;
		uint64_t totalBuilt = 0;
		uint64_t failed = 0;
		TimingSeries skinMs;  ///< GPU: skinning dispatches + BLAS builds/refits
	};

	/**
	 * @brief M7 skinned geometry: per animated partition (skin instance x partition) a D3D12 compute pass skins the
	 * cached bind-pose vertices with this frame's bone palette into a float3 buffer, and the partition's BLAS is refit
	 * from it (full rebuild every kRebuildInterval frames). Instances use an identity transform: the positions are
	 * already camera-relative world space.
	 */
	class SkinnedMeshes
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint64_t kPageBytes = 64ull << 20;
		static constexpr uint64_t kOutputBudgetBytes = 1024ull << 20;
		static constexpr uint64_t kBlasBudgetBytes = 1024ull << 20;
		static constexpr uint64_t kUploadBytes = 8ull << 20;  ///< per slot: palettes
		static constexpr uint64_t kDynamicUploadBytes = 8ull << 20;  ///< per slot: changed BSDynamicTriShape positions
		static constexpr uint64_t kDynamicBudgetBytes = 256ull << 20;
		static constexpr uint64_t kEvictAfterFrames = 120;
		static constexpr uint64_t kRebuildInterval = 60;
		/// Mesh-page descriptor slots: the mesh pool uses [0, kFirstPageSlot), skinned outputs use the rest.
		static constexpr uint32_t kFirstPageSlot = 48;
		static constexpr uint32_t kPageSlots = 16;

		bool Init(ID3D12Device5* a_device);
		void SetTimestampFrequency(uint64_t a_frequency) { timestampFrequency = a_frequency; }

		/**
		 * @brief Records skinning and BLAS builds/refits for this frame's skinned partitions (before the TLAS build; the
		 * caller issues the UAV barrier). Scratch comes from the caller's range, starting at a_scratchUsed.
		 */
		void Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, uint64_t a_frame, const MeshCache& a_cache,
			const std::vector<GeometryCandidate>& a_candidates, const SkinnedScene& a_scene, const RE::NiPoint3& a_posAdjust,
			D3D12_GPU_VIRTUAL_ADDRESS a_scratch, uint64_t a_scratchBytes, uint64_t& a_scratchUsed);

		/** @brief Appends this frame's skinned instances (those with a BLAS) to a_out. */
		void GatherInstances(const std::vector<GeometryCandidate>& a_candidates, const SkinnedScene& a_scene,
			const RE::NiPoint3& a_posAdjust, std::vector<InstanceRecord>& a_out) const;

		void CollectResults(uint32_t a_slot);

		const BufferPool& GetOutputPool() const { return outputPool; }
		const SkinnedStats& GetStats() const { return stats; }

	private:
		struct Key
		{
			const void* skinInstance = nullptr;
			uint32_t partition = 0;
			bool operator==(const Key&) const = default;
		};
		struct KeyHash
		{
			using is_avalanching = void;
			uint64_t operator()(const Key& a_key) const noexcept;
		};
		struct Entry
		{
			PoolAllocation output;
			PoolAllocation blas;
			PoolAllocation dynamic;  // M7b positions
			uint32_t dynamicVersion = 0;
			bool dynamicValid = false;
			ResidentMesh source;
			uint64_t lastSeenFrame = 0;
			uint64_t builtFrame = 0;
			uint64_t updateScratchBytes = 0;
			uint64_t buildScratchBytes = 0;
			bool built = false;
			bool failed = false;
		};

		void Release(Entry& a_entry);

		ID3D12Device5* device = nullptr;
		BufferPool outputPool;  // float3 skinned positions, rests in NON_PIXEL_SHADER_RESOURCE
		BufferPool blasPool;
		BufferPool dynamicPool;  // M7b positions, rests in NON_PIXEL_SHADER_RESOURCE
		winrt::com_ptr<ID3D12Resource> dynamicUploads[kFramesInFlight];
		uint8_t* dynamicUploadCpu[kFramesInFlight]{};
		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Resource> uploads[kFramesInFlight];
		uint8_t* uploadCpu[kFramesInFlight]{};
		winrt::com_ptr<ID3D12QueryHeap> timestamps;
		winrt::com_ptr<ID3D12Resource> timestampReadback;
		const uint64_t* timestampCpu = nullptr;
		uint64_t timestampFrequency = 0;
		bool slotPending[kFramesInFlight]{};

		ankerl::unordered_dense::map<Key, Entry, KeyHash> entries;
		std::vector<std::pair<uint64_t, Entry>> retired;  // (frame replaced, allocations to free later)
		std::vector<Key> visible;  // this frame's keys, aligned with SkinnedScene::partitions
		SkinnedStats stats;
		bool ready = false;
	};
}
