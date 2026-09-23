#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <deque>
#include <winrt/base.h>

#include "BufferPool.h"
#include "RT.h"
#include "Scene.h"

namespace RT
{
	/** @brief One vertex format seen in the scene, with three independent stride measurements. */
	struct VertexFormatStats
	{
		uint16_t flags = 0;           ///< VertexDesc::GetFlags()
		uint32_t strideFromDesc = 0;  ///< (desc & 0xF) * 4, the nifskope layout
		uint32_t strideFromGetSize = 0;  ///< CommonLib VertexDesc::GetSize()
		uint32_t vbBytesPerVertex = 0;   ///< game VB ByteWidth / vertexCount (ground truth)
		uint32_t meshes = 0;
	};

	struct MeshCacheStats
	{
		uint32_t entries = 0;
		uint32_t resident = 0;
		uint32_t pending = 0;  ///< queued, readback in flight or upload in flight
		uint64_t residentVertexBytes = 0;
		uint64_t residentIndexBytes = 0;
		uint64_t poolBytes = 0;  ///< GPU memory reserved by the mesh pool pages
		uint32_t poolPages = 0;

		uint32_t uploadsLastFrame = 0;
		uint32_t evictionsLastFrame = 0;
		uint32_t readbacksLastFrame = 0;
		uint64_t uploadBytesLastFrame = 0;
		TimingSeries uploadsPerFrame;  ///< counts, reusing the windowed series
		TimingSeries evictionsPerFrame;
		TimingSeries uploadMBPerFrame;
		TimingSeries updateMs;

		uint64_t totalUploads = 0;
		uint64_t totalEvictions = 0;
		uint64_t totalReadbacks = 0;
		uint64_t totalFailed = 0;
		uint64_t totalDeferred = 0;  ///< uploads postponed by the per-frame budget or a full ring

		uint32_t sourceRawCpu = 0;       ///< meshes uploaded from TriShape::rawVertexData/rawIndexData
		uint32_t sourceD3D11Readback = 0;  ///< meshes uploaded from a D3D11 staging readback
		uint32_t rawBoth = 0, rawVertexOnly = 0, rawIndexOnly = 0, rawNeither = 0;  ///< per new mesh

		uint32_t strideMatchesVB = 0;
		uint32_t strideMismatchesVB = 0;
		uint32_t indexBytesMatchIB = 0;
		uint32_t indexBytesMismatchIB = 0;

		uint32_t rawCompared = 0;  ///< raw CPU copy vs GPU readback, byte for byte
		uint32_t rawMatched = 0;
		uint32_t rawMismatched = 0;
		uint32_t rawCopyFaults = 0;  ///< access violations while copying raw data (caught)

		// M4 bottom-level acceleration structures.
		uint32_t blasBuilt = 0;       ///< entries that currently have a BLAS
		uint32_t blasPending = 0;     ///< resident entries still waiting for a BLAS
		uint32_t blasBuiltLastFrame = 0;
		uint64_t blasTotalBuilt = 0;
		uint64_t blasFailed = 0;
		uint64_t asPoolBytes = 0;
		uint64_t blasBytes = 0;

		std::vector<VertexFormatStats> formats;
	};

	/** @brief One TLAS instance for this frame, resolved from a scene candidate. */
	struct InstanceRecord
	{
		D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
		RE::NiTransform world;
		uint32_t vertexPage = 0;
		uint32_t vertexOffset = 0;
		uint32_t indexPage = 0;
		uint32_t indexOffset = 0;
		uint32_t stride = 0;
		bool terrain = false;
		bool alphaTested = false;
		bool alphaBlended = false;
	};

	/**
	 * @brief Static-mesh cache: one D3D12 copy (and BLAS) of each unique mesh, uploaded on a per-frame budget
	 * and evicted when unseen. Buffers can't be shared between the devices (M2 spike), so data comes from the
	 * game's CPU copy when present, else from a D3D11 staging readback polled without CPU waits.
	 */
	class MeshCache
	{
	public:
		static constexpr uint64_t kPageBytes = 64ull << 20;
		static constexpr uint64_t kPoolBudgetBytes = 2048ull << 20;
		static constexpr uint64_t kASPoolBudgetBytes = 2048ull << 20;
		static constexpr uint64_t kUploadRingBytes = 32ull << 20;
		static constexpr uint64_t kMaxUploadBytesPerFrame = 8ull << 20;
		static constexpr uint32_t kMaxNewMeshesPerFrame = 128;
		static constexpr uint32_t kMaxReadbacksPerFrame = 16;
		static constexpr uint64_t kEvictAfterFrames = 120;
		static constexpr uint32_t kRawVerifySamples = 16;
		static constexpr uint32_t kMaxBlasBuildsPerFrame = 256;
		static constexpr uint64_t kMaxBlasTrianglesPerFrame = 2'000'000;

		bool Init(ID3D12Device5* a_device, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context);

		/**
		 * @brief Per-frame update. Records copy commands into a_list; they complete at a_submitFence.
		 * @return True if any commands were recorded (the caller must then execute and signal a_submitFence).
		 */
		bool Update(const std::vector<GeometryCandidate>& a_candidates, uint64_t a_frame, uint64_t a_completedFence, ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence);

		/**
		 * @brief Records BLAS builds for resident meshes seen this frame that don't have one yet.
		 * Each build gets its own scratch range starting at a_scratchUsed; the caller issues the UAV barrier.
		 */
		void BuildBLASes(ID3D12GraphicsCommandList4* a_list, uint64_t a_frame, D3D12_GPU_VIRTUAL_ADDRESS a_scratch, uint64_t a_scratchBytes, uint64_t& a_scratchUsed);

		/** @brief Resolves this frame's candidates to instances whose mesh has a BLAS. */
		void GatherInstances(const std::vector<GeometryCandidate>& a_candidates, std::vector<InstanceRecord>& a_out) const;

		const BufferPool& GetMeshPool() const { return pool; }
		const MeshCacheStats& GetStats() const { return stats; }

	private:
		/** @brief UPLOAD-heap ring; space is reclaimed once the fence value of the frame that used it completes. */
		class UploadRing
		{
		public:
			bool Init(ID3D12Device* a_device, uint64_t a_bytes);
			uint8_t* Allocate(uint64_t a_bytes, uint64_t& a_offset);
			void Submit(uint64_t a_fence);
			void Retire(uint64_t a_completedFence);
			ID3D12Resource* GetResource() const { return buffer.get(); }

		private:
			winrt::com_ptr<ID3D12Resource> buffer;
			uint8_t* cpu = nullptr;
			uint64_t size = 0;
			uint64_t head = 0;  // monotonic bytes allocated
			uint64_t tail = 0;  // monotonic bytes retired
			uint64_t submittedHead = 0;
			std::deque<std::pair<uint64_t, uint64_t>> inFlight;  // (head at submit, fence)
		};

		struct MeshKey
		{
			const void* rendererData = nullptr;
			const void* vertexBuffer = nullptr;
			uint64_t vertexDesc = 0;
			uint32_t vertexCount = 0;
			uint32_t triangleCount = 0;
			bool operator==(const MeshKey&) const = default;
		};
		struct MeshKeyHash
		{
			using is_avalanching = void;
			uint64_t operator()(const MeshKey& a_key) const noexcept;
		};
		static MeshKey MakeKey(const GeometryCandidate& a_candidate);

		enum class State : uint8_t
		{
			kQueued,
			kReadbackInFlight,
			kUploadInFlight,
			kResident,
			kFailed
		};

		struct MeshEntry
		{
			State state = State::kQueued;
			bool terrain = false;
			bool queued = false;
			uint64_t lastSeenFrame = 0;
			uint64_t uploadFence = 0;
			uint32_t stride = 0;
			uint32_t vertexCount = 0;
			uint32_t triangleCount = 0;
			uint64_t vertexBytes = 0;
			uint64_t indexBytes = 0;
			PoolAllocation vertexAllocation;
			PoolAllocation indexAllocation;
			PoolAllocation blasAllocation;
			bool blasFailed = false;
			// D3D11 staging readback (readback source, or verification of a raw upload).
			winrt::com_ptr<ID3D11Buffer> stagingVB;
			winrt::com_ptr<ID3D11Buffer> stagingIB;
			winrt::com_ptr<ID3D11Query> readbackDone;
			bool verifyOnly = false;
			std::vector<uint8_t> rawCopy;  // raw VB + IB bytes, kept only for verification samples
		};

		enum class UploadResult : uint8_t
		{
			kOk,
			kRingFull,     // retry next frame
			kCopyFault,    // source memory unreadable
			kOutOfMemory,  // pool budget exhausted
		};

		bool StartRawUpload(const MeshKey& a_key, MeshEntry& a_entry, ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence, uint64_t& a_budgetBytes);
		bool StartReadback(const MeshKey& a_key, MeshEntry& a_entry);
		UploadResult UploadBytes(MeshEntry& a_entry, const uint8_t* a_vertices, const uint8_t* a_indices, ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence);
		void PollReadbacks(ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence, uint64_t& a_budgetBytes, bool& a_recorded);
		void Release(MeshEntry& a_entry);

		ID3D12Device5* device = nullptr;
		ID3D11Device* d3d11Device = nullptr;
		ID3D11DeviceContext* d3d11Context = nullptr;
		BufferPool pool;    // vertex + index data
		BufferPool asPool;  // bottom-level acceleration structures
		UploadRing ring;
		ankerl::unordered_dense::map<MeshKey, MeshEntry, MeshKeyHash> entries;
		std::deque<MeshKey> queue;
		std::vector<MeshKey> readbacks;
		std::map<uint16_t, VertexFormatStats> formats;
		MeshCacheStats stats;
	};
}
