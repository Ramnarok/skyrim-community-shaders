#include "SkinnedMeshes.h"

#include <fstream>

namespace RT
{
	namespace
	{
		constexpr uint32_t kTimestampsPerSlot = 2;

		struct SkinParams
		{
			uint32_t vertexCount;
			uint32_t stride;
			uint32_t skinningOffset;
			uint32_t halfPositions;
			uint32_t boneCount;
			uint32_t dynamicStride;
			uint32_t pad[2];
		};
		static_assert(sizeof(SkinParams) == 32);

		uint64_t Align(uint64_t a_value, uint64_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		float Ms(uint64_t a_begin, uint64_t a_end, uint64_t a_frequency)
		{
			return (a_end > a_begin && a_frequency) ? static_cast<float>(static_cast<double>(a_end - a_begin) * 1000.0 / static_cast<double>(a_frequency)) : 0.0f;
		}
	}

	uint64_t SkinnedMeshes::KeyHash::operator()(const Key& a_key) const noexcept
	{
		return ankerl::unordered_dense::detail::wyhash::hash((reinterpret_cast<uint64_t>(a_key.skinInstance) * 31 + a_key.partition) * 31 + reinterpret_cast<uint64_t>(a_key.mesh));
	}

	bool SkinnedMeshes::Init(ID3D12Device5* a_device)
	{
		device = a_device;
		auto fail = [](std::string a_reason) {
			logger::error("[SkyrimRT] Skinned meshes setup failed: {}", a_reason);
			return false;
		};

		outputPool.Init(device, kPageBytes, kOutputBudgetBytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"SkyrimRT::SkinnedOutput");
		dynamicPool.Init(device, kPageBytes, kDynamicBudgetBytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE, L"SkyrimRT::DynamicPositions");
		blasPool.Init(device, kPageBytes, kBlasBudgetBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"SkyrimRT::SkinnedBLAS");

		const std::filesystem::path path = "Data\\Shaders\\SkyrimRT\\SkinCS.cso";
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return fail(std::format("cannot open {}", path.string()));
		const std::vector<char> bytecode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		D3D12_ROOT_PARAMETER params[5]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0
		params[0].Constants = { 0, 0, sizeof(SkinParams) / 4 };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t0: bind-pose vertices
		params[1].Descriptor = { 0, 0 };
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t1: palette
		params[2].Descriptor = { 1, 0 };
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u0: skinned positions
		params[3].Descriptor = { 0, 0 };
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t2: dynamic positions
		params[4].Descriptor = { 2, 0 };
		for (auto& param : params)
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{ .NumParameters = 5, .pParameters = params };
		winrt::com_ptr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put());
		if (FAILED(hr))
			return fail(std::format("serialize root signature failed ({})", FormatHResult(hr)));
		if (FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
			return fail(std::format("CreateRootSignature failed ({})", FormatHResult(hr)));
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.CS = { bytecode.data(), bytecode.size() };
		if (FAILED(hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pipeline.put()))))
			return fail(std::format("CreateComputePipelineState(SkinCS) failed ({})", FormatHResult(hr)));
		pipeline->SetName(L"SkyrimRT::SkinPSO");

		for (uint32_t i = 0; i < kFramesInFlight; i++) {
			if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_UPLOAD, kUploadBytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, uploads[i].put())))
				return fail(std::format("creating palette upload buffer failed ({})", FormatHResult(hr)));
			uploads[i]->SetName(L"SkyrimRT::SkinPalettes");
			D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			if (FAILED(uploads[i]->Map(0, &noRead, &mapped)))
				return fail("mapping palette upload buffer failed");
			uploadCpu[i] = static_cast<uint8_t*>(mapped);
			if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_UPLOAD, kDynamicUploadBytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, dynamicUploads[i].put())))
				return fail(std::format("creating dynamic-position upload buffer failed ({})", FormatHResult(hr)));
			dynamicUploads[i]->SetName(L"SkyrimRT::DynamicPositionUpload");
			if (FAILED(dynamicUploads[i]->Map(0, &noRead, &mapped)))
				return fail("mapping dynamic-position upload buffer failed");
			dynamicUploadCpu[i] = static_cast<uint8_t*>(mapped);
		}
		D3D12_QUERY_HEAP_DESC queryDesc{ .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = kTimestampsPerSlot * kFramesInFlight };
		if (FAILED(hr = device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(timestamps.put()))) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t) * kTimestampsPerSlot * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, timestampReadback.put())))
			return fail(std::format("creating timestamp objects failed ({})", FormatHResult(hr)));
		void* mapped = nullptr;
		if (FAILED(timestampReadback->Map(0, nullptr, &mapped)))
			return fail("mapping timestamp readback failed");
		timestampCpu = static_cast<const uint64_t*>(mapped);

		ready = true;
		logger::info("[SkyrimRT] Skinned meshes ready (compute skinning + BLAS refit)");
		return true;
	}

	void SkinnedMeshes::Release(Entry& a_entry)
	{
		if (a_entry.output.IsValid())
			outputPool.Free(a_entry.output);
		if (a_entry.blas.IsValid())
			blasPool.Free(a_entry.blas);
		if (a_entry.dynamic.IsValid())
			dynamicPool.Free(a_entry.dynamic);
		a_entry.dynamicValid = false;
		a_entry.built = false;
	}

	void SkinnedMeshes::Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, uint64_t a_frame, const MeshCache& a_cache,
		const std::vector<GeometryCandidate>& a_candidates, const SkinnedScene& a_scene, const RE::NiPoint3& a_posAdjust,
		D3D12_GPU_VIRTUAL_ADDRESS a_scratch, uint64_t a_scratchBytes, uint64_t& a_scratchUsed)
	{
		stats.skinnedLastFrame = 0;
		stats.waitingForMesh = 0;
		stats.blasBuiltLastFrame = 0;
		stats.blasRefitLastFrame = 0;
		stats.blasSkippedScratch = 0;
		stats.duplicatePartitions = 0;
		stats.verticesLastFrame = 0;
		stats.dynamicPartitions = 0;
		stats.dynamicUploadsLastFrame = 0;
		stats.dynamicUploadBytesLastFrame = 0;
		stats.dynamicWaiting = 0;
		visible.assign(a_scene.partitions.size(), Key{});
		if (!ready)
			return;

		// Unseen for kEvictAfterFrames: no frame in flight can reference the entry any more.
		std::erase_if(entries, [&](auto& a_pair) {
			if (a_pair.second.lastSeenFrame + kEvictAfterFrames >= a_frame)
				return false;
			Release(a_pair.second);
			return true;
		});
		// Replaced entries are freed once the frames that may still read them have completed.
		std::erase_if(retired, [&](auto& a_pair) {
			if (a_pair.first + kFramesInFlight + 1 > a_frame)
				return false;
			Release(a_pair.second);
			return true;
		});

		struct Work
		{
			Key key;
			const SkinnedPartition* partition;
			uint64_t paletteOffset;
		};
		std::vector<Work> work;
		struct DynamicCopy
		{
			ID3D12Resource* destination;
			uint64_t destinationOffset;
			uint64_t sourceOffset;
			uint64_t bytes;
		};
		std::vector<DynamicCopy> dynamicCopies;
		uint64_t dynamicUsed = 0;
		work.reserve(a_scene.partitions.size());
		uint64_t uploadUsed = 0;
		uint8_t* upload = uploadCpu[a_slot];
		ankerl::unordered_dense::set<Key, KeyHash> keysThisFrame;
		keysThisFrame.reserve(a_scene.partitions.size());

		for (size_t i = 0; i < a_scene.partitions.size(); i++) {
			const auto& partition = a_scene.partitions[i];
			ResidentMesh source;
			if (!a_cache.FindResident(a_candidates[partition.candidateIndex], source)) {
				stats.waitingForMesh++;
				continue;
			}
			const Key key{ partition.skinInstance, partition.partitionIndex, a_candidates[partition.candidateIndex].rendererData };
			// Each entry gets at most one skin + build per frame: two BuildRaytracingAccelerationStructure calls on the
			// same BLAS in one list, with no barrier between them, race (and a refit reading a BLAS another build is
			// writing is undefined). A GPU-hang candidate, so a repeat is skipped and counted.
			if (!keysThisFrame.insert(key).second) {
				stats.duplicatePartitions++;
				continue;
			}
			auto& existing = entries[key];
			// A different mesh behind the same skin instance (re-equipped armour, address reuse): start over with
			// fresh allocations, retiring the old ones until frames in flight are done with them.
			if (existing.output.IsValid() && (existing.source.vertexCount != source.vertexCount || existing.source.triangleCount != source.triangleCount ||
												 existing.source.indexAddress != source.indexAddress)) {
				retired.emplace_back(a_frame, existing);
				existing = Entry{};
			}
			auto& entry = existing;
			entry.lastSeenFrame = a_frame;
			if (entry.failed)
				continue;
			entry.source = source;
			if (!entry.output.IsValid() && !outputPool.Allocate(Align(static_cast<uint64_t>(source.vertexCount) * 12, BufferPool::kAlignment), entry.output)) {
				entry.failed = true;
				stats.failed++;
				continue;
			}

			// M7b: (re)upload the dynamic positions when the game rewrote them, within the per-frame budget.
			if (partition.dynamicData) {
				const uint64_t bytes = static_cast<uint64_t>(partition.dynamicVertexCount) * partition.dynamicStride;
				if (!entry.dynamic.IsValid() && !dynamicPool.Allocate(Align(bytes, BufferPool::kAlignment), entry.dynamic)) {
					entry.failed = true;
					stats.failed++;
					continue;
				}
				if (!entry.dynamicValid || entry.dynamicVersion != partition.dynamicVersion) {
					if (dynamicUsed + bytes <= kDynamicUploadBytes) {
						std::memcpy(dynamicUploadCpu[a_slot] + dynamicUsed, partition.dynamicData, bytes);
						dynamicCopies.push_back({ dynamicPool.GetResource(entry.dynamic.page), entry.dynamic.offset, dynamicUsed, bytes });
						dynamicUsed = Align(dynamicUsed + bytes, 256);
						entry.dynamicVersion = partition.dynamicVersion;
						entry.dynamicValid = true;
						stats.dynamicUploadsLastFrame++;
						stats.dynamicUploadBytesLastFrame += bytes;
					} else if (!entry.dynamicValid) {
						stats.dynamicWaiting++;
						continue;
					}
				}
			}

			const uint64_t paletteBytes = static_cast<uint64_t>(partition.boneCount) * 48;
			if (uploadUsed + paletteBytes > kUploadBytes)
				break;
			auto* rows = reinterpret_cast<float*>(upload + uploadUsed);
			const float* palette = a_scene.palettes.data() + partition.paletteOffset;
			const float adjust[3] = { a_posAdjust.x, a_posAdjust.y, a_posAdjust.z };
			for (uint32_t b = 0; b < partition.boneCount; b++) {
				for (uint32_t r = 0; r < 3; r++) {
					const float* in = palette + (b * 3 + r) * 4;
					float* out = rows + (b * 3 + r) * 4;
					out[0] = in[0];
					out[1] = in[1];
					out[2] = in[2];
					out[3] = in[3] - adjust[r];  // camera-relative, as every other TLAS input
				}
			}
			work.push_back({ key, &partition, uploadUsed });
			uploadUsed = Align(uploadUsed + paletteBytes, 256);
			visible[i] = key;
		}

		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot + 0);
		if (!work.empty()) {
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			auto transitionPages = [&](D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after) {
				barriers.clear();
				for (uint32_t page = 0; page < outputPool.GetPageSlots(); page++) {
					if (auto* resource = outputPool.GetResource(page)) {
						D3D12_RESOURCE_BARRIER barrier{};
						barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
						barrier.Transition.pResource = resource;
						barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
						barrier.Transition.StateBefore = a_before;
						barrier.Transition.StateAfter = a_after;
						barriers.push_back(barrier);
					}
				}
				if (!barriers.empty())
					a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
			};

			// 0. Changed dynamic positions.
			if (!dynamicCopies.empty()) {
				std::vector<ID3D12Resource*> pages;
				for (const auto& copy : dynamicCopies) {
					if (std::ranges::find(pages, copy.destination) == pages.end())
						pages.push_back(copy.destination);
				}
				auto transition = [&](D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after) {
					barriers.clear();
					for (auto* page : pages) {
						D3D12_RESOURCE_BARRIER barrier{};
						barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
						barrier.Transition.pResource = page;
						barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
						barrier.Transition.StateBefore = a_before;
						barrier.Transition.StateAfter = a_after;
						barriers.push_back(barrier);
					}
					a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
				};
				transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				for (const auto& copy : dynamicCopies)
					a_list->CopyBufferRegion(copy.destination, copy.destinationOffset, dynamicUploads[a_slot].get(), copy.sourceOffset, copy.bytes);
				transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}

			// 1. Skinning.
			transitionPages(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			a_list->SetComputeRootSignature(rootSignature.get());
			a_list->SetPipelineState(pipeline.get());
			const D3D12_GPU_VIRTUAL_ADDRESS uploadVA = uploads[a_slot]->GetGPUVirtualAddress();
			// No insertions from here on, so entry references stay valid.
			for (const auto& item : work) {
				const auto& entry = entries.find(item.key)->second;
				const auto& source = entry.source;
				const bool dynamic = item.partition->dynamicData && entry.dynamicValid;
				const SkinParams params{ source.vertexCount, source.stride, item.partition->skinningOffset, item.partition->halfPositions ? 1u : 0u, item.partition->boneCount,
					dynamic ? item.partition->dynamicStride : 0u, {} };
				stats.dynamicPartitions += dynamic;
				a_list->SetComputeRoot32BitConstants(0, sizeof(SkinParams) / 4, &params, 0);
				a_list->SetComputeRootShaderResourceView(1, source.vertexAddress);
				a_list->SetComputeRootShaderResourceView(2, uploadVA + item.paletteOffset);
				a_list->SetComputeRootUnorderedAccessView(3, outputPool.GetAddress(entry.output));
				a_list->SetComputeRootShaderResourceView(4, dynamic ? dynamicPool.GetAddress(entry.dynamic) : source.vertexAddress);
				a_list->Dispatch((source.vertexCount + 63) / 64, 1, 1);
				stats.skinnedLastFrame++;
				stats.verticesLastFrame += source.vertexCount;
			}
			transitionPages(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			// 2. BLAS: build (first time, and every kRebuildInterval frames), refit otherwise.
			for (const auto& item : work) {
				auto& entry = entries.find(item.key)->second;
				const auto& source = entry.source;
				D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
				geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
				geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				geometry.Triangles.VertexCount = source.vertexCount;
				geometry.Triangles.VertexBuffer = { outputPool.GetAddress(entry.output), 12 };
				geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
				geometry.Triangles.IndexCount = source.triangleCount * 3;
				geometry.Triangles.IndexBuffer = source.indexAddress;

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
				inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
				inputs.NumDescs = 1;
				inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				inputs.pGeometryDescs = &geometry;

				if (!entry.blas.IsValid()) {
					D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
					device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
					entry.buildScratchBytes = Align(prebuild.ScratchDataSizeInBytes, BufferPool::kAlignment);
					entry.updateScratchBytes = Align(std::max<uint64_t>(prebuild.UpdateScratchDataSizeInBytes, 1), BufferPool::kAlignment);
					if (!blasPool.Allocate(prebuild.ResultDataMaxSizeInBytes, entry.blas)) {
						entry.failed = true;
						stats.failed++;
						continue;
					}
				}
				const bool rebuild = !entry.built || a_frame >= entry.builtFrame + kRebuildInterval;
				const uint64_t scratchBytes = rebuild ? entry.buildScratchBytes : entry.updateScratchBytes;
				if (a_scratchUsed + scratchBytes > a_scratchBytes) {
					stats.blasSkippedScratch++;  // keeps last frame's pose; a never-built entry stays out of the TLAS
					continue;
				}

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
				build.Inputs = inputs;
				build.DestAccelerationStructureData = blasPool.GetAddress(entry.blas);
				build.ScratchAccelerationStructureData = a_scratch + a_scratchUsed;
				if (!rebuild) {
					build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
					build.SourceAccelerationStructureData = build.DestAccelerationStructureData;
				}
				a_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
				a_scratchUsed += scratchBytes;
				if (rebuild) {
					entry.built = true;
					entry.builtFrame = a_frame;
					stats.blasBuiltLastFrame++;
					stats.totalBuilt++;
				} else {
					stats.blasRefitLastFrame++;
				}
			}
		}
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot + 1);
		a_list->ResolveQueryData(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot, kTimestampsPerSlot, timestampReadback.get(), sizeof(uint64_t) * kTimestampsPerSlot * a_slot);
		slotPending[a_slot] = true;

		stats.entries = static_cast<uint32_t>(entries.size());
		stats.outputBytes = outputPool.GetReservedBytes();
		stats.blasBytes = blasPool.GetReservedBytes();
	}

	void SkinnedMeshes::GatherInstances(const std::vector<GeometryCandidate>& a_candidates, const SkinnedScene& a_scene,
		const RE::NiPoint3& a_posAdjust, std::vector<InstanceRecord>& a_out) const
	{
		uint32_t count = 0;
		for (size_t i = 0; i < a_scene.partitions.size() && i < visible.size(); i++) {
			const Key& key = visible[i];
			if (!key.skinInstance)
				continue;
			auto it = entries.find(key);
			if (it == entries.end() || !it->second.built || it->second.failed)
				continue;
			const auto& entry = it->second;
			const auto& candidate = a_candidates[a_scene.partitions[i].candidateIndex];
			InstanceRecord record{};
			record.blas = blasPool.GetAddress(entry.blas);
			// Positions are already camera-relative: identity, with the translation Raytracer subtracts put back.
			record.world.rotate = RE::NiMatrix3();
			record.world.scale = 1.0f;
			record.world.translate = a_posAdjust;
			record.vertexPage = kFirstPageSlot + entry.output.page;
			record.vertexOffset = static_cast<uint32_t>(entry.output.offset);
			record.indexPage = entry.source.indexPage;
			record.indexOffset = entry.source.indexOffset;
			record.stride = 12;
			record.albedo = candidate.albedo;
			// M7c: trees are skinned (their branches sway on bones), and so is hair; UVs come from the bind-pose source.
			record.alpha = candidate.alphaWord;
			record.albedoWord = candidate.albedoWord;  // M8: texture and vertex colours from the bind-pose source too
			record.uvPage = entry.source.vertexPage;
			record.uvOffset = entry.source.vertexOffset;
			record.uvStride = entry.source.stride;
			record.room = candidate.roomWord;
			record.alphaTested = candidate.alphaTested;
			record.alphaBlended = candidate.alphaBlended;
			record.actor = true;
			record.windAnimated = candidate.windAnimated;
			a_out.push_back(record);
			count++;
		}
		const_cast<SkinnedStats&>(stats).instances = count;
	}

	void SkinnedMeshes::CollectResults(uint32_t a_slot)
	{
		if (!slotPending[a_slot])
			return;
		slotPending[a_slot] = false;
		const uint64_t* t = timestampCpu + kTimestampsPerSlot * a_slot;
		stats.skinMs.Add(Ms(t[0], t[1], timestampFrequency));
	}
}
