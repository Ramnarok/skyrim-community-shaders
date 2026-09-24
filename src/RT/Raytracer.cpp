#include "Raytracer.h"

#include "AlphaAtlas.h"

#include <dxgi1_2.h>
#include <fstream>

namespace RT
{
	namespace
	{
		// Per-slot upload buffer layout.
		constexpr uint64_t kInstanceDescOffset = 0;                         // kMaxInstances * 64 B
		constexpr uint64_t kInstanceDataOffset = 4ull << 20;                // kMaxInstances * 48 B
		constexpr uint64_t kAabbOffset = 7ull << 20;                        // kMaxExclusions * 24 B
		constexpr uint64_t kConstantsOffset = 8ull << 20;                   // TraceConstants
		constexpr uint64_t kZeroOffset = kConstantsOffset + 256;            // zeros for the counter clear
		constexpr uint64_t kCounterBytes = 64;                              // 16 uints, kCounterCount used
		static_assert(kCounterCount * sizeof(uint32_t) <= kCounterBytes);
		constexpr uint32_t kTimestampsPerSlot = 4;

		constexpr uint32_t kMaskStatic = 0x01;
		constexpr uint32_t kMaskTerrain = 0x02;
		constexpr uint32_t kMaskAlphaTested = 0x08;
		constexpr uint32_t kMaskExclusion = 0x10;
		constexpr uint32_t kMaskActor = 0x04;         // M7 skinned
		constexpr uint32_t kMaskAlphaBlended = 0x20;  // in no trace: alpha-blended (drawn after the pre-water depth copy) and decals

		// Descriptor heap layout.
		constexpr uint32_t kDepthDescriptor = 0;
		constexpr uint32_t kFirstPageDescriptor = 1;
		constexpr uint32_t kFirstViewDescriptor = kFirstPageDescriptor + Raytracer::kMeshPageSlots;
		constexpr uint32_t kAlphaAtlasDescriptor = kFirstViewDescriptor + static_cast<uint32_t>(DebugView::kCount);
		constexpr uint32_t kDescriptorCount = kAlphaAtlasDescriptor + 1;

		// M7c: alpha-tested instances with an atlas tile are traced as non-opaque; the passes alpha-test candidate hits.
		constexpr uint32_t kInstanceFlagForceNonOpaque = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;

		// Must match TraceConstants in RayQueryDebugCS.hlsl (cbuffer packing).
		struct alignas(16) TraceConstants
		{
			float viewProjInverse[16];
			uint32_t renderSize[2];
			uint32_t instanceCount;
			uint32_t exclusionCount;
			float loadedMin[4];
			float loadedMax[4];
			float mismatchThreshold;
			float exclusionMargin;
			float maxDistance;
			float clutterHeight;
		};
		static_assert(sizeof(TraceConstants) == 128);

		// Must match InstanceData in RayQueryDebugCS.hlsl.
		struct InstanceGpu
		{
			uint32_t vertexPage, vertexOffset, indexPage, indexOffset;
			uint32_t stride, flags, albedo, alpha;  // albedo: RGBA8 average diffuse (M6 material table); alpha: M7c alpha test
			uint32_t uvPage, uvOffset, uvStride, room;  // M7c: texture-coordinate source (bind pose for skinned); M8 room index + 1
		};
		static_assert(sizeof(InstanceGpu) == 48);
		static_assert(kInstanceDataOffset + Raytracer::kMaxInstances * sizeof(InstanceGpu) <= kAabbOffset);
		static_assert(kAabbOffset + Raytracer::kMaxExclusions * sizeof(D3D12_RAYTRACING_AABB) <= kConstantsOffset);
		static_assert(kZeroOffset + kCounterBytes <= Raytracer::kUploadSlotBytes);

		void Transition(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			a_list->ResourceBarrier(1, &barrier);
		}

		void GlobalUavBarrier(ID3D12GraphicsCommandList* a_list)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = nullptr;
			a_list->ResourceBarrier(1, &barrier);
		}

		uint64_t Align(uint64_t a_value, uint64_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		float Ms(uint64_t a_begin, uint64_t a_end, uint64_t a_frequency)
		{
			return (a_end > a_begin && a_frequency) ? static_cast<float>(static_cast<double>(a_end - a_begin) * 1000.0 / static_cast<double>(a_frequency)) : 0.0f;
		}
	}

	bool Raytracer::Fail(std::string a_reason)
	{
		failureReason = std::move(a_reason);
		logger::error("[SkyrimRT] Raytracer setup failed: {}", failureReason);
		return false;
	}

	bool Raytracer::CreatePipeline()
	{
		// Compiled at build time by DXC (cmake/SkyrimRTShaders.cmake).
		const std::filesystem::path path = "Data\\Shaders\\SkyrimRT\\RayQueryDebugCS.cso";
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return Fail(std::format("cannot open {}", path.string()));
		const std::vector<char> bytecode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		D3D12_DESCRIPTOR_RANGE ranges[4]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, kDepthDescriptor };                  // t1: raster depth
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMeshPageSlots, 0, 1, kFirstPageDescriptor };  // t0, space1: mesh pages
		ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, static_cast<UINT>(DebugView::kCount), 0, 0, kFirstViewDescriptor };  // u0-u3
		ranges[3] = GetAlphaAtlasRange(kAlphaAtlasDescriptor);                                        // t0, space2: alpha atlas

		D3D12_ROOT_PARAMETER params[6]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0
		params[0].Descriptor = { 0, 0 };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t0: TLAS
		params[1].Descriptor = { 0, 0 };
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t2: instance data
		params[2].Descriptor = { 2, 0 };
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t3: exclusion AABBs
		params[3].Descriptor = { 3, 0 };
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u4: counters
		params[4].Descriptor = { 4, 0 };
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[5].DescriptorTable = { 4, ranges };
		for (auto& param : params)
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		const D3D12_STATIC_SAMPLER_DESC sampler = GetAlphaAtlasSampler();
		D3D12_ROOT_SIGNATURE_DESC rootDesc{ .NumParameters = 6, .pParameters = params, .NumStaticSamplers = 1, .pStaticSamplers = &sampler };
		winrt::com_ptr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put());
		if (FAILED(hr))
			return Fail(std::format("serialize trace root signature failed ({}): {}", FormatHResult(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
		if (FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
			return Fail(std::format("CreateRootSignature(trace) failed ({})", FormatHResult(hr)));

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.CS = { bytecode.data(), bytecode.size() };
		if (FAILED(hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pipeline.put()))))
			return Fail(std::format("CreateComputePipelineState(RayQueryDebugCS) failed ({})", FormatHResult(hr)));
		pipeline->SetName(L"SkyrimRT::RayQueryDebugPSO");
		return true;
	}

	bool Raytracer::Init(ID3D12Device5* a_device, ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, uint32_t a_screenWidth, uint32_t a_screenHeight,
		ID3D12Resource* a_alphaAtlas)
	{
		device = a_device;
		d3d11Device = a_d3d11Device;
		d3d11Context = a_d3d11Context;
		width = a_screenWidth;
		height = a_screenHeight;
		alphaAtlas = a_alphaAtlas;

		std::string error;
		if (!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R32_FLOAT, "RasterDepth", rasterDepth, error))
			return Fail(std::move(error));
		constexpr const char* kViewNames[] = { "DepthView", "InstanceView", "NormalView", "DiffView" };
		for (uint32_t i = 0; i < views.size(); i++) {
			if (!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, kViewNames[i], views[i], error))
				return Fail(std::move(error));
		}

		copyDepthCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SkyrimRT\\CopyDepthCS.hlsl", {}, "cs_5_0")));
		if (!copyDepthCS)
			return Fail("compiling Data\\Shaders\\SkyrimRT\\CopyDepthCS.hlsl failed");

		if (!CreatePipeline())
			return false;

		// Descriptors: raster depth, 64 mesh-page slots (null until pages exist), 4 view UAVs.
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = kDescriptorCount, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.put()));
		if (FAILED(hr))
			return Fail(std::format("CreateDescriptorHeap(trace) failed ({})", FormatHResult(hr)));
		descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpuHandle = [&](uint32_t a_index) {
			auto handle = heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(a_index) * descriptorSize;
			return handle;
		};

		D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
		depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
		depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		depthSrv.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(rasterDepth.resource12.get(), &depthSrv, cpuHandle(kDepthDescriptor));

		D3D12_SHADER_RESOURCE_VIEW_DESC nullPage{};
		nullPage.Format = DXGI_FORMAT_R32_TYPELESS;
		nullPage.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		nullPage.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nullPage.Buffer.NumElements = 1;
		nullPage.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		for (uint32_t i = 0; i < kMeshPageSlots; i++)
			device->CreateShaderResourceView(nullptr, &nullPage, cpuHandle(kFirstPageDescriptor + i));

		for (uint32_t i = 0; i < views.size(); i++) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(views[i].resource12.get(), nullptr, &uav, cpuHandle(kFirstViewDescriptor + i));
		}
		WriteAlphaAtlasDescriptor(device, alphaAtlas, cpuHandle(kAlphaAtlasDescriptor));

		// Acceleration structure memory sized for the maximum counts, so nothing is reallocated while in flight.
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
		tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		tlasInputs.NumDescs = kMaxInstances;
		tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);

		D3D12_RAYTRACING_GEOMETRY_DESC aabbGeometry{};
		aabbGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
		aabbGeometry.AABBs.AABBCount = kMaxExclusions;
		aabbGeometry.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS aabbInputs{};
		aabbInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		aabbInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		aabbInputs.NumDescs = 1;
		aabbInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		aabbInputs.pGeometryDescs = &aabbGeometry;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO aabbInfo{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&aabbInputs, &aabbInfo);

		tlasScratchBytes = Align(tlasInfo.ScratchDataSizeInBytes, BufferPool::kAlignment);
		exclusionScratchBytes = Align(aabbInfo.ScratchDataSizeInBytes, BufferPool::kAlignment);
		if (tlasScratchBytes + exclusionScratchBytes >= kScratchBytes)
			return Fail("scratch buffer too small for TLAS + exclusion BLAS");

		constexpr auto kAS = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		constexpr auto kUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, kScratchBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kUAV, scratch.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, Align(tlasInfo.ResultDataMaxSizeInBytes, 256), kAS, kUAV, tlas.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, Align(aabbInfo.ResultDataMaxSizeInBytes, 256), kAS, kUAV, exclusionBlas.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, kCounterBytes, D3D12_RESOURCE_STATE_COMMON, kUAV, counters.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, kCounterBytes * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, countersReadback.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t) * kTimestampsPerSlot * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, timestampReadback.put())))
			return Fail(std::format("creating acceleration structure / result buffers failed ({})", FormatHResult(hr)));
		scratch->SetName(L"SkyrimRT::Scratch");
		tlas->SetName(L"SkyrimRT::TLAS");
		exclusionBlas->SetName(L"SkyrimRT::ExclusionBLAS");
		counters->SetName(L"SkyrimRT::TraceCounters");

		for (uint32_t i = 0; i < kFramesInFlight; i++) {
			if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_UPLOAD, kUploadSlotBytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, uploads[i].put())))
				return Fail(std::format("creating trace upload buffer failed ({})", FormatHResult(hr)));
			uploads[i]->SetName(L"SkyrimRT::TraceUpload");
			D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			if (FAILED(uploads[i]->Map(0, &noRead, &mapped)))
				return Fail("mapping trace upload buffer failed");
			uploadCpu[i] = static_cast<uint8_t*>(mapped);
			std::memset(uploadCpu[i] + kZeroOffset, 0, kCounterBytes);
		}

		void* mapped = nullptr;
		if (FAILED(countersReadback->Map(0, nullptr, &mapped)))
			return Fail("mapping counter readback failed");
		countersCpu = static_cast<const uint32_t*>(mapped);
		if (FAILED(timestampReadback->Map(0, nullptr, &mapped)))
			return Fail("mapping timestamp readback failed");
		timestampCpu = static_cast<const uint64_t*>(mapped);

		D3D12_QUERY_HEAP_DESC queryDesc{ .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = kTimestampsPerSlot * kFramesInFlight };
		if (FAILED(hr = device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(timestamps.put()))))
			return Fail(std::format("CreateQueryHeap(trace) failed ({})", FormatHResult(hr)));

		instanceDescs.reserve(kMaxInstances);
		logger::info("[SkyrimRT] Raytracer ready: {}x{} views, TLAS {:.1f} MB for {} instances, scratch {} MB",
			width, height, tlasInfo.ResultDataMaxSizeInBytes / (1024.0 * 1024.0), kMaxInstances, kScratchBytes >> 20);

		// M5. A failure here only disables sun shadows; the debug trace keeps working.
		sunShadowsReady = sunShadows.Init(device, d3d11Device, d3d11Context, width, height, rasterDepth.resource12.get(), copyDepthCS.get(), alphaAtlas);
		// M8. A failure only leaves the game's point lights unshadowed, as before.
		pointShadowsReady = pointShadows.Init(device, d3d11Device, d3d11Context, width, height, rasterDepth.resource12.get(), copyDepthCS.get(), alphaAtlas,
			ShadowKind::kPointLights);
		// M7. A failure only keeps actors out of the TLAS.
		skinnedReady = skinned.Init(device);
		return true;
	}

	void Raytracer::SetTimestampFrequency(uint64_t a_frequency)
	{
		timestampFrequency = a_frequency;
		sunShadows.SetTimestampFrequency(a_frequency);
		pointShadows.SetTimestampFrequency(a_frequency);
		skinned.SetTimestampFrequency(a_frequency);
	}

	void Raytracer::CopyInputs(bool a_compareShadowMap)
	{
		if (sunShadowsReady)
			sunShadows.CopyInputs(a_compareShadowMap);

		// The same scene depth ScreenSpaceShadows reads in its Prepass: TerrainBlending's R32 blended pre-pass depth
		// when that feature is active, else the game's kPOST_ZPREPASS_COPY (at Prepass time: the depth pre-pass).
		auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
		if (!depthSRV)
			return;

		auto* ctx = d3d11Context;
		ID3D11UnorderedAccessView* uav = rasterDepth.uav11.get();
		ctx->CSSetShader(copyDepthCS.get(), nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &depthSRV);
		ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetShaderResources(0, 1, &nullSRV);
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ctx->CSSetShader(nullptr, nullptr, 0);
	}

	D3D12_GPU_VIRTUAL_ADDRESS Raytracer::GetInstanceDataAddress(uint32_t a_slot) const
	{
		return uploads[a_slot]->GetGPUVirtualAddress() + kInstanceDataOffset;
	}

	void Raytracer::UpdatePageDescriptors(const BufferPool& a_pool)
	{
		auto first = heap->GetCPUDescriptorHandleForHeapStart();
		first.ptr += static_cast<SIZE_T>(kFirstPageDescriptor) * descriptorSize;
		RT::UpdatePageDescriptors(device, a_pool, first, descriptorSize, describedPageSerials.data(), SkinnedMeshes::kFirstPageSlot);
		first.ptr += static_cast<SIZE_T>(SkinnedMeshes::kFirstPageSlot) * descriptorSize;
		RT::UpdatePageDescriptors(device, skinned.GetOutputPool(), first, descriptorSize, describedPageSerials.data() + SkinnedMeshes::kFirstPageSlot, SkinnedMeshes::kPageSlots);
	}

	void Raytracer::Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, uint64_t a_frame, MeshCache& a_cache,
		const std::vector<GeometryCandidate>& a_candidates, const SkinnedScene& a_skinned, const std::vector<ExclusionBound>& a_exclusions,
		const LoadedArea& a_area, const FrameCamera& a_camera, bool a_debugTrace, const SunShadowParams* a_shadows, const PointShadowParams* a_pointShadows,
		bool a_compareShadowMap, bool a_captureDump)
	{
		uint8_t* upload = uploadCpu[a_slot];
		const D3D12_GPU_VIRTUAL_ADDRESS uploadVA = uploads[a_slot]->GetGPUVirtualAddress();
		const D3D12_GPU_VIRTUAL_ADDRESS scratchVA = scratch->GetGPUVirtualAddress();
		const auto& adjust = a_camera.posAdjust;

		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot + 0);

		// 1. BLAS builds for newly resident meshes (scratch below the TLAS / exclusion reservations).
		SetPassMarker(a_list, L"SkyrimRT: BLAS builds");
		uint64_t scratchUsed = 0;
		a_cache.BuildBLASes(a_list, a_frame, scratchVA, kScratchBytes - tlasScratchBytes - exclusionScratchBytes, scratchUsed);
		// M7: skin this frame's actors and build/refit their BLASes (same scratch range, same barrier below).
		SetPassMarker(a_list, L"SkyrimRT: skinning + BLAS refit");
		if (skinnedReady)
			skinned.Record(a_list, a_slot, a_frame, a_cache, a_candidates, a_skinned, adjust, scratchVA, kScratchBytes - tlasScratchBytes - exclusionScratchBytes, scratchUsed);

		// 2. Instances: row-major 3x4, camera-relative. NiTransform applies rotate * p * scale + translate
		//    with rotate.entry[row][col] (CommonLib NiMatrix3::operator*), so rows map straight across.
		a_cache.GatherInstances(a_candidates, instances);
		if (skinnedReady)
			skinned.GatherInstances(a_candidates, a_skinned, adjust, instances);
		const uint32_t instanceCount = static_cast<uint32_t>(std::min<size_t>(instances.size(), kMaxInstances - 1));
		auto* descs = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(upload + kInstanceDescOffset);
		auto* data = reinterpret_cast<InstanceGpu*>(upload + kInstanceDataOffset);
		for (uint32_t i = 0; i < instanceCount; i++) {
			const auto& record = instances[i];
			const auto& world = record.world;
			const float translate[3] = { world.translate.x - adjust.x, world.translate.y - adjust.y, world.translate.z - adjust.z };
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++)
					desc.Transform[row][column] = world.rotate.entry[row][column] * world.scale;
				desc.Transform[row][3] = translate[row];
			}
			desc.InstanceID = i;
			desc.InstanceMask = record.actor ? kMaskActor : record.alphaBlended || record.decal ? kMaskAlphaBlended : record.alphaTested ? kMaskAlphaTested : record.terrain ? kMaskTerrain : kMaskStatic;
			desc.Flags = record.alpha ? kInstanceFlagForceNonOpaque : 0u;
			desc.AccelerationStructure = record.blas;
			descs[i] = desc;
			data[i] = { record.vertexPage, record.vertexOffset, record.indexPage, record.indexOffset, record.stride,
				(record.terrain ? 1u : 0u) | (record.alphaTested ? 2u : 0u) | (record.alphaBlended ? 4u : 0u) | (record.actor ? 8u : 0u) | (record.windAnimated ? 16u : 0u) | (record.albedoWord << 8), record.albedo, record.alpha,
				record.uvPage, record.uvOffset, record.uvStride, record.room };
		}

		SetPassMarker(a_list, L"SkyrimRT: exclusion BLAS");
		// 3. Exclusion AABBs (camera-relative) as one procedural BLAS, instanced with an identity transform.
		const uint32_t exclusionCount = static_cast<uint32_t>(std::min<size_t>(a_exclusions.size(), kMaxExclusions));
		auto* aabbs = reinterpret_cast<D3D12_RAYTRACING_AABB*>(upload + kAabbOffset);
		for (uint32_t i = 0; i < exclusionCount; i++) {
			const auto& bound = a_exclusions[i];
			const float cx = bound.center.x - adjust.x, cy = bound.center.y - adjust.y, cz = bound.center.z - adjust.z;
			aabbs[i] = { cx - bound.radius, cy - bound.radius, cz - bound.radius, cx + bound.radius, cy + bound.radius, cz + bound.radius };
		}
		uint32_t tlasCount = instanceCount;
		if (exclusionCount > 0) {
			D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
			geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
			geometry.AABBs.AABBCount = exclusionCount;
			geometry.AABBs.AABBs = { uploadVA + kAabbOffset, sizeof(D3D12_RAYTRACING_AABB) };
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
			build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			build.Inputs.NumDescs = 1;
			build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			build.Inputs.pGeometryDescs = &geometry;
			build.DestAccelerationStructureData = exclusionBlas->GetGPUVirtualAddress();
			build.ScratchAccelerationStructureData = scratchVA + kScratchBytes - tlasScratchBytes - exclusionScratchBytes;
			a_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1.0f;
			desc.InstanceID = 0xFFFFFF;
			desc.InstanceMask = kMaskExclusion;
			desc.AccelerationStructure = exclusionBlas->GetGPUVirtualAddress();
			descs[tlasCount++] = desc;
		}

		// BLAS writes must land before the TLAS build reads them.
		GlobalUavBarrier(a_list);
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot + 1);

		// 4. TLAS, rebuilt every frame.
		SetPassMarker(a_list, L"SkyrimRT: TLAS build");
		{
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
			build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			build.Inputs.NumDescs = tlasCount;
			build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			build.Inputs.InstanceDescs = uploadVA + kInstanceDescOffset;
			build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
			build.ScratchAccelerationStructureData = scratchVA + kScratchBytes - tlasScratchBytes;
			a_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		}
		GlobalUavBarrier(a_list);
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot + 2);

		const uint32_t renderWidth = std::min(a_camera.renderWidth, width);
		const uint32_t renderHeight = std::min(a_camera.renderHeight, height);
		const bool shadows = a_shadows && sunShadowsReady;
		const bool pointShadowsTraced = a_pointShadows && pointShadowsReady;
		// M7c: D3D11 filled new atlas tiles before the fence signal; readable by the traces until back to COMMON below.
		const bool readsAtlas = alphaAtlas && (a_debugTrace || shadows || pointShadowsTraced);
		if (readsAtlas)
			Transition(a_list, alphaAtlas, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		SetPassMarker(a_list, L"SkyrimRT: debug trace");
		if (a_debugTrace)
			RecordDebugTrace(a_list, a_slot, a_cache, instanceCount, exclusionCount, a_area, a_camera, renderWidth, renderHeight, a_captureDump);
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot + 3);
		a_list->ResolveQueryData(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * kTimestampsPerSlot, kTimestampsPerSlot, timestampReadback.get(), sizeof(uint64_t) * kTimestampsPerSlot * a_slot);

		// 7. M5 sun shadows, reusing this frame's TLAS (and, M7c, its instance data and mesh pages for the alpha test).
		if (shadows)
			sunShadows.Record(a_list, a_slot, tlas->GetGPUVirtualAddress(), uploadVA + kInstanceDataOffset, a_cache.GetMeshPool(), a_camera,
				renderWidth, renderHeight, *a_shadows, a_compareShadowMap, a_captureDump);
		// 8. M8 point-light shadows, same inputs.
		if (pointShadowsTraced)
			pointShadows.RecordPointLights(a_list, a_slot, tlas->GetGPUVirtualAddress(), uploadVA + kInstanceDataOffset, a_cache.GetMeshPool(), a_camera,
				renderWidth, renderHeight, *a_pointShadows, a_captureDump);
		if (readsAtlas)
			Transition(a_list, alphaAtlas, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

		slotPending[a_slot] = true;
		slotInfo[a_slot] = { a_debugTrace, instanceCount, exclusionCount, static_cast<uint32_t>(instances.size() - instanceCount),
			renderWidth, renderHeight, a_area, adjust };
	}

	void Raytracer::RecordDebugTrace(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, MeshCache& a_cache, uint32_t a_instanceCount,
		uint32_t a_exclusionCount, const LoadedArea& a_area, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, bool a_captureDump)
	{
		uint8_t* upload = uploadCpu[a_slot];
		const D3D12_GPU_VIRTUAL_ADDRESS uploadVA = uploads[a_slot]->GetGPUVirtualAddress();
		const auto& adjust = a_camera.posAdjust;

		// 5. Trace.
		auto* constants = reinterpret_cast<TraceConstants*>(upload + kConstantsOffset);
		std::memcpy(constants->viewProjInverse, a_camera.viewProjInverse, sizeof(constants->viewProjInverse));
		constants->renderSize[0] = a_renderWidth;
		constants->renderSize[1] = a_renderHeight;
		const uint32_t instanceCount = a_instanceCount;
		const uint32_t exclusionCount = a_exclusionCount;
		constants->instanceCount = instanceCount;
		constants->exclusionCount = exclusionCount;
		constants->loadedMin[0] = a_area.min.x - adjust.x;
		constants->loadedMin[1] = a_area.min.y - adjust.y;
		constants->loadedMin[2] = a_area.bounded ? 1.0f : 0.0f;
		constants->loadedMax[0] = a_area.max.x - adjust.x;
		constants->loadedMax[1] = a_area.max.y - adjust.y;
		constants->mismatchThreshold = kMismatchThreshold;
		constants->exclusionMargin = 0.01f;
		constants->maxDistance = 100000.0f;
		constants->clutterHeight = kClutterHeight;

		UpdatePageDescriptors(a_cache.GetMeshPool());

		Transition(a_list, counters.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		a_list->CopyBufferRegion(counters.get(), 0, uploads[a_slot].get(), kZeroOffset, kCounterBytes);
		Transition(a_list, counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Transition(a_list, rasterDepth.resource12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		for (auto& view : views)
			Transition(a_list, view.resource12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		ID3D12DescriptorHeap* heaps[] = { heap.get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetComputeRootSignature(rootSignature.get());
		a_list->SetPipelineState(pipeline.get());
		a_list->SetComputeRootConstantBufferView(0, uploadVA + kConstantsOffset);
		a_list->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress());
		a_list->SetComputeRootShaderResourceView(2, uploadVA + kInstanceDataOffset);
		a_list->SetComputeRootShaderResourceView(3, uploadVA + kAabbOffset);
		a_list->SetComputeRootUnorderedAccessView(4, counters->GetGPUVirtualAddress());
		a_list->SetComputeRootDescriptorTable(5, heap->GetGPUDescriptorHandleForHeapStart());
		a_list->Dispatch((constants->renderSize[0] + 7) / 8, (constants->renderSize[1] + 7) / 8, 1);

		for (auto& view : views)
			Transition(a_list, view.resource12.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		Transition(a_list, rasterDepth.resource12.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		Transition(a_list, counters.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		a_list->CopyBufferRegion(countersReadback.get(), kCounterBytes * a_slot, counters.get(), 0, kCounterBytes);
		Transition(a_list, counters.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

		// 6. Optional dump: copy all four views into the readback buffer.
		if (a_captureDump) {
			const auto viewDesc = views[0].resource12->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT64 bytes = 0;
			device->GetCopyableFootprints(&viewDesc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
			bytes = Align(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);  // placed footprint offsets must be 512-aligned
			if (!dumpReadback && FAILED(CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, bytes * views.size(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, dumpReadback.put()))) {
				logger::error("[SkyrimRT] Debug dump: cannot create trace readback buffer");
			} else {
				dumpRowPitch = footprint.Footprint.RowPitch;
				dumpWidth = a_renderWidth;
				dumpHeight = a_renderHeight;
				dumpCaptured = true;
				for (uint32_t i = 0; i < views.size(); i++) {
					Transition(a_list, views[i].resource12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
					D3D12_TEXTURE_COPY_LOCATION dst{ .pResource = dumpReadback.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
					dst.PlacedFootprint = footprint;
					dst.PlacedFootprint.Offset = bytes * i;
					D3D12_TEXTURE_COPY_LOCATION src{ .pResource = views[i].resource12.get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					src.SubresourceIndex = 0;
					a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					Transition(a_list, views[i].resource12.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
				}
			}
		}
	}

	void Raytracer::CollectResults(uint32_t a_slot)
	{
		if (sunShadowsReady)
			sunShadows.CollectResults(a_slot);
		if (pointShadowsReady)
			pointShadows.CollectResults(a_slot);
		if (skinnedReady)
			skinned.CollectResults(a_slot);
		if (!slotPending[a_slot])
			return;
		slotPending[a_slot] = false;

		const uint64_t* t = timestampCpu + kTimestampsPerSlot * a_slot;
		stats.blasBuildMs.Add(Ms(t[0], t[1], timestampFrequency));
		stats.tlasBuildMs.Add(Ms(t[1], t[2], timestampFrequency));

		const auto& info = slotInfo[a_slot];
		if (!info.debugTraced)
			return;
		const uint32_t* slotCounters = countersCpu + (kCounterBytes / sizeof(uint32_t)) * a_slot;
		for (uint32_t i = 0; i < kCounterCount; i++)
			stats.counters[i] = slotCounters[i];
		stats.traceMs.Add(Ms(t[2], t[3], timestampFrequency));

		stats.instances = info.instances;
		stats.exclusions = info.exclusions;
		stats.instancesDropped = info.dropped;
		stats.renderWidth = info.renderWidth;
		stats.renderHeight = info.renderHeight;
		stats.area = info.area;
		stats.posAdjust = info.posAdjust;
		stats.haveResult = true;
		stats.mismatchPercent.Add(stats.MismatchPercent());
		stats.coveragePercent.Add(stats.CoveragePercent());

		if (++resultsCollected % 600 == 0) {
			const auto& c = stats.counters;
			logger::info("[SkyrimRT] Trace: depth mismatch {:.2f}% over {:.1f}% coverage | pixels render {} sky {} outside {} excluded occluder {} alpha {} clutter {} counted {} matched {} nearer {} farther {} miss {} | instances {} exclusions {} | BLAS {:.3f} ms TLAS {:.3f} ms trace {:.3f} ms",
				stats.MismatchPercent(), stats.CoveragePercent(),
				c[kRenderPixels], c[kSky], c[kOutsideLoaded], c[kExcluded], c[kExcludedAlpha], c[kExcludedClutter], c[kCounted], c[kMatched], c[kTracedNearer], c[kTracedFarther], c[kTracedMiss],
				stats.instances, stats.exclusions,
				stats.blasBuildMs.Average(), stats.tlasBuildMs.Average(), stats.traceMs.Average());
		}
	}

	void Raytracer::ReadDumpImages(std::vector<DumpImage>& a_out)
	{
		if (sunShadowsReady)
			sunShadows.ReadDumpImages(a_out);
		if (pointShadowsReady)
			pointShadows.ReadDumpImages(a_out);
		if (!dumpCaptured || !dumpReadback || !dumpWidth || !dumpHeight)
			return;
		dumpCaptured = false;
		const auto viewDesc = views[0].resource12->GetDesc();
		UINT64 bytes = 0;
		device->GetCopyableFootprints(&viewDesc, 0, 1, 0, nullptr, nullptr, nullptr, &bytes);
		bytes = Align(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

		void* mapped = nullptr;
		if (FAILED(dumpReadback->Map(0, nullptr, &mapped)))
			return;
		constexpr const char* kNames[] = { "depth", "instance", "normal", "diff" };
		for (uint32_t i = 0; i < views.size(); i++) {
			DumpImage image{ kNames[i], dumpWidth, dumpHeight, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<uint8_t>(static_cast<size_t>(dumpWidth) * dumpHeight * 4) };
			const auto* source = static_cast<const uint8_t*>(mapped) + bytes * i;
			for (uint32_t y = 0; y < dumpHeight; y++)
				std::memcpy(image.pixels.data() + static_cast<size_t>(y) * dumpWidth * 4, source + static_cast<size_t>(y) * dumpRowPitch, static_cast<size_t>(dumpWidth) * 4);
			a_out.push_back(std::move(image));
		}
		D3D12_RANGE noWrite{ 0, 0 };
		dumpReadback->Unmap(0, &noWrite);
	}
}
