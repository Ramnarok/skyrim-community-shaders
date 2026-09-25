#include "NrdDenoiser.h"

#if defined(SKYRIMRT_NRD)

#	include "BufferPool.h"
#	include "RT.h"

namespace RT
{
	namespace
	{
		DXGI_FORMAT ToDxgi(nrd::Format a_format)
		{
			using F = nrd::Format;
			switch (a_format) {
			case F::R8_UNORM:
				return DXGI_FORMAT_R8_UNORM;
			case F::R8_SNORM:
				return DXGI_FORMAT_R8_SNORM;
			case F::R8_UINT:
				return DXGI_FORMAT_R8_UINT;
			case F::R8_SINT:
				return DXGI_FORMAT_R8_SINT;
			case F::RG8_UNORM:
				return DXGI_FORMAT_R8G8_UNORM;
			case F::RG8_SNORM:
				return DXGI_FORMAT_R8G8_SNORM;
			case F::RG8_UINT:
				return DXGI_FORMAT_R8G8_UINT;
			case F::RG8_SINT:
				return DXGI_FORMAT_R8G8_SINT;
			case F::RGBA8_UNORM:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case F::RGBA8_SNORM:
				return DXGI_FORMAT_R8G8B8A8_SNORM;
			case F::RGBA8_UINT:
				return DXGI_FORMAT_R8G8B8A8_UINT;
			case F::RGBA8_SINT:
				return DXGI_FORMAT_R8G8B8A8_SINT;
			case F::RGBA8_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			case F::R16_UNORM:
				return DXGI_FORMAT_R16_UNORM;
			case F::R16_SNORM:
				return DXGI_FORMAT_R16_SNORM;
			case F::R16_UINT:
				return DXGI_FORMAT_R16_UINT;
			case F::R16_SINT:
				return DXGI_FORMAT_R16_SINT;
			case F::R16_SFLOAT:
				return DXGI_FORMAT_R16_FLOAT;
			case F::RG16_UNORM:
				return DXGI_FORMAT_R16G16_UNORM;
			case F::RG16_SNORM:
				return DXGI_FORMAT_R16G16_SNORM;
			case F::RG16_UINT:
				return DXGI_FORMAT_R16G16_UINT;
			case F::RG16_SINT:
				return DXGI_FORMAT_R16G16_SINT;
			case F::RG16_SFLOAT:
				return DXGI_FORMAT_R16G16_FLOAT;
			case F::RGBA16_UNORM:
				return DXGI_FORMAT_R16G16B16A16_UNORM;
			case F::RGBA16_SNORM:
				return DXGI_FORMAT_R16G16B16A16_SNORM;
			case F::RGBA16_UINT:
				return DXGI_FORMAT_R16G16B16A16_UINT;
			case F::RGBA16_SINT:
				return DXGI_FORMAT_R16G16B16A16_SINT;
			case F::RGBA16_SFLOAT:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case F::R32_UINT:
				return DXGI_FORMAT_R32_UINT;
			case F::R32_SINT:
				return DXGI_FORMAT_R32_SINT;
			case F::R32_SFLOAT:
				return DXGI_FORMAT_R32_FLOAT;
			case F::RG32_UINT:
				return DXGI_FORMAT_R32G32_UINT;
			case F::RG32_SINT:
				return DXGI_FORMAT_R32G32_SINT;
			case F::RG32_SFLOAT:
				return DXGI_FORMAT_R32G32_FLOAT;
			case F::RGB32_UINT:
				return DXGI_FORMAT_R32G32B32_UINT;
			case F::RGB32_SINT:
				return DXGI_FORMAT_R32G32B32_SINT;
			case F::RGB32_SFLOAT:
				return DXGI_FORMAT_R32G32B32_FLOAT;
			case F::RGBA32_UINT:
				return DXGI_FORMAT_R32G32B32A32_UINT;
			case F::RGBA32_SINT:
				return DXGI_FORMAT_R32G32B32A32_SINT;
			case F::RGBA32_SFLOAT:
				return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case F::R10_G10_B10_A2_UNORM:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case F::R10_G10_B10_A2_UINT:
				return DXGI_FORMAT_R10G10B10A2_UINT;
			case F::R11_G11_B10_UFLOAT:
				return DXGI_FORMAT_R11G11B10_FLOAT;
			case F::R9_G9_B9_E5_UFLOAT:
				return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			return barrier;
		}

		D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* a_resource)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = a_resource;
			return barrier;
		}

		constexpr auto kSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		constexpr auto kUAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	NrdDenoiser::~NrdDenoiser()
	{
		if (instance)
			nrd::DestroyInstance(*instance);
	}

	bool NrdDenoiser::Fail(std::string a_reason)
	{
		failureReason = std::move(a_reason);
		logger::error("[SkyrimRT] NRD setup failed: {}", failureReason);
		return false;
	}

	bool NrdDenoiser::Init(ID3D12Device* a_device, uint32_t a_width, uint32_t a_height, nrd::Denoiser a_denoiser)
	{
		device = a_device;
		width = a_width;
		height = a_height;
		denoiser = a_denoiser;

		const nrd::DenoiserDesc denoiserDesc{ kIdentifier, denoiser };
		nrd::InstanceCreationDesc creation{};
		creation.denoisers = &denoiserDesc;
		creation.denoisersNum = 1;
		if (const auto result = nrd::CreateInstance(creation, instance); result != nrd::Result::SUCCESS)
			return Fail(std::format("nrd::CreateInstance failed ({})", static_cast<uint32_t>(result)));

		const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance);
		tableSrvs = std::max(desc.descriptorPoolDesc.perSetTexturesMaxNum, 1u);
		tableUavs = std::max(desc.descriptorPoolDesc.perSetStorageTexturesMaxNum, 1u);

		// Root signature shared by every NRD pipeline (as NRDIntegration builds it): root CBV + static samplers in the
		// constant/sampler space, one table with an SRV range followed by a UAV range in the resource space.
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, tableSrvs, desc.resourcesBaseRegisterIndex, desc.resourcesSpaceIndex, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, tableUavs, desc.resourcesBaseRegisterIndex, desc.resourcesSpaceIndex, tableSrvs };
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[0].Descriptor = { desc.constantBufferRegisterIndex, desc.constantBufferAndSamplersSpaceIndex };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 2, ranges };
		std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(desc.samplersNum);
		for (uint32_t i = 0; i < desc.samplersNum; i++) {
			auto& s = samplers[i];
			s.Filter = desc.samplers[i] == nrd::Sampler::NEAREST_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			s.MaxLOD = D3D12_FLOAT32_MAX;
			s.ShaderRegister = desc.samplersBaseRegisterIndex + i;
			s.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
		}
		D3D12_ROOT_SIGNATURE_DESC rootDesc{ .NumParameters = 2, .pParameters = params, .NumStaticSamplers = desc.samplersNum, .pStaticSamplers = samplers.data() };
		winrt::com_ptr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put());
		if (FAILED(hr))
			return Fail(std::format("serialize NRD root signature failed ({}): {}", FormatHResult(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
		if (FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
			return Fail(std::format("CreateRootSignature(NRD) failed ({})", FormatHResult(hr)));

		pipelines.resize(desc.pipelinesNum);
		for (uint32_t i = 0; i < desc.pipelinesNum; i++) {
			const auto& shader = desc.pipelines[i].computeShaderDXIL;
			if (!shader.bytecode || !shader.size)
				return Fail(std::format("NRD pipeline {} has no DXIL (built without NRD_EMBEDS_DXIL_SHADERS?)", i));
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = rootSignature.get();
			psoDesc.CS = { shader.bytecode, static_cast<SIZE_T>(shader.size) };
			if (FAILED(hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pipelines[i].put()))))
				return Fail(std::format("CreateComputePipelineState(NRD {}) failed ({})", desc.pipelines[i].shaderIdentifier, FormatHResult(hr)));
		}

		auto createPool = [&](const nrd::TextureDesc* a_descs, uint32_t a_count, const wchar_t* a_prefix, std::vector<winrt::com_ptr<ID3D12Resource>>& a_out) {
			a_out.resize(a_count);
			for (uint32_t i = 0; i < a_count; i++) {
				D3D12_HEAP_PROPERTIES heapProps{ .Type = D3D12_HEAP_TYPE_DEFAULT };
				D3D12_RESOURCE_DESC texDesc{};
				texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				const uint32_t factor = std::max<uint32_t>(a_descs[i].downsampleFactor, 1);
				texDesc.Width = (width + factor - 1) / factor;
				texDesc.Height = (height + factor - 1) / factor;
				texDesc.DepthOrArraySize = 1;
				texDesc.MipLevels = 1;
				texDesc.Format = ToDxgi(a_descs[i].format);
				texDesc.SampleDesc = { 1, 0 };
				texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				if (texDesc.Format == DXGI_FORMAT_UNKNOWN)
					return Fail(std::format("unsupported NRD pool format {}", static_cast<uint32_t>(a_descs[i].format)));
				if (HRESULT poolHr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, kSRV, nullptr, IID_PPV_ARGS(a_out[i].put())); FAILED(poolHr))
					return Fail(std::format("creating NRD pool texture failed ({})", FormatHResult(poolHr)));
				a_out[i]->SetName(std::format(L"{}{}", a_prefix, i).c_str());
				states[a_out[i].get()] = kSRV;
			}
			return true;
		};
		if (!createPool(desc.permanentPool, desc.permanentPoolSize, L"SkyrimRT::NRDPermanent", permanentPool) ||
			!createPool(desc.transientPool, desc.transientPoolSize, L"SkyrimRT::NRDTransient", transientPool))
			return false;

		const uint32_t tableSize = tableSrvs + tableUavs;
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = kFramesInFlight * kMaxDispatchesPerFrame * tableSize,
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		if (FAILED(hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.put()))))
			return Fail(std::format("CreateDescriptorHeap(NRD) failed ({})", FormatHResult(hr)));
		descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		constantSliceBytes = (std::max<uint64_t>(desc.constantBufferMaxDataSize, 1) + 255) & ~255ull;
		if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_UPLOAD, constantSliceBytes * kMaxDispatchesPerFrame * kFramesInFlight,
					   D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, constants.put())))
			return Fail(std::format("creating NRD constant ring failed ({})", FormatHResult(hr)));
		D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		if (FAILED(constants->Map(0, &noRead, &mapped)))
			return Fail("mapping NRD constant ring failed");
		constantsCpu = static_cast<uint8_t*>(mapped);

		const auto* library = nrd::GetLibraryDesc();
		logger::info("[SkyrimRT] NRD {}.{}.{} ready: {}, {} pipelines, pools {} permanent + {} transient, {}x{}",
			library->versionMajor, library->versionMinor, library->versionBuild, nrd::GetDenoiserString(denoiser), desc.pipelinesNum, desc.permanentPoolSize,
			desc.transientPoolSize, width, height);
		ready = true;
		return true;
	}

	ID3D12Resource* NrdDenoiser::ResolveResource(const nrd::ResourceDesc& a_desc, const Resources& a_resources) const
	{
		using T = nrd::ResourceType;
		switch (a_desc.type) {
		case T::TRANSIENT_POOL:
			return a_desc.indexInPool < transientPool.size() ? transientPool[a_desc.indexInPool].get() : nullptr;
		case T::PERMANENT_POOL:
			return a_desc.indexInPool < permanentPool.size() ? permanentPool[a_desc.indexInPool].get() : nullptr;
		case T::IN_MV:
			return a_resources.motionVectors;
		case T::IN_NORMAL_ROUGHNESS:
			return a_resources.normalRoughness;
		case T::IN_VIEWZ:
			return a_resources.viewZ;
		case T::IN_DIFF_RADIANCE_HITDIST:
		case T::IN_SPEC_RADIANCE_HITDIST:
		case T::IN_DIFF_HITDIST:  // M9 phase 5: REBLUR_DIFFUSE_OCCLUSION
			return a_resources.radianceHitDist;
		case T::OUT_DIFF_RADIANCE_HITDIST:
		case T::OUT_SPEC_RADIANCE_HITDIST:
		case T::OUT_DIFF_HITDIST:
			return a_resources.outRadianceHitDist;
		default:
			return nullptr;
		}
	}

	void NrdDenoiser::Record(ID3D12GraphicsCommandList* a_list, uint32_t a_slot, const nrd::CommonSettings& a_common,
		const nrd::ReblurSettings& a_settings, const Resources& a_resources)
	{
		lastDispatchCount = 0;
		if (!ready || nrd::SetCommonSettings(*instance, a_common) != nrd::Result::SUCCESS || nrd::SetDenoiserSettings(*instance, kIdentifier, &a_settings) != nrd::Result::SUCCESS)
			return;
		const nrd::DispatchDesc* dispatches = nullptr;
		uint32_t dispatchCount = 0;
		if (nrd::GetComputeDispatches(*instance, &kIdentifier, 1, dispatches, dispatchCount) != nrd::Result::SUCCESS)
			return;

		for (auto* io : { a_resources.motionVectors, a_resources.normalRoughness, a_resources.viewZ, a_resources.radianceHitDist, a_resources.outRadianceHitDist })
			states[io] = kSRV;

		const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance);
		const uint32_t tableSize = tableSrvs + tableUavs;
		ID3D12DescriptorHeap* heaps[] = { heap.get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetComputeRootSignature(rootSignature.get());

		D3D12_GPU_VIRTUAL_ADDRESS previousConstants = 0;
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		for (uint32_t d = 0; d < dispatchCount && d < kMaxDispatchesPerFrame; d++) {
			const auto& dispatch = dispatches[d];
			const auto& pipeline = desc.pipelines[dispatch.pipelineIndex];
			const uint32_t table = (a_slot * kMaxDispatchesPerFrame + d) * tableSize;
			auto cpuHandle = [&](uint32_t a_index) {
				auto h = heap->GetCPUDescriptorHandleForHeapStart();
				h.ptr += static_cast<SIZE_T>(table + a_index) * descriptorSize;
				return h;
			};

			barriers.clear();
			uint32_t resourceIndex = 0;
			uint32_t srvCount = 0;
			uint32_t uavCount = 0;
			for (uint32_t r = 0; r < pipeline.resourceRangesNum; r++) {
				const auto& range = pipeline.resourceRanges[r];
				const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
				for (uint32_t j = 0; j < range.descriptorsNum; j++, resourceIndex++) {
					ID3D12Resource* resource = ResolveResource(dispatch.resources[resourceIndex], a_resources);
					const auto format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R8_UNORM;
					if (storage) {
						D3D12_UNORDERED_ACCESS_VIEW_DESC uav{ .Format = format, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };
						device->CreateUnorderedAccessView(resource, nullptr, &uav, cpuHandle(tableSrvs + uavCount++));
					} else {
						D3D12_SHADER_RESOURCE_VIEW_DESC srv{ .Format = format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
						srv.Texture2D.MipLevels = 1;
						device->CreateShaderResourceView(resource, &srv, cpuHandle(srvCount++));
					}
					if (!resource)
						continue;
					auto& state = states[resource];
					const auto wanted = storage ? kUAV : kSRV;
					if (state != wanted)
						barriers.push_back(TransitionBarrier(resource, state, wanted));
					else if (storage)
						barriers.push_back(UavBarrier(resource));  // consecutive writes by different dispatches
					state = wanted;
				}
			}
			if (!barriers.empty())
				a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

			if (dispatch.constantBufferDataSize && !(dispatch.constantBufferDataMatchesPreviousDispatch && previousConstants)) {
				const uint64_t offset = (static_cast<uint64_t>(a_slot) * kMaxDispatchesPerFrame + d) * constantSliceBytes;
				std::memcpy(constantsCpu + offset, dispatch.constantBufferData, dispatch.constantBufferDataSize);
				previousConstants = constants->GetGPUVirtualAddress() + offset;
			}

			a_list->SetPipelineState(pipelines[dispatch.pipelineIndex].get());
			if (previousConstants)
				a_list->SetComputeRootConstantBufferView(0, previousConstants);
			auto gpuHandle = heap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<UINT64>(table) * descriptorSize;
			a_list->SetComputeRootDescriptorTable(1, gpuHandle);
			a_list->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
			lastDispatchCount++;
		}

		// The application's resources go back to NON_PIXEL_SHADER_RESOURCE; pools stay where they are (tracked).
		barriers.clear();
		for (auto* io : { a_resources.motionVectors, a_resources.normalRoughness, a_resources.viewZ, a_resources.radianceHitDist, a_resources.outRadianceHitDist }) {
			auto& state = states[io];
			if (state != kSRV)
				barriers.push_back(TransitionBarrier(io, state, kSRV));
			state = kSRV;
		}
		if (!barriers.empty())
			a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	}
}

#endif
