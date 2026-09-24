#include "SunShadows.h"

#include "AlphaAtlas.h"
#include "BufferPool.h"
#include "SkinnedMeshes.h"

#include <fstream>

namespace RT
{
	namespace
	{
		// Must match ShadowConstants in SunShadowCommon.hlsli (cbuffer packing).
		struct alignas(16) ShadowConstants
		{
			float viewProjInverse[16];
			float viewProj[16];
			float prevViewProj[16];
			float toSun[4];
			float posAdjustDelta[4];
			uint32_t renderSize[2];
			uint32_t prevRenderSize[2];
			uint32_t frameIndex;
			uint32_t historyValid;
			uint32_t casterMask;
			uint32_t flags;
			float normalBias;
			float distanceBias;
			float maxDistance;
			float maxHistory;
			float depthTolerance;
			float spatialRadius;
			float planeTolerance;
			float compareDistance;
			uint32_t viewMode;
			uint32_t pointLightCount;  // M8 point-light variant
			uint32_t inverseSquare;
			uint32_t pad;
		};
		static_assert(sizeof(ShadowConstants) == 304);

		constexpr uint32_t kFlagCompareShadowMap = 1;

		// InstanceMask bits, as assigned in Raytracer::Record.
		constexpr uint32_t kMaskStatic = 0x01;
		constexpr uint32_t kMaskTerrain = 0x02;
		constexpr uint32_t kMaskAlphaTested = 0x08;
		constexpr uint32_t kMaskActor = 0x04;  // M7 skinned

		constexpr uint64_t kConstantsOffset = 0;
		constexpr uint64_t kZeroOffset = 512;
		constexpr uint64_t kPointLightsOffset = 1024;  // M8: this frame's lights (root SRV t6)
		constexpr uint64_t kUploadBytes = kPointLightsOffset + sizeof(PointLight) * SunShadows::kMaxPointLights;
		constexpr uint64_t kCounterBytes = 64;
		constexpr uint32_t kTimestampsPerSlot = 4;

		// Descriptor tables: t1..t4 then u0..u1 (unused entries hold null descriptors).
		constexpr uint32_t kTableSrvs = 4;
		constexpr uint32_t kTableUavs = 2;
		constexpr uint32_t kTableSize = kTableSrvs + kTableUavs;
		constexpr uint32_t kTraceTable = 0;
		constexpr uint32_t kTemporalTable = 1;  // + parity
		constexpr uint32_t kSpatialTable = 3;   // + parity
		constexpr uint32_t kTableCount = 5;
		// M7c, after the tables: the static mesh-pool pages (t0, space1) and the alpha atlas (t0, space2).
		constexpr uint32_t kMeshPageSlots = 64;  // MeshPages[] in MeshData.hlsli
		constexpr uint32_t kFirstPageDescriptor = kTableCount * kTableSize;
		constexpr uint32_t kAlphaAtlasDescriptor = kFirstPageDescriptor + kMeshPageSlots;
		constexpr uint32_t kDescriptorCount = kAlphaAtlasDescriptor + 1;

		constexpr auto kSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		constexpr auto kUAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		constexpr auto kCommon = D3D12_RESOURCE_STATE_COMMON;
		constexpr auto kCopySource = D3D12_RESOURCE_STATE_COPY_SOURCE;

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

		template <size_t N>
		void Barriers(ID3D12GraphicsCommandList* a_list, const D3D12_RESOURCE_BARRIER (&a_barriers)[N])
		{
			a_list->ResourceBarrier(static_cast<UINT>(N), a_barriers);
		}

		float Ms(uint64_t a_begin, uint64_t a_end, uint64_t a_frequency)
		{
			return (a_end > a_begin && a_frequency) ? static_cast<float>(static_cast<double>(a_end - a_begin) * 1000.0 / static_cast<double>(a_frequency)) : 0.0f;
		}
	}

	bool SunShadows::Fail(std::string a_reason)
	{
		failureReason = std::move(a_reason);
		logger::error("[SkyrimRT] {} setup failed: {}", kind == ShadowKind::kSun ? "Sun shadows" : "Point-light shadows", failureReason);
		return false;
	}

	std::wstring SunShadows::ResourceName(const wchar_t* a_suffix) const
	{
		return std::wstring(kind == ShadowKind::kSun ? L"SkyrimRT::SunShadow" : L"SkyrimRT::PointLightShadow") + a_suffix;
	}

	bool SunShadows::CreateTexture(DXGI_FORMAT a_format, const wchar_t* a_name, winrt::com_ptr<ID3D12Resource>& a_out)
	{
		D3D12_HEAP_PROPERTIES heapProps{ .Type = D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		// Internal textures rest in the SRV state between passes.
		if (HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, kSRV, nullptr, IID_PPV_ARGS(a_out.put())); FAILED(hr))
			return Fail(std::format("creating {} failed ({})", stl::utf16_to_utf8(a_name).value_or("texture"s), FormatHResult(hr)));
		a_out->SetName(a_name);
		return true;
	}

	bool SunShadows::CreatePipelines()
	{
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTableSrvs, 1, 0, 0 };           // t1..t4
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kTableUavs, 0, 0, kTableSrvs };  // u0..u1

		// M7c alpha test (trace only): mesh pages then the atlas, one table.
		D3D12_DESCRIPTOR_RANGE alphaRanges[2]{};
		alphaRanges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMeshPageSlots, 0, 1, 0 };  // t0, space1: mesh pages
		alphaRanges[1] = GetAlphaAtlasRange(kMeshPageSlots);                          // t0, space2: alpha atlas

		D3D12_ROOT_PARAMETER params[7]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0
		params[0].Descriptor = { 0, 0 };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t0: TLAS
		params[1].Descriptor = { 0, 0 };
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u4: counters
		params[2].Descriptor = { 4, 0 };
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[3].DescriptorTable = { 2, ranges };
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t5: instance data
		params[4].Descriptor = { 5, 0 };
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[5].DescriptorTable = { 2, alphaRanges };
		params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t6: M8 point lights
		params[6].Descriptor = { 6, 0 };
		for (auto& param : params)
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		const D3D12_STATIC_SAMPLER_DESC sampler = GetAlphaAtlasSampler();
		D3D12_ROOT_SIGNATURE_DESC rootDesc{ .NumParameters = 7, .pParameters = params, .NumStaticSamplers = 1, .pStaticSamplers = &sampler };
		winrt::com_ptr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put());
		if (FAILED(hr))
			return Fail(std::format("serialize shadow root signature failed ({}): {}", FormatHResult(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
		if (FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
			return Fail(std::format("CreateRootSignature(shadows) failed ({})", FormatHResult(hr)));

		auto create = [&](const char* a_file, const std::wstring& a_name, winrt::com_ptr<ID3D12PipelineState>& a_out) {
			// Compiled at build time by DXC (cmake/SkyrimRTShaders.cmake).
			const auto path = std::filesystem::path("Data\\Shaders\\SkyrimRT") / a_file;
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return Fail(std::format("cannot open {}", path.string()));
			const std::vector<char> bytecode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = rootSignature.get();
			psoDesc.CS = { bytecode.data(), bytecode.size() };
			if (HRESULT psoHr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(a_out.put())); FAILED(psoHr))
				return Fail(std::format("CreateComputePipelineState({}) failed ({})", a_file, FormatHResult(psoHr)));
			a_out->SetName(a_name.c_str());
			return true;
		};
		return create(kind == ShadowKind::kSun ? "SunShadowTraceCS.cso" : "PointLightShadowTraceCS.cso", ResourceName(L"TracePSO"), tracePipeline) &&
		       create("SunShadowTemporalCS.cso", ResourceName(L"TemporalPSO"), temporalPipeline) &&
		       create("SunShadowSpatialCS.cso", ResourceName(L"SpatialPSO"), spatialPipeline);
	}

	bool SunShadows::CreateDescriptors()
	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = kDescriptorCount, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		if (HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.put())); FAILED(hr))
			return Fail(std::format("CreateDescriptorHeap(shadows) failed ({})", FormatHResult(hr)));
		descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		auto handle = [&](uint32_t a_table, uint32_t a_entry) {
			auto h = heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<SIZE_T>(a_table * kTableSize + a_entry) * descriptorSize;
			return h;
		};
		auto srv = [&](uint32_t a_table, uint32_t a_register, ID3D12Resource* a_resource, DXGI_FORMAT a_format) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(a_resource, &desc, handle(a_table, a_register - 1));
		};
		auto uav = [&](uint32_t a_table, uint32_t a_register, ID3D12Resource* a_resource, DXGI_FORMAT a_format) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = a_format;
			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(a_resource, nullptr, &desc, handle(a_table, kTableSrvs + a_register));
		};

		constexpr auto kR8 = DXGI_FORMAT_R8_UNORM;
		constexpr auto kR32 = DXGI_FORMAT_R32_FLOAT;
		constexpr auto kRGBA16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
		constexpr auto kRGBA8 = DXGI_FORMAT_R8G8B8A8_UNORM;

		// Null descriptors first, so every table entry is valid even where a pass doesn't declare the register.
		for (uint32_t table = 0; table < kTableCount; table++) {
			for (uint32_t t = 1; t <= kTableSrvs; t++)
				srv(table, t, nullptr, kR8);
			for (uint32_t u = 0; u < kTableUavs; u++)
				uav(table, u, nullptr, kR8);
		}

		// Trace: t1 depth, t2 game shadow mask (sun only; null for point lights); u0 raw visibility, u1 geometry.
		srv(kTraceTable, 1, rasterDepth, kR32);
		srv(kTraceTable, 2, gameShadowMask.resource12.get(), kR8);
		uav(kTraceTable, 0, rawVisibility.get(), kR8);
		uav(kTraceTable, 1, geometry.get(), kRGBA16);

		for (uint32_t p = 0; p < 2; p++) {
			// Temporal: t1 depth, t2 raw, t3 geometry, t4 previous history; u0 current history.
			srv(kTemporalTable + p, 1, rasterDepth, kR32);
			srv(kTemporalTable + p, 2, rawVisibility.get(), kR8);
			srv(kTemporalTable + p, 3, geometry.get(), kRGBA16);
			srv(kTemporalTable + p, 4, history[1 - p].get(), kRGBA16);
			uav(kTemporalTable + p, 0, history[p].get(), kRGBA16);

			// Spatial: t1 depth, t2 current history, t3 geometry, t4 raw; u0 mask, u1 debug view.
			srv(kSpatialTable + p, 1, rasterDepth, kR32);
			srv(kSpatialTable + p, 2, history[p].get(), kRGBA16);
			srv(kSpatialTable + p, 3, geometry.get(), kRGBA16);
			srv(kSpatialTable + p, 4, rawVisibility.get(), kR8);
			uav(kSpatialTable + p, 0, mask.resource12.get(), kR8);
			uav(kSpatialTable + p, 1, view.resource12.get(), kRGBA8);
		}

		// M7c: mesh pages start null (filled per frame by UpdatePageDescriptors), then the atlas.
		D3D12_SHADER_RESOURCE_VIEW_DESC nullPage{ .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
		nullPage.Buffer.NumElements = 1;
		nullPage.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		auto flat = [&](uint32_t a_index) {
			auto h = heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<SIZE_T>(a_index) * descriptorSize;
			return h;
		};
		for (uint32_t i = 0; i < kMeshPageSlots; i++)
			device->CreateShaderResourceView(nullptr, &nullPage, flat(kFirstPageDescriptor + i));
		WriteAlphaAtlasDescriptor(device, alphaAtlas, flat(kAlphaAtlasDescriptor));
		return true;
	}

	bool SunShadows::Init(ID3D12Device5* a_device, ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context,
		uint32_t a_width, uint32_t a_height, ID3D12Resource* a_rasterDepth, ID3D11ComputeShader* a_copyCS, ID3D12Resource* a_alphaAtlas,
		ShadowKind a_kind)
	{
		kind = a_kind;
		alphaAtlas = a_alphaAtlas;
		device = a_device;
		d3d11Device = a_d3d11Device;
		d3d11Context = a_d3d11Context;
		width = a_width;
		height = a_height;
		rasterDepth = a_rasterDepth;
		copyCS = a_copyCS;

		const bool sun = kind == ShadowKind::kSun;
		std::string error;
		if (!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R8_UNORM, sun ? "SunShadowMask" : "PointLightShadowMask", mask, error) ||
			!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, sun ? "SunShadowView" : "PointLightShadowView", view, error) ||
			(sun && !CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R8_UNORM, "GameShadowMaskCopy", gameShadowMask, error)))
			return Fail(std::move(error));
		ClearMask();

		if (!CreateTexture(DXGI_FORMAT_R8_UNORM, ResourceName(L"Raw").c_str(), rawVisibility) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceName(L"Geometry").c_str(), geometry) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceName(L"History0").c_str(), history[0]) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceName(L"History1").c_str(), history[1]))
			return false;

		if (!CreatePipelines() || !CreateDescriptors())
			return false;

		HRESULT hr;
		constexpr auto kUavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, kCounterBytes, kCommon, kUavFlag, counters.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, kCounterBytes * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, countersReadback.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t) * kTimestampsPerSlot * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, timestampReadback.put())))
			return Fail(std::format("creating shadow counter buffers failed ({})", FormatHResult(hr)));
		counters->SetName(ResourceName(L"Counters").c_str());

		for (uint32_t i = 0; i < kFramesInFlight; i++) {
			if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_UPLOAD, kUploadBytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, uploads[i].put())))
				return Fail(std::format("creating shadow upload buffer failed ({})", FormatHResult(hr)));
			uploads[i]->SetName(ResourceName(L"Upload").c_str());
			D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			if (FAILED(uploads[i]->Map(0, &noRead, &mapped)))
				return Fail("mapping shadow upload buffer failed");
			uploadCpu[i] = static_cast<uint8_t*>(mapped);
			std::memset(uploadCpu[i] + kZeroOffset, 0, kCounterBytes);
		}

		void* mapped = nullptr;
		if (FAILED(countersReadback->Map(0, nullptr, &mapped)))
			return Fail("mapping shadow counter readback failed");
		countersCpu = static_cast<const uint32_t*>(mapped);
		if (FAILED(timestampReadback->Map(0, nullptr, &mapped)))
			return Fail("mapping shadow timestamp readback failed");
		timestampCpu = static_cast<const uint64_t*>(mapped);

		D3D12_QUERY_HEAP_DESC queryDesc{ .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = kTimestampsPerSlot * kFramesInFlight };
		if (FAILED(hr = device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(timestamps.put()))))
			return Fail(std::format("CreateQueryHeap(shadows) failed ({})", FormatHResult(hr)));

		logger::info("[SkyrimRT] {} ready: {}x{} mask", sun ? "Sun shadows" : "Point-light shadows", width, height);
		return true;
	}

	void SunShadows::ClearMask()
	{
		constexpr float kLit[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		d3d11Context->ClearUnorderedAccessViewFloat(mask.uav11.get(), kLit);
	}

	void SunShadows::CopyInputs(bool a_compareShadowMap)
	{
		if (!a_compareShadowMap || kind != ShadowKind::kSun)
			return;
		auto* renderer = globals::game::renderer;
		ID3D11ShaderResourceView* srv = renderer ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].SRV : nullptr;
		if (!srv)
			return;

		auto* ctx = d3d11Context;
		ID3D11UnorderedAccessView* uav = gameShadowMask.uav11.get();
		ctx->CSSetShader(copyCS, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &srv);
		ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetShaderResources(0, 1, &nullSRV);
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ctx->CSSetShader(nullptr, nullptr, 0);
	}

	void SunShadows::Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
		const BufferPool& a_meshPool, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const SunShadowParams& a_params,
		bool a_compareShadowMap, bool a_captureDump)
	{
		PassSettings settings;
		std::memcpy(settings.toSun, a_params.toSun, sizeof(settings.toSun));
		settings.tanHalfAngle = std::tan(std::clamp(a_params.coneHalfAngleDegrees, 0.0f, 10.0f) * std::numbers::pi_v<float> / 180.0f);
		settings.alphaTestedCasters = a_params.alphaTestedCasters;
		settings.normalBias = a_params.normalBias;
		settings.distanceBias = a_params.distanceBias;
		settings.maxHistory = a_params.maxHistory;
		settings.spatialRadius = a_params.spatialRadius;
		settings.viewMode = a_params.viewMode;
		RecordPasses(a_list, a_slot, a_tlas, a_instances, a_meshPool, a_camera, a_renderWidth, a_renderHeight, settings, a_compareShadowMap, a_captureDump);
		stats.toSun[0] = a_params.toSun[0];
		stats.toSun[1] = a_params.toSun[1];
		stats.toSun[2] = a_params.toSun[2];
		stats.coneHalfAngleDegrees = a_params.coneHalfAngleDegrees;
	}

	void SunShadows::RecordPointLights(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
		const BufferPool& a_meshPool, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const PointShadowParams& a_params,
		bool a_captureDump)
	{
		PassSettings settings;
		settings.pointLightCount = static_cast<uint32_t>(std::min<size_t>(a_params.lights.size(), kMaxPointLights));
		if (settings.pointLightCount)
			std::memcpy(uploadCpu[a_slot] + kPointLightsOffset, a_params.lights.data(), sizeof(PointLight) * settings.pointLightCount);
		settings.inverseSquare = a_params.inverseSquare;
		settings.alphaTestedCasters = a_params.alphaTestedCasters;
		settings.normalBias = a_params.normalBias;
		settings.distanceBias = a_params.distanceBias;
		settings.maxHistory = a_params.maxHistory;
		settings.spatialRadius = a_params.spatialRadius;
		settings.viewMode = a_params.viewMode;
		RecordPasses(a_list, a_slot, a_tlas, a_instances, a_meshPool, a_camera, a_renderWidth, a_renderHeight, settings, false, a_captureDump);
		// Which lights the trace covers (PointLightShadowTraceCS's kSkippedLights; Disabled counts as traced here).
		constexpr uint32_t kPortalStrict = 1u << 0;  // LightLimitFix::LightFlags
		constexpr uint32_t kShadow = 1u << 1;
		stats.pointLights = settings.pointLightCount;
		stats.pointLightsShadowMapped = stats.pointLightsPortalStrict = stats.pointLightsTraced = 0;
		for (uint32_t i = 0; i < settings.pointLightCount; i++) {
			const uint32_t flags = a_params.lights[i].flags;
			if (flags & kShadow)
				stats.pointLightsShadowMapped++;
			else if (flags & kPortalStrict)
				stats.pointLightsPortalStrict++;
			else
				stats.pointLightsTraced++;
		}
	}

	void SunShadows::RecordPasses(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
		const BufferPool& a_meshPool, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const PassSettings& a_settings,
		bool a_compareShadowMap, bool a_captureDump)
	{
		const bool historyValid = haveHistory && a_camera.gameFrame == historyGameFrame + 1;
		if (!historyValid)
			stats.historyResets++;
		parity ^= 1;
		frameIndex++;

		auto* constants = reinterpret_cast<ShadowConstants*>(uploadCpu[a_slot] + kConstantsOffset);
		std::memcpy(constants->viewProjInverse, a_camera.viewProjInverse, sizeof(constants->viewProjInverse));
		std::memcpy(constants->viewProj, a_camera.viewProj, sizeof(constants->viewProj));
		std::memcpy(constants->prevViewProj, prevViewProj, sizeof(constants->prevViewProj));
		constants->toSun[0] = a_settings.toSun[0];
		constants->toSun[1] = a_settings.toSun[1];
		constants->toSun[2] = a_settings.toSun[2];
		constants->toSun[3] = a_settings.tanHalfAngle;
		constants->posAdjustDelta[0] = a_camera.posAdjust.x - prevPosAdjust.x;
		constants->posAdjustDelta[1] = a_camera.posAdjust.y - prevPosAdjust.y;
		constants->posAdjustDelta[2] = a_camera.posAdjust.z - prevPosAdjust.z;
		constants->posAdjustDelta[3] = 0.0f;
		constants->renderSize[0] = a_renderWidth;
		constants->renderSize[1] = a_renderHeight;
		constants->prevRenderSize[0] = prevRenderWidth;
		constants->prevRenderSize[1] = prevRenderHeight;
		constants->frameIndex = frameIndex;
		constants->historyValid = historyValid ? 1u : 0u;
		constants->casterMask = kMaskStatic | kMaskTerrain | kMaskActor | (a_settings.alphaTestedCasters ? kMaskAlphaTested : 0u);
		constants->flags = a_compareShadowMap ? kFlagCompareShadowMap : 0u;
		constants->normalBias = a_settings.normalBias;
		constants->distanceBias = a_settings.distanceBias;
		constants->maxDistance = kMaxRayDistance;
		constants->maxHistory = static_cast<float>(std::max(a_settings.maxHistory, 1u));
		constants->depthTolerance = kDepthTolerance;
		constants->spatialRadius = std::max(a_settings.spatialRadius, 0.0f);
		constants->planeTolerance = kPlaneTolerance;
		constants->compareDistance = kCompareDistance;
		constants->viewMode = a_settings.viewMode;
		constants->pointLightCount = a_settings.pointLightCount;
		constants->inverseSquare = a_settings.inverseSquare ? 1u : 0u;

		const D3D12_GPU_VIRTUAL_ADDRESS uploadVA = uploads[a_slot]->GetGPUVirtualAddress();
		auto table = [&](uint32_t a_table) {
			auto h = heap->GetGPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<UINT64>(a_table * kTableSize) * descriptorSize;
			return h;
		};
		const uint32_t groupsX = (a_renderWidth + 7) / 8;
		const uint32_t groupsY = (a_renderHeight + 7) / 8;
		const uint32_t query = a_slot * kTimestampsPerSlot;

		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 0);

		ID3D12DescriptorHeap* heaps[] = { heap.get() };
		a_list->SetDescriptorHeaps(1, heaps);
		a_list->SetComputeRootSignature(rootSignature.get());
		a_list->SetComputeRootConstantBufferView(0, uploadVA + kConstantsOffset);
		a_list->SetComputeRootShaderResourceView(1, a_tlas);
		a_list->SetComputeRootUnorderedAccessView(2, counters->GetGPUVirtualAddress());
		a_list->SetComputeRootShaderResourceView(6, uploadVA + kPointLightsOffset);  // read only by the point-light trace
		// M7c: the alpha test reads indices and UVs from the static pool only (skinned meshes' bind-pose sources live
		// there too), so the skinned output pages stay null.
		{
			auto first = heap->GetCPUDescriptorHandleForHeapStart();
			first.ptr += static_cast<SIZE_T>(kFirstPageDescriptor) * descriptorSize;
			UpdatePageDescriptors(device, a_meshPool, first, descriptorSize, describedPageSerials.data(), SkinnedMeshes::kFirstPageSlot);
			auto pageTable = heap->GetGPUDescriptorHandleForHeapStart();
			pageTable.ptr += static_cast<UINT64>(kFirstPageDescriptor) * descriptorSize;
			a_list->SetComputeRootShaderResourceView(4, a_instances);
			a_list->SetComputeRootDescriptorTable(5, pageTable);
		}

		// Counters: zero, then UAV.
		Barriers(a_list, { TransitionBarrier(counters.get(), kCommon, D3D12_RESOURCE_STATE_COPY_DEST) });
		a_list->CopyBufferRegion(counters.get(), 0, uploads[a_slot].get(), kZeroOffset, kCounterBytes);

		// 1. Trace.
		const bool sun = kind == ShadowKind::kSun;
		SetPassMarker(a_list, sun ? L"SkyrimRT: sun shadow trace" : L"SkyrimRT: point-light shadow trace");
		ID3D12Resource* gameMask = gameShadowMask.resource12.get();  // sun only
		Barriers(a_list, { TransitionBarrier(counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, kUAV),
							 TransitionBarrier(rasterDepth, kCommon, kSRV),
							 TransitionBarrier(rawVisibility.get(), kSRV, kUAV),
							 TransitionBarrier(geometry.get(), kSRV, kUAV) });
		if (gameMask)
			Barriers(a_list, { TransitionBarrier(gameMask, kCommon, kSRV) });
		a_list->SetPipelineState(tracePipeline.get());
		a_list->SetComputeRootDescriptorTable(3, table(kTraceTable));
		a_list->Dispatch(groupsX, groupsY, 1);
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);

		// 2. Temporal accumulation into history[parity].
		SetPassMarker(a_list, sun ? L"SkyrimRT: sun shadow temporal" : L"SkyrimRT: point-light shadow temporal");
		Barriers(a_list, { TransitionBarrier(rawVisibility.get(), kUAV, kSRV),
							 TransitionBarrier(geometry.get(), kUAV, kSRV),
							 TransitionBarrier(history[parity].get(), kSRV, kUAV) });
		a_list->SetPipelineState(temporalPipeline.get());
		a_list->SetComputeRootDescriptorTable(3, table(kTemporalTable + parity));
		a_list->Dispatch(groupsX, groupsY, 1);
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2);

		// 3. Spatial filter into the shared mask (and debug view).
		SetPassMarker(a_list, sun ? L"SkyrimRT: sun shadow spatial" : L"SkyrimRT: point-light shadow spatial");
		Barriers(a_list, { TransitionBarrier(history[parity].get(), kUAV, kSRV),
							 TransitionBarrier(mask.resource12.get(), kCommon, kUAV),
							 TransitionBarrier(view.resource12.get(), kCommon, kUAV) });
		a_list->SetPipelineState(spatialPipeline.get());
		a_list->SetComputeRootDescriptorTable(3, table(kSpatialTable + parity));
		a_list->Dispatch(groupsX, groupsY, 1);
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 3);

		// Shared textures back to COMMON before the fence signal, so D3D11 sees the writes.
		Barriers(a_list, { TransitionBarrier(mask.resource12.get(), kUAV, kCommon),
							 TransitionBarrier(view.resource12.get(), kUAV, kCommon),
							 TransitionBarrier(rasterDepth, kSRV, kCommon),
							 TransitionBarrier(counters.get(), kUAV, kCopySource) });
		if (gameMask)
			Barriers(a_list, { TransitionBarrier(gameMask, kSRV, kCommon) });
		a_list->CopyBufferRegion(countersReadback.get(), kCounterBytes * a_slot, counters.get(), 0, kCounterBytes);
		Barriers(a_list, { TransitionBarrier(counters.get(), kCopySource, kCommon) });
		a_list->ResolveQueryData(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query, kTimestampsPerSlot, timestampReadback.get(), sizeof(uint64_t) * query);

		if (a_captureDump) {
			const auto rawDesc = rawVisibility->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			device->GetCopyableFootprints(&rawDesc, 0, 1, 0, &footprint, nullptr, nullptr, &dumpImageBytes);
			dumpImageBytes = (dumpImageBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
			if (!dumpReadback && FAILED(CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, dumpImageBytes * 3, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, dumpReadback.put()))) {
				logger::error("[SkyrimRT] Debug dump: cannot create shadow readback buffer");
			} else {
				dumpRowPitch = footprint.Footprint.RowPitch;
				dumpWidth = a_renderWidth;
				dumpHeight = a_renderHeight;
				dumpHasComparison = a_compareShadowMap;
				dumpCaptured = true;
				struct Source
				{
					ID3D12Resource* resource;
					D3D12_RESOURCE_STATES rest;
				};
				const Source sources[3] = { { rawVisibility.get(), kSRV }, { mask.resource12.get(), kCommon }, { gameMask, kCommon } };
				for (uint32_t i = 0; i < (gameMask ? 3u : 2u); i++) {
					Barriers(a_list, { TransitionBarrier(sources[i].resource, sources[i].rest, kCopySource) });
					D3D12_TEXTURE_COPY_LOCATION dst{ .pResource = dumpReadback.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
					dst.PlacedFootprint = footprint;
					dst.PlacedFootprint.Offset = dumpImageBytes * i;
					D3D12_TEXTURE_COPY_LOCATION src{ .pResource = sources[i].resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					src.SubresourceIndex = 0;
					a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					Barriers(a_list, { TransitionBarrier(sources[i].resource, kCopySource, sources[i].rest) });
				}
			}
		}

		slotPending[a_slot] = true;
		slotCompared[a_slot] = a_compareShadowMap;
		stats.textureWidth = width;
		stats.textureHeight = height;
		stats.renderWidth = a_renderWidth;
		stats.renderHeight = a_renderHeight;
		stats.framesTraced++;

		haveHistory = true;
		historyGameFrame = a_camera.gameFrame;
		std::memcpy(prevViewProj, a_camera.viewProj, sizeof(prevViewProj));
		prevPosAdjust = a_camera.posAdjust;
		prevRenderWidth = a_renderWidth;
		prevRenderHeight = a_renderHeight;
	}

	void SunShadows::CollectResults(uint32_t a_slot)
	{
		if (!slotPending[a_slot])
			return;
		slotPending[a_slot] = false;

		const uint32_t* slotCounters = countersCpu + (kCounterBytes / sizeof(uint32_t)) * a_slot;
		for (uint32_t i = 0; i < kShadowCounterCount; i++)
			stats.counters[i] = slotCounters[i];
		if (slotCompared[a_slot]) {
			stats.comparedCounters = stats.counters;
			stats.haveComparison = true;
		}
		const uint64_t* t = timestampCpu + kTimestampsPerSlot * a_slot;
		stats.traceMs.Add(Ms(t[0], t[1], timestampFrequency));
		stats.temporalMs.Add(Ms(t[1], t[2], timestampFrequency));
		stats.spatialMs.Add(Ms(t[2], t[3], timestampFrequency));
		stats.totalMs.Add(Ms(t[0], t[3], timestampFrequency));
		stats.shadowedPercent.Add(stats.ShadowedPercent());
		stats.haveResult = true;

		if (stats.framesTraced % 600 == 0) {
			if (kind == ShadowKind::kSun) {
				logger::info("[SkyrimRT] Sun shadows: {:.1f}% of {} pixels shadowed | trace {:.3f} ms temporal {:.3f} ms spatial {:.3f} ms total {:.3f} ms | history resets {}",
					stats.ShadowedPercent(), stats.counters[kShadowTraced], stats.traceMs.Average(), stats.temporalMs.Average(),
					stats.spatialMs.Average(), stats.totalMs.Average(), stats.historyResets);
			} else {
				const auto& c = stats.counters;
				logger::info("[SkyrimRT] Point-light shadows: {} lights ({} traced, {} shadow-mapped, {} portal-strict), {:.1f}% of {} pixels sample one, {:.1f}% of those occluded (by distance to the light <32 {} <64 {} <128 {}) | trace {:.3f} ms temporal {:.3f} ms spatial {:.3f} ms total {:.3f} ms | history resets {}",
					stats.pointLights, stats.pointLightsTraced, stats.pointLightsShadowMapped, stats.pointLightsPortalStrict, c[kPointTraced] ? 100.0f * c[kPointSampled] / c[kPointTraced] : 0.0f, c[kPointTraced],
					c[kPointSampled] ? 100.0f * c[kPointOccluded] / c[kPointSampled] : 0.0f, c[kPointOccluderNear32], c[kPointOccluderNear64], c[kPointOccluderNear128],
					stats.traceMs.Average(), stats.temporalMs.Average(), stats.spatialMs.Average(), stats.totalMs.Average(), stats.historyResets);
			}
		}
	}

	void SunShadows::ReadDumpImages(std::vector<DumpImage>& a_out)
	{
		if (!dumpCaptured || !dumpReadback || !dumpWidth || !dumpHeight)
			return;
		dumpCaptured = false;
		void* mapped = nullptr;
		if (FAILED(dumpReadback->Map(0, nullptr, &mapped)))
			return;
		constexpr const char* kSunNames[] = { "shadow_raw", "shadow_mask", "game_shadow_mask" };
		constexpr const char* kPointNames[] = { "point_shadow_raw", "point_shadow_mask", "" };
		const auto& kNames = kind == ShadowKind::kSun ? kSunNames : kPointNames;
		const uint32_t imageCount = dumpHasComparison ? 3 : 2;
		for (uint32_t i = 0; i < imageCount; i++) {
			// R8 -> grey RGBA8, cropped to the render region.
			DumpImage image{ kNames[i], dumpWidth, dumpHeight, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<uint8_t>(static_cast<size_t>(dumpWidth) * dumpHeight * 4) };
			const auto* source = static_cast<const uint8_t*>(mapped) + dumpImageBytes * i;
			for (uint32_t y = 0; y < dumpHeight; y++) {
				const uint8_t* row = source + static_cast<size_t>(y) * dumpRowPitch;
				uint8_t* out = image.pixels.data() + static_cast<size_t>(y) * dumpWidth * 4;
				for (uint32_t x = 0; x < dumpWidth; x++) {
					out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = row[x];
					out[x * 4 + 3] = 255;
				}
			}
			a_out.push_back(std::move(image));
		}
		D3D12_RANGE noWrite{ 0, 0 };
		dumpReadback->Unmap(0, &noWrite);
	}
}
