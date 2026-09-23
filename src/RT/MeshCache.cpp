#include "MeshCache.h"

namespace RT
{
	namespace
	{
		constexpr uint64_t kRingAlignment = 16;

		uint64_t AlignUp(uint64_t a_value, uint64_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		// The size of the game's raw CPU allocations isn't known, so copies from them are guarded.
		// Kept free of C++ objects so __try is allowed.
		bool SafeCopy(void* a_dst, const void* a_src, size_t a_bytes)
		{
			__try {
				std::memcpy(a_dst, a_src, a_bytes);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		::ID3D11Buffer* AsD3D11(RE::ID3D11Buffer* a_buffer)
		{
			// CommonLib forward-declares its own RE::ID3D11Buffer; it is the same COM object.
			return reinterpret_cast<::ID3D11Buffer*>(a_buffer);
		}

		uint32_t ByteWidth(::ID3D11Buffer* a_buffer)
		{
			D3D11_BUFFER_DESC desc{};
			a_buffer->GetDesc(&desc);
			return desc.ByteWidth;
		}
	}

	// ----------------------------------------------------------------------------------------------
	// UploadRing

	bool MeshCache::UploadRing::Init(ID3D12Device* a_device, uint64_t a_bytes)
	{
		D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = a_bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.put()))))
			return false;
		buffer->SetName(L"SkyrimRT::UploadRing");
		D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		if (FAILED(buffer->Map(0, &noRead, &mapped)))
			return false;
		cpu = static_cast<uint8_t*>(mapped);
		size = a_bytes;
		return true;
	}

	uint8_t* MeshCache::UploadRing::Allocate(uint64_t a_bytes, uint64_t& a_offset)
	{
		const uint64_t bytes = AlignUp(a_bytes, kRingAlignment);
		if (bytes > size)
			return nullptr;
		uint64_t position = head % size;
		if (position + bytes > size) {
			const uint64_t pad = size - position;  // don't split an allocation across the wrap
			if (head + pad + bytes - tail > size)
				return nullptr;
			head += pad;
			position = 0;
		}
		if (head + bytes - tail > size)
			return nullptr;
		a_offset = position;
		head += bytes;
		return cpu + position;
	}

	void MeshCache::UploadRing::Submit(uint64_t a_fence)
	{
		if (head != submittedHead) {
			inFlight.emplace_back(head, a_fence);
			submittedHead = head;
		}
	}

	void MeshCache::UploadRing::Retire(uint64_t a_completedFence)
	{
		while (!inFlight.empty() && inFlight.front().second <= a_completedFence) {
			tail = inFlight.front().first;
			inFlight.pop_front();
		}
	}

	// ----------------------------------------------------------------------------------------------
	// MeshCache

	uint64_t MeshCache::MeshKeyHash::operator()(const MeshKey& a_key) const noexcept
	{
		static_assert(sizeof(MeshKey) == 32, "MeshKey must have no padding to be hashed as bytes");
		return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key));
	}

	MeshCache::MeshKey MeshCache::MakeKey(const GeometryCandidate& a_candidate)
	{
		return { a_candidate.rendererData, a_candidate.rendererData->vertexBuffer, a_candidate.vertexDesc, a_candidate.vertexCount, a_candidate.triangleCount };
	}

	bool MeshCache::Init(ID3D12Device5* a_device, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context)
	{
		device = a_device;
		d3d11Device = a_d3d11Device;
		d3d11Context = a_d3d11Context;
		pool.Init(a_device, kPageBytes, kPoolBudgetBytes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, L"SkyrimRT::MeshPage");
		asPool.Init(a_device, kPageBytes, kASPoolBudgetBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"SkyrimRT::BLASPage");
		return ring.Init(a_device, kUploadRingBytes);
	}

	void MeshCache::Release(MeshEntry& a_entry)
	{
		pool.Free(a_entry.vertexAllocation);
		pool.Free(a_entry.indexAllocation);
		asPool.Free(a_entry.blasAllocation);
		a_entry.stagingVB = nullptr;
		a_entry.stagingIB = nullptr;
		a_entry.readbackDone = nullptr;
		a_entry.rawCopy.clear();
	}

	MeshCache::UploadResult MeshCache::UploadBytes(MeshEntry& a_entry, const uint8_t* a_vertices, const uint8_t* a_indices, ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence)
	{
		// Caller has checked the byte budget; this stages, allocates and records.
		uint64_t ringOffset = 0;
		uint8_t* dst = ring.Allocate(a_entry.vertexBytes + a_entry.indexBytes, ringOffset);
		if (!dst)
			return UploadResult::kRingFull;
		if (!SafeCopy(dst, a_vertices, a_entry.vertexBytes) || !SafeCopy(dst + a_entry.vertexBytes, a_indices, a_entry.indexBytes)) {
			stats.rawCopyFaults++;
			return UploadResult::kCopyFault;
		}
		if (!pool.Allocate(a_entry.vertexBytes, a_entry.vertexAllocation) || !pool.Allocate(a_entry.indexBytes, a_entry.indexAllocation)) {
			pool.Free(a_entry.vertexAllocation);
			pool.Free(a_entry.indexAllocation);
			return UploadResult::kOutOfMemory;
		}

		// DEFAULT buffers in COMMON promote to COPY_DEST implicitly and decay back after execution.
		a_list->CopyBufferRegion(pool.GetResource(a_entry.vertexAllocation.page), a_entry.vertexAllocation.offset, ring.GetResource(), ringOffset, a_entry.vertexBytes);
		a_list->CopyBufferRegion(pool.GetResource(a_entry.indexAllocation.page), a_entry.indexAllocation.offset, ring.GetResource(), ringOffset + a_entry.vertexBytes, a_entry.indexBytes);

		a_entry.state = State::kUploadInFlight;
		a_entry.uploadFence = a_submitFence;
		stats.uploadsLastFrame++;
		stats.uploadBytesLastFrame += a_entry.vertexBytes + a_entry.indexBytes;
		stats.totalUploads++;
		return UploadResult::kOk;
	}

	bool MeshCache::StartRawUpload(const MeshKey& a_key, MeshEntry& a_entry, ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence, uint64_t& a_budgetBytes)
	{
		const auto* rendererData = static_cast<const RE::BSGraphics::TriShape*>(a_key.rendererData);
		const uint64_t total = a_entry.vertexBytes + a_entry.indexBytes;
		if (total > a_budgetBytes && total <= kMaxUploadBytesPerFrame)
			return false;  // wait for next frame's budget; oversized meshes go through alone

		const auto* vertices = rendererData->rawVertexData;
		const auto* indices = reinterpret_cast<const uint8_t*>(rendererData->rawIndexData);
		switch (UploadBytes(a_entry, vertices, indices, a_list, a_submitFence)) {
		case UploadResult::kOk:
			break;
		case UploadResult::kRingFull:
			return false;  // retry next frame
		case UploadResult::kCopyFault:
			// The raw pointer was bad: fall back to the GPU copy.
			if (!StartReadback(a_key, a_entry)) {
				a_entry.state = State::kFailed;
				stats.totalFailed++;
			}
			return true;
		case UploadResult::kOutOfMemory:
			a_entry.state = State::kFailed;
			stats.totalFailed++;
			return true;
		}
		a_budgetBytes -= std::min(a_budgetBytes, total);
		stats.sourceRawCpu++;

		// Verify the first few raw uploads against the GPU buffers, byte for byte.
		if (stats.rawCompared + static_cast<uint32_t>(std::ranges::count_if(readbacks, [&](const MeshKey& k) { auto it = entries.find(k); return it != entries.end() && it->second.verifyOnly; })) < kRawVerifySamples) {
			a_entry.rawCopy.resize(total);
			SafeCopy(a_entry.rawCopy.data(), vertices, a_entry.vertexBytes);
			SafeCopy(a_entry.rawCopy.data() + a_entry.vertexBytes, indices, a_entry.indexBytes);
			a_entry.verifyOnly = true;
			StartReadback(a_key, a_entry);
		}
		return true;
	}

	bool MeshCache::StartReadback(const MeshKey& a_key, MeshEntry& a_entry)
	{
		const auto* rendererData = static_cast<const RE::BSGraphics::TriShape*>(a_key.rendererData);
		auto* vertexBuffer = AsD3D11(rendererData->vertexBuffer);
		auto* indexBuffer = AsD3D11(rendererData->indexBuffer);

		auto createStaging = [&](::ID3D11Buffer* a_source, winrt::com_ptr<ID3D11Buffer>& a_out) {
			D3D11_BUFFER_DESC desc{ .ByteWidth = ByteWidth(a_source), .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ };
			return SUCCEEDED(d3d11Device->CreateBuffer(&desc, nullptr, a_out.put()));
		};
		D3D11_QUERY_DESC queryDesc{ .Query = D3D11_QUERY_EVENT };
		if (!createStaging(vertexBuffer, a_entry.stagingVB) || !createStaging(indexBuffer, a_entry.stagingIB) ||
			FAILED(d3d11Device->CreateQuery(&queryDesc, a_entry.readbackDone.put()))) {
			a_entry.stagingVB = nullptr;
			a_entry.stagingIB = nullptr;
			a_entry.readbackDone = nullptr;
			return false;
		}

		// Queued this frame, while the game's buffers are known to be alive.
		d3d11Context->CopyResource(a_entry.stagingVB.get(), vertexBuffer);
		d3d11Context->CopyResource(a_entry.stagingIB.get(), indexBuffer);
		d3d11Context->End(a_entry.readbackDone.get());

		if (!a_entry.verifyOnly)
			a_entry.state = State::kReadbackInFlight;
		readbacks.push_back(a_key);
		stats.readbacksLastFrame++;
		stats.totalReadbacks++;
		return true;
	}

	void MeshCache::PollReadbacks(ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence, uint64_t& a_budgetBytes, bool& a_recorded)
	{
		for (size_t i = 0; i < readbacks.size();) {
			auto it = entries.find(readbacks[i]);
			if (it == entries.end()) {
				readbacks[i] = readbacks.back();
				readbacks.pop_back();
				continue;
			}
			auto& entry = it->second;

			BOOL done = FALSE;
			if (d3d11Context->GetData(entry.readbackDone.get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) {
				i++;
				continue;
			}

			const uint64_t total = entry.vertexBytes + entry.indexBytes;
			if (!entry.verifyOnly && total > a_budgetBytes && total <= kMaxUploadBytesPerFrame) {
				stats.totalDeferred++;
				i++;
				continue;
			}

			D3D11_MAPPED_SUBRESOURCE vertices{}, indices{};
			if (FAILED(d3d11Context->Map(entry.stagingVB.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &vertices))) {
				i++;
				continue;
			}
			if (FAILED(d3d11Context->Map(entry.stagingIB.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &indices))) {
				d3d11Context->Unmap(entry.stagingVB.get(), 0);
				i++;
				continue;
			}

			bool finished = true;
			if (entry.verifyOnly) {
				const bool match = std::memcmp(entry.rawCopy.data(), vertices.pData, entry.vertexBytes) == 0 &&
				                   std::memcmp(entry.rawCopy.data() + entry.vertexBytes, indices.pData, entry.indexBytes) == 0;
				stats.rawCompared++;
				(match ? stats.rawMatched : stats.rawMismatched)++;
				entry.verifyOnly = false;
				entry.rawCopy.clear();
			} else {
				switch (UploadBytes(entry, static_cast<const uint8_t*>(vertices.pData), static_cast<const uint8_t*>(indices.pData), a_list, a_submitFence)) {
				case UploadResult::kOk:
					a_budgetBytes -= std::min(a_budgetBytes, total);
					a_recorded = true;
					stats.sourceD3D11Readback++;
					break;
				case UploadResult::kRingFull:
					finished = false;  // keep the staging copy, retry next frame
					stats.totalDeferred++;
					break;
				case UploadResult::kCopyFault:
				case UploadResult::kOutOfMemory:
					entry.state = State::kFailed;
					stats.totalFailed++;
					break;
				}
			}

			d3d11Context->Unmap(entry.stagingIB.get(), 0);
			d3d11Context->Unmap(entry.stagingVB.get(), 0);

			if (!finished) {
				i++;
				continue;
			}
			entry.stagingVB = nullptr;
			entry.stagingIB = nullptr;
			entry.readbackDone = nullptr;
			readbacks[i] = readbacks.back();
			readbacks.pop_back();
		}
	}

	bool MeshCache::Update(const std::vector<GeometryCandidate>& a_candidates, uint64_t a_frame, uint64_t a_completedFence, ID3D12GraphicsCommandList* a_list, uint64_t a_submitFence)
	{
		LARGE_INTEGER start, end, frequency;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);

		stats.uploadsLastFrame = 0;
		stats.evictionsLastFrame = 0;
		stats.readbacksLastFrame = 0;
		stats.uploadBytesLastFrame = 0;

		ring.Retire(a_completedFence);
		for (auto& [key, entry] : entries) {
			if (entry.state == State::kUploadInFlight && entry.uploadFence <= a_completedFence)
				entry.state = State::kResident;
		}

		// 1. Mark every mesh seen this frame; register new ones.
		for (const auto& candidate : a_candidates) {
			auto* rendererData = candidate.rendererData;
			const MeshKey key = MakeKey(candidate);
			auto [it, inserted] = entries.try_emplace(key);
			auto& entry = it->second;
			entry.lastSeenFrame = a_frame;
			if (!inserted)
				continue;

			entry.terrain = candidate.terrain;
			entry.skinned = candidate.skinned;
			entry.vertexCount = candidate.vertexCount;
			entry.triangleCount = candidate.triangleCount;

			// Three independent stride measurements (M3 acceptance: verify the VertexDesc bits).
			auto desc = rendererData->vertexDesc;  // GetSize() is non-const
			const uint16_t flags = static_cast<uint16_t>(desc.GetFlags());
			const uint32_t strideFromDesc = static_cast<uint32_t>(candidate.vertexDesc & 0xF) * 4;
			const uint32_t vbBytes = ByteWidth(AsD3D11(rendererData->vertexBuffer));
			const uint32_t ibBytes = ByteWidth(AsD3D11(rendererData->indexBuffer));
			const uint32_t vbPerVertex = vbBytes % candidate.vertexCount == 0 ? vbBytes / candidate.vertexCount : 0;

			auto& format = formats[flags];
			if (format.meshes++ == 0)
				format = { flags, strideFromDesc, desc.GetSize(), vbPerVertex, 1 };

			entry.stride = strideFromDesc;
			const uint64_t expectedVB = static_cast<uint64_t>(strideFromDesc) * candidate.vertexCount;
			(expectedVB == vbBytes ? stats.strideMatchesVB : stats.strideMismatchesVB)++;
			entry.vertexBytes = expectedVB && expectedVB <= vbBytes ? expectedVB : vbBytes;

			const uint64_t expectedIB = static_cast<uint64_t>(candidate.triangleCount) * 3 * sizeof(uint16_t);
			(expectedIB == ibBytes ? stats.indexBytesMatchIB : stats.indexBytesMismatchIB)++;
			entry.indexBytes = expectedIB <= ibBytes ? expectedIB : ibBytes;

			const bool rawV = rendererData->rawVertexData != nullptr;
			const bool rawI = rendererData->rawIndexData != nullptr;
			(rawV && rawI ? stats.rawBoth : rawV ? stats.rawVertexOnly : rawI ? stats.rawIndexOnly : stats.rawNeither)++;

			entry.queued = true;
			queue.push_back(key);
		}

		bool recorded = false;
		uint64_t budget = kMaxUploadBytesPerFrame;

		// 2. Finish readbacks that completed on the D3D11 GPU (polled, never waited on).
		PollReadbacks(a_list, a_submitFence, budget, recorded);

		// 3. Start new uploads within the budget. Game data is only read for meshes seen this frame.
		uint32_t started = 0;
		uint32_t readbacksStarted = 0;
		for (size_t n = queue.size(); n > 0 && started < kMaxNewMeshesPerFrame && budget > 0; n--) {
			const MeshKey key = queue.front();
			queue.pop_front();
			auto it = entries.find(key);
			if (it == entries.end() || it->second.state != State::kQueued) {
				continue;
			}
			auto& entry = it->second;
			if (entry.lastSeenFrame != a_frame) {
				queue.push_back(key);
				continue;
			}

			const auto* rendererData = static_cast<const RE::BSGraphics::TriShape*>(key.rendererData);
			if (rendererData->rawVertexData && rendererData->rawIndexData) {
				if (!StartRawUpload(key, entry, a_list, a_submitFence, budget)) {
					queue.push_back(key);
					stats.totalDeferred++;
					continue;
				}
				recorded |= entry.state == State::kUploadInFlight;
			} else {
				if (readbacksStarted >= kMaxReadbacksPerFrame) {
					queue.push_back(key);
					stats.totalDeferred++;
					continue;
				}
				if (!StartReadback(key, entry)) {
					entry.state = State::kFailed;
					stats.totalFailed++;
				}
				readbacksStarted++;
			}
			entry.queued = false;
			started++;
		}

		// 4. Evict meshes unseen for kEvictAfterFrames, once no GPU work references them.
		for (auto it = entries.begin(); it != entries.end();) {
			auto& entry = it->second;
			const bool stale = entry.lastSeenFrame + kEvictAfterFrames < a_frame;
			const bool busy = entry.state == State::kReadbackInFlight || entry.state == State::kUploadInFlight || entry.readbackDone;
			if (!stale || busy) {
				++it;
				continue;
			}
			Release(entry);
			it = entries.erase(it);
			stats.evictionsLastFrame++;
			stats.totalEvictions++;
		}

		if (recorded)
			ring.Submit(a_submitFence);

		// Snapshot.
		stats.entries = static_cast<uint32_t>(entries.size());
		stats.resident = 0;
		stats.pending = 0;
		stats.residentVertexBytes = 0;
		stats.residentIndexBytes = 0;
		for (const auto& [key, entry] : entries) {
			if (entry.state == State::kResident) {
				stats.resident++;
				stats.residentVertexBytes += entry.vertexBytes;
				stats.residentIndexBytes += entry.indexBytes;
			} else if (entry.state != State::kFailed) {
				stats.pending++;
			}
		}
		stats.poolBytes = pool.GetReservedBytes();
		stats.poolPages = pool.GetPageCount();
		stats.blasBuilt = 0;
		stats.blasPending = 0;
		stats.blasBytes = 0;
		for (const auto& [key, entry] : entries) {
			if (entry.blasAllocation.IsValid()) {
				stats.blasBuilt++;
				stats.blasBytes += entry.blasAllocation.size;
			} else if (entry.state == State::kResident && !entry.blasFailed) {
				stats.blasPending++;
			}
		}
		stats.asPoolBytes = asPool.GetReservedBytes();
		stats.formats.clear();
		for (const auto& [flags, format] : formats)
			stats.formats.push_back(format);

		stats.uploadsPerFrame.Add(static_cast<float>(stats.uploadsLastFrame));
		stats.evictionsPerFrame.Add(static_cast<float>(stats.evictionsLastFrame));
		stats.uploadMBPerFrame.Add(static_cast<float>(stats.uploadBytesLastFrame) / (1024.0f * 1024.0f));
		QueryPerformanceCounter(&end);
		stats.updateMs.Add(static_cast<float>(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)));
		return recorded;
	}

	void MeshCache::BuildBLASes(ID3D12GraphicsCommandList4* a_list, uint64_t a_frame, D3D12_GPU_VIRTUAL_ADDRESS a_scratch, uint64_t a_scratchBytes, uint64_t& a_scratchUsed)
	{
		stats.blasBuiltLastFrame = 0;
		uint64_t triangles = 0;

		for (auto& [key, entry] : entries) {
			if (stats.blasBuiltLastFrame >= kMaxBlasBuildsPerFrame || triangles >= kMaxBlasTrianglesPerFrame)
				break;
			// Only meshes in the scene, whose upload has completed (state is updated from the fence). BLAS builds are
			// recorded before this frame's Update() marks meshes seen, so "seen last frame" is the freshest mark.
			if (entry.state != State::kResident || entry.blasAllocation.IsValid() || entry.blasFailed || entry.lastSeenFrame + 1 < a_frame)
				continue;
			if (entry.skinned)
				continue;  // bind-pose data only: SkinnedMeshes builds per-instance BLASes from the skinned positions

			// ARCHITECTURE §3/M3: positions are float3 at offset 0; stride from the VertexDesc nibble; 16-bit indices.
			D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
			geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometry.Triangles.VertexCount = entry.vertexCount;
			geometry.Triangles.VertexBuffer = { pool.GetAddress(entry.vertexAllocation), entry.stride };
			geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
			geometry.Triangles.IndexCount = entry.triangleCount * 3;
			geometry.Triangles.IndexBuffer = pool.GetAddress(entry.indexAllocation);

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &geometry;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
			device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
			const uint64_t scratchBytes = (prebuild.ScratchDataSizeInBytes + BufferPool::kAlignment - 1) & ~(BufferPool::kAlignment - 1);
			if (a_scratchUsed + scratchBytes > a_scratchBytes)
				break;  // scratch full: the rest waits for next frame
			if (!asPool.Allocate(prebuild.ResultDataMaxSizeInBytes, entry.blasAllocation)) {
				entry.blasFailed = true;
				stats.blasFailed++;
				continue;
			}

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
			build.Inputs = inputs;
			build.DestAccelerationStructureData = asPool.GetAddress(entry.blasAllocation);
			build.ScratchAccelerationStructureData = a_scratch + a_scratchUsed;
			a_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

			a_scratchUsed += scratchBytes;
			triangles += entry.triangleCount;
			stats.blasBuiltLastFrame++;
			stats.blasTotalBuilt++;
		}
	}

	bool MeshCache::FindResident(const GeometryCandidate& a_candidate, ResidentMesh& a_out) const
	{
		auto it = entries.find(MakeKey(a_candidate));
		if (it == entries.end() || it->second.state != State::kResident)
			return false;
		const auto& entry = it->second;
		a_out = { .vertexPage = entry.vertexAllocation.page,
			.vertexOffset = static_cast<uint32_t>(entry.vertexAllocation.offset),
			.indexPage = entry.indexAllocation.page,
			.indexOffset = static_cast<uint32_t>(entry.indexAllocation.offset),
			.stride = entry.stride,
			.vertexCount = entry.vertexCount,
			.triangleCount = entry.triangleCount,
			.vertexAddress = pool.GetAddress(entry.vertexAllocation),
			.indexAddress = pool.GetAddress(entry.indexAllocation) };
		return true;
	}

	void MeshCache::GatherInstances(const std::vector<GeometryCandidate>& a_candidates, std::vector<InstanceRecord>& a_out) const
	{
		a_out.clear();
		for (const auto& candidate : a_candidates) {
			if (candidate.skinned)
				continue;
			auto it = entries.find(MakeKey(candidate));
			if (it == entries.end() || !it->second.blasAllocation.IsValid())
				continue;
			const auto& entry = it->second;
			a_out.push_back({ .blas = asPool.GetAddress(entry.blasAllocation),
				.world = candidate.world,
				.vertexPage = entry.vertexAllocation.page,
				.vertexOffset = static_cast<uint32_t>(entry.vertexAllocation.offset),
				.indexPage = entry.indexAllocation.page,
				.indexOffset = static_cast<uint32_t>(entry.indexAllocation.offset),
				.stride = entry.stride,
				.albedo = candidate.albedo,
				.terrain = candidate.terrain,
				.alphaTested = candidate.alphaTested,
				.alphaBlended = candidate.alphaBlended });
		}
	}
}
