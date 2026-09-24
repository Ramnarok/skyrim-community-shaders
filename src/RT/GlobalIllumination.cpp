#include "GlobalIllumination.h"

#if defined(SKYRIMRT_NRD)

#	include "AlbedoAtlas.h"
#	include "AlphaAtlas.h"
#	include "Deferred.h"

#	include <DirectXPackedVector.h>
#	include <fstream>

namespace RT
{
	namespace
	{
		// Must match GIConstants in GICommon.hlsli (cbuffer packing).
		struct alignas(16) GIConstants
		{
			float viewProjInverse[16];
			float view[16];
			float viewInverse[16];
			float toSun[4];
			float sunColor[4];
			float ambientSH[3][4];
			float hitDistParams[4];
			uint32_t renderSize[2];
			uint32_t frameIndex;
			uint32_t casterMask;
			float normalBias;
			float distanceBias;
			float skyViewZ;
			uint32_t linearLighting;
			float intensity;
			float aoStrength;
			float colorGamma;
			float lightGamma;
			float ambientGamma;
			float ambientMult;
			uint32_t viewMode;
			uint32_t interior;
			uint32_t pointLightCount;
			uint32_t pointLightShadows;
			uint32_t inverseSquare;
			float directionalLightMult;
			uint32_t skyLight;  // M8: misses that reach the sky carry its radiance (exteriors)
			uint32_t bounces;   // M8 multi-bounce: path vertices (1-3)
			uint32_t reflections;          // M8 reflections
			float reflectionMaxRoughness;  // ... traced up to this G-buffer roughness
			uint32_t reflectionHalfResolution;  // ... one ray per 2x2 block
			uint32_t pad[3];
		};
		static_assert(sizeof(GIConstants) == 400);

		constexpr uint32_t kMaskStatic = 0x01;  // InstanceMask bits, as Raytracer::Record assigns them
		constexpr uint32_t kMaskTerrain = 0x02;
		constexpr uint32_t kMaskActor = 0x04;
		constexpr uint32_t kMaskAlphaTested = 0x08;
		constexpr float kNormalBias = 1.0f;
		constexpr float kDistanceBias = 0.002f;

		constexpr uint64_t kConstantsOffset = 0;
		constexpr uint64_t kZeroOffset = 512;
		constexpr uint64_t kPointLightsOffset = 1024;
		constexpr uint64_t kUploadBytes = kPointLightsOffset + sizeof(PointLight) * GlobalIllumination::kMaxPointLights;
		constexpr uint64_t kCounterBytes = 64;
		static_assert(kGICounterCount * sizeof(uint32_t) <= kCounterBytes);
		// Timestamps: start, GI trace, reflection trace, REBLUR_DIFFUSE, REBLUR_SPECULAR, resolves.
		constexpr uint32_t kTimestampsPerSlot = 6;

		// Descriptor heap: mesh pages, then tables of SRV t1..t4 + UAV u0..u3 (GI trace, GI resolve), the atlases, then
		// the M8 reflection tables in the same layout.
		constexpr uint32_t kTableSrvs = 4;
		constexpr uint32_t kTableUavs = 4;
		constexpr uint32_t kTableSize = kTableSrvs + kTableUavs;
		constexpr uint32_t kTraceTable = GlobalIllumination::kMeshPageSlots;
		constexpr uint32_t kResolveTable = kTraceTable + kTableSize;
		constexpr uint32_t kAlphaAtlasDescriptor = kResolveTable + kTableSize;  // M7c
		constexpr uint32_t kAlbedoAtlasDescriptor = kAlphaAtlasDescriptor + 1;  // M8, right after it (t1, space2)
		constexpr uint32_t kReflectionTraceTable = kAlbedoAtlasDescriptor + 1;  // M8 reflections
		constexpr uint32_t kReflectionResolveTable = kReflectionTraceTable + kTableSize;
		constexpr uint32_t kDescriptorCount = kReflectionResolveTable + kTableSize;

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

		D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* a_resource)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = a_resource;
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

		// CS matrices are stored row by row and used as M * v; NRD wants the same M stored column by column.
		void ToColumnMajor(const float* a_rowMajor, float* a_out)
		{
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++)
					a_out[c * 4 + r] = a_rowMajor[r * 4 + c];
		}

		// a_view * Translate(a_delta), row-major M * v.
		void ViewTimesTranslation(const float* a_view, const RE::NiPoint3& a_delta, float* a_out)
		{
			std::memcpy(a_out, a_view, sizeof(float) * 16);
			for (int r = 0; r < 4; r++)
				a_out[r * 4 + 3] = a_view[r * 4 + 0] * a_delta.x + a_view[r * 4 + 1] * a_delta.y + a_view[r * 4 + 2] * a_delta.z + a_view[r * 4 + 3];
		}

		uint8_t ToneMapByte(float a_value)
		{
			const float v = std::max(a_value, 0.0f);
			return static_cast<uint8_t>(std::clamp(v / (1.0f + v), 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		// NRD's radiance packing (_NRD_YCoCgToLinear in NRD.hlsli): REBLUR's inputs and outputs hold YCoCg.
		std::array<float, 3> YCoCgToRGB(float a_y, float a_co, float a_cg)
		{
			const float t = a_y - a_cg;
			return { t + a_co, a_y + a_cg, t - a_co };
		}
	}

	bool GlobalIllumination::Fail(std::string a_reason)
	{
		failureReason = std::move(a_reason);
		logger::error("[SkyrimRT] GI setup failed: {}", failureReason);
		return false;
	}

	bool GlobalIllumination::CreateTexture(DXGI_FORMAT a_format, const wchar_t* a_name, winrt::com_ptr<ID3D12Resource>& a_out)
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
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, kSRV, nullptr, IID_PPV_ARGS(a_out.put())); FAILED(hr))
			return Fail(std::format("creating {} failed ({})", stl::utf16_to_utf8(a_name).value_or("texture"s), FormatHResult(hr)));
		a_out->SetName(a_name);
		return true;
	}

	bool GlobalIllumination::CreatePipelines()
	{
		D3D12_DESCRIPTOR_RANGE tableRanges[2]{};
		tableRanges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTableSrvs, 1, 0, 0 };           // t1..t4
		tableRanges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kTableUavs, 0, 0, kTableSrvs };  // u0..u3
		D3D12_DESCRIPTOR_RANGE pageRanges[3]{};
		pageRanges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMeshPageSlots, 0, 1, 0 };  // t0, space1: mesh pages
		pageRanges[1] = GetAlphaAtlasRange(kAlphaAtlasDescriptor);                    // t0, space2: M7c alpha atlas
		pageRanges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 2, kAlbedoAtlasDescriptor };  // t1, space2: M8 albedo atlas

		D3D12_ROOT_PARAMETER params[7]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0
		params[0].Descriptor = { 0, 0 };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t0: TLAS
		params[1].Descriptor = { 0, 0 };
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t5: instance data
		params[2].Descriptor = { 5, 0 };
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u4: counters
		params[3].Descriptor = { 4, 0 };
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[4].DescriptorTable = { 2, tableRanges };
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[5].DescriptorTable = { 3, pageRanges };
		params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t6: point lights
		params[6].Descriptor = { 6, 0 };
		for (auto& param : params)
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		const D3D12_STATIC_SAMPLER_DESC sampler = GetAlphaAtlasSampler();
		D3D12_ROOT_SIGNATURE_DESC rootDesc{ .NumParameters = 7, .pParameters = params, .NumStaticSamplers = 1, .pStaticSamplers = &sampler };
		winrt::com_ptr<ID3DBlob> blob, errors;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put());
		if (FAILED(hr))
			return Fail(std::format("serialize GI root signature failed ({}): {}", FormatHResult(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
		if (FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
			return Fail(std::format("CreateRootSignature(GI) failed ({})", FormatHResult(hr)));

		std::string error;
		if (!CreatePipeline("GITraceCS.cso", L"SkyrimRT::GITracePSO", tracePipeline, error) ||
			!CreatePipeline("GIResolveCS.cso", L"SkyrimRT::GIResolvePSO", resolvePipeline, error))
			return Fail(std::move(error));
		return true;
	}

	bool GlobalIllumination::CreatePipeline(const char* a_file, const wchar_t* a_name, winrt::com_ptr<ID3D12PipelineState>& a_out, std::string& a_error)
	{
		const auto path = std::filesystem::path("Data\\Shaders\\SkyrimRT") / a_file;
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			a_error = std::format("cannot open {}", path.string());
			return false;
		}
		const std::vector<char> bytecode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.CS = { bytecode.data(), bytecode.size() };
		if (HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(a_out.put())); FAILED(hr)) {
			a_error = std::format("CreateComputePipelineState({}) failed ({})", a_file, FormatHResult(hr));
			return false;
		}
		a_out->SetName(a_name);
		return true;
	}

	void GlobalIllumination::WriteDescriptors()
	{
		auto handle = [&](uint32_t a_index) {
			auto h = heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<SIZE_T>(a_index) * descriptorSize;
			return h;
		};
		auto srv = [&](uint32_t a_table, uint32_t a_register, ID3D12Resource* a_resource, DXGI_FORMAT a_format) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc{ .Format = a_format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
			desc.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(a_resource, &desc, handle(a_table + a_register - 1));
		};
		auto uav = [&](uint32_t a_table, uint32_t a_register, ID3D12Resource* a_resource, DXGI_FORMAT a_format) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{ .Format = a_format, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };
			device->CreateUnorderedAccessView(a_resource, nullptr, &desc, handle(a_table + kTableSrvs + a_register));
		};
		auto formatOf = [](ID3D12Resource* a_resource, DXGI_FORMAT a_fallback) { return a_resource ? a_resource->GetDesc().Format : a_fallback; };

		// Trace: t1 depth, t2 G-buffer normal, t3 game motion vectors, t4 unused; u0 viewZ, u1 normal, u2 MV, u3 noisy.
		srv(kTraceTable, 1, rasterDepth, DXGI_FORMAT_R32_FLOAT);
		srv(kTraceTable, 2, gbufferNormal.resource12.get(), formatOf(gbufferNormal.resource12.get(), DXGI_FORMAT_R10G10B10A2_UNORM));
		srv(kTraceTable, 3, motionVectors.resource12.get(), formatOf(motionVectors.resource12.get(), DXGI_FORMAT_R16G16_FLOAT));
		srv(kTraceTable, 4, nullptr, DXGI_FORMAT_R8_UNORM);
		uav(kTraceTable, 0, viewZ.get(), DXGI_FORMAT_R32_FLOAT);
		uav(kTraceTable, 1, normalRoughness.get(), DXGI_FORMAT_R10G10B10A2_UNORM);
		uav(kTraceTable, 2, nrdMotionVectors.get(), DXGI_FORMAT_R16G16_FLOAT);
		uav(kTraceTable, 3, noisy.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);

		// Resolve: t1 depth, t2 denoised, t3 normal, t4 noisy; u0 AO, u1 Y, u2 CoCg, u3 view.
		srv(kResolveTable, 1, rasterDepth, DXGI_FORMAT_R32_FLOAT);
		srv(kResolveTable, 2, denoised.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
		srv(kResolveTable, 3, normalRoughness.get(), DXGI_FORMAT_R10G10B10A2_UNORM);
		srv(kResolveTable, 4, noisy.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
		uav(kResolveTable, 0, ao.resource12.get(), DXGI_FORMAT_R8_UNORM);
		uav(kResolveTable, 1, y.resource12.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
		uav(kResolveTable, 2, coCg.resource12.get(), DXGI_FORMAT_R16G16_FLOAT);
		uav(kResolveTable, 3, view.resource12.get(), DXGI_FORMAT_R8G8B8A8_UNORM);

		WriteAlphaAtlasDescriptor(device, alphaAtlas, handle(kAlphaAtlasDescriptor));
		WriteAlbedoAtlasDescriptor(device, albedoAtlas, handle(kAlbedoAtlasDescriptor));

		// M8 reflection trace: t1 depth, t2 G-buffer normal, t3 unused, t4 reflectance; u0 viewZ, u1 normal, u2 unused,
		// u3 noisy (REBLUR_SPECULAR's inputs). Null until InitReflections creates them.
		srv(kReflectionTraceTable, 1, rasterDepth, DXGI_FORMAT_R32_FLOAT);
		srv(kReflectionTraceTable, 2, gbufferNormal.resource12.get(), formatOf(gbufferNormal.resource12.get(), DXGI_FORMAT_R10G10B10A2_UNORM));
		srv(kReflectionTraceTable, 3, nullptr, DXGI_FORMAT_R16G16_FLOAT);
		srv(kReflectionTraceTable, 4, reflectance.resource12.get(), formatOf(reflectance.resource12.get(), DXGI_FORMAT_R11G11B10_FLOAT));
		uav(kReflectionTraceTable, 0, specularViewZ.get(), DXGI_FORMAT_R32_FLOAT);
		uav(kReflectionTraceTable, 1, specularNormalRoughness.get(), DXGI_FORMAT_R10G10B10A2_UNORM);
		uav(kReflectionTraceTable, 2, nullptr, DXGI_FORMAT_R16G16_FLOAT);
		uav(kReflectionTraceTable, 3, specularNoisy.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);

		// M8 reflection resolve: t1 depth, t2 denoised, t3 viewZ, t4 noisy; u0 reflections, u1-u2 unused, u3 view.
		srv(kReflectionResolveTable, 1, rasterDepth, DXGI_FORMAT_R32_FLOAT);
		srv(kReflectionResolveTable, 2, specularDenoised.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
		srv(kReflectionResolveTable, 3, specularViewZ.get(), DXGI_FORMAT_R32_FLOAT);
		srv(kReflectionResolveTable, 4, specularNoisy.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
		uav(kReflectionResolveTable, 0, reflections.resource12.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
		uav(kReflectionResolveTable, 1, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
		uav(kReflectionResolveTable, 2, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
		uav(kReflectionResolveTable, 3, view.resource12.get(), DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	bool GlobalIllumination::Init(ID3D12Device5* a_device, ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context,
		uint32_t a_width, uint32_t a_height, ID3D12Resource* a_rasterDepth, ID3D12Resource* a_alphaAtlas, ID3D12Resource* a_albedoAtlas)
	{
		alphaAtlas = a_alphaAtlas;
		albedoAtlas = a_albedoAtlas;
		device = a_device;
		d3d11Device = a_d3d11Device;
		d3d11Context = a_d3d11Context;
		width = a_width;
		height = a_height;
		rasterDepth = a_rasterDepth;

		std::string error;
		if (!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R8_UNORM, "GIAmbientOcclusion", ao, error) ||
			!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "GIIrradianceY", y, error) ||
			!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R16G16_FLOAT, "GIIrradianceCoCg", coCg, error) ||
			!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "GIView", view, error))
			return Fail(std::move(error));

		if (!CreateTexture(DXGI_FORMAT_R32_FLOAT, L"SkyrimRT::GIViewZ", viewZ) ||
			!CreateTexture(DXGI_FORMAT_R10G10B10A2_UNORM, L"SkyrimRT::GINormalRoughness", normalRoughness) ||
			!CreateTexture(DXGI_FORMAT_R16G16_FLOAT, L"SkyrimRT::GIMotionVectors", nrdMotionVectors) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"SkyrimRT::GINoisy", noisy) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"SkyrimRT::GIDenoised", denoised))
			return false;

		if (!denoiser.Init(device, width, height, nrd::Denoiser::REBLUR_DIFFUSE))
			return Fail("NRD: " + denoiser.GetFailureReason());
		if (!CreatePipelines())
			return false;

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = kDescriptorCount, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.put()));
		if (FAILED(hr))
			return Fail(std::format("CreateDescriptorHeap(GI) failed ({})", FormatHResult(hr)));
		descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_SHADER_RESOURCE_VIEW_DESC nullPage{ .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
		nullPage.Buffer.NumElements = 1;
		nullPage.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		for (uint32_t i = 0; i < kMeshPageSlots; i++) {
			auto h = heap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<SIZE_T>(i) * descriptorSize;
			device->CreateShaderResourceView(nullptr, &nullPage, h);
		}
		WriteDescriptors();

		constexpr auto kUavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, kCounterBytes, kCommon, kUavFlag, counters.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, kCounterBytes * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, countersReadback.put())) ||
			FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t) * kTimestampsPerSlot * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, timestampReadback.put())))
			return Fail(std::format("creating GI counter buffers failed ({})", FormatHResult(hr)));
		counters->SetName(L"SkyrimRT::GICounters");
		for (uint32_t i = 0; i < kFramesInFlight; i++) {
			if (FAILED(hr = CreateBufferResource(device, D3D12_HEAP_TYPE_UPLOAD, kUploadBytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, uploads[i].put())))
				return Fail(std::format("creating GI upload buffer failed ({})", FormatHResult(hr)));
			uploads[i]->SetName(L"SkyrimRT::GIUpload");
			D3D12_RANGE noRead{ 0, 0 };
			void* mapped = nullptr;
			if (FAILED(uploads[i]->Map(0, &noRead, &mapped)))
				return Fail("mapping GI upload buffer failed");
			uploadCpu[i] = static_cast<uint8_t*>(mapped);
			std::memset(uploadCpu[i] + kZeroOffset, 0, kCounterBytes);
		}
		void* mapped = nullptr;
		if (FAILED(countersReadback->Map(0, nullptr, &mapped)))
			return Fail("mapping GI counter readback failed");
		countersCpu = static_cast<const uint32_t*>(mapped);
		if (FAILED(timestampReadback->Map(0, nullptr, &mapped)))
			return Fail("mapping GI timestamp readback failed");
		timestampCpu = static_cast<const uint64_t*>(mapped);
		D3D12_QUERY_HEAP_DESC queryDesc{ .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = kTimestampsPerSlot * kFramesInFlight };
		if (FAILED(hr = device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(timestamps.put()))))
			return Fail(std::format("CreateQueryHeap(GI) failed ({})", FormatHResult(hr)));

		stats.textureWidth = width;
		stats.textureHeight = height;
		logger::info("[SkyrimRT] GI ready: {}x{}, REBLUR_DIFFUSE", width, height);
		return true;
	}

	bool GlobalIllumination::InitReflections()
	{
		reflectionsInitTried = true;
		auto fail = [&](std::string a_reason) {
			stats.reflectionsFailure = std::move(a_reason);
			logger::error("[SkyrimRT] Reflections unavailable: {}", stats.reflectionsFailure);
			failureReason.clear();  // GI itself is fine
			return false;
		};

		std::string error;
		if (!CreateSharedTexture(d3d11Device, device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "GIReflections", reflections, error))
			return fail(std::move(error));
		if (!CreateTexture(DXGI_FORMAT_R32_FLOAT, L"SkyrimRT::SpecularViewZ", specularViewZ) ||
			!CreateTexture(DXGI_FORMAT_R10G10B10A2_UNORM, L"SkyrimRT::SpecularNormalRoughness", specularNormalRoughness) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"SkyrimRT::SpecularNoisy", specularNoisy) ||
			!CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"SkyrimRT::SpecularDenoised", specularDenoised))
			return fail(failureReason);
		if (!specularDenoiser.Init(device, width, height, nrd::Denoiser::REBLUR_SPECULAR))
			return fail("NRD: " + specularDenoiser.GetFailureReason());
		if (!CreatePipeline("ReflectionTraceCS.cso", L"SkyrimRT::ReflectionTracePSO", reflectionTracePipeline, error) ||
			!CreatePipeline("ReflectionResolveCS.cso", L"SkyrimRT::ReflectionResolvePSO", reflectionResolvePipeline, error))
			return fail(std::move(error));

		reflectionsReady = true;
		stats.reflectionsAvailable = true;
		WriteDescriptors();
		logger::info("[SkyrimRT] Reflections ready: {}x{}, REBLUR_SPECULAR", width, height);
		return true;
	}

	bool GlobalIllumination::CopyInputs(bool a_reflections)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		const auto& targets = renderer->GetRuntimeData().renderTargets;
		ID3D11Texture2D* sources[3] = { targets[NORMALROUGHNESS].texture, targets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture, targets[REFLECTANCE].texture };
		SharedTexture* destinations[3] = { &gbufferNormal, &motionVectors, &reflectance };
		constexpr const char* kNames[3] = { "GIGBufferNormal", "GIMotionVectors", "GIReflectance" };

		// M8 reflections are set up the first time they're wanted; a failure is logged once and they stay off.
		if (a_reflections && !reflectionsInitTried)
			InitReflections();
		const uint32_t count = a_reflections && reflectionsReady && sources[2] ? 3 : 2;
		reflectionsRecorded = false;

		bool created = false;
		for (uint32_t i = 0; i < count; i++) {
			if (!sources[i])
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			sources[i]->GetDesc(&desc);
			if (!destinations[i]->texture11) {
				std::string error;
				if (!CreateSharedTexture(d3d11Device, device, desc.Width, desc.Height, desc.Format, kNames[i], *destinations[i], error)) {
					logger::error("[SkyrimRT] GI: {}", error);
					if (i == 2) {  // reflections only
						stats.reflectionsFailure = std::move(error);
						stats.reflectionsAvailable = reflectionsReady = false;
						break;
					}
					return false;
				}
				created = true;
			}
			d3d11Context->CopyResource(destinations[i]->texture11.get(), sources[i]);
		}
		if (created)
			WriteDescriptors();
		return true;
	}

	void GlobalIllumination::FillNrdSettings(const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const GIParams& a_params,
		bool a_historyValid, bool a_everCleared, nrd::CommonSettings& a_common, nrd::ReblurSettings& a_reblur) const
	{
		// Camera-relative world space moves with the camera: the previous view matrix is expressed in this frame's
		// origin by folding in the CameraPosAdjust delta.
		const RE::NiPoint3 delta{ a_camera.posAdjust.x - prevPosAdjust.x, a_camera.posAdjust.y - prevPosAdjust.y, a_camera.posAdjust.z - prevPosAdjust.z };
		float prevViewHere[16];
		ViewTimesTranslation(a_historyValid ? prevView : a_camera.view, a_historyValid ? delta : RE::NiPoint3{}, prevViewHere);

		ToColumnMajor(a_camera.projUnjittered, a_common.viewToClipMatrix);
		ToColumnMajor(a_historyValid ? prevProj : a_camera.projUnjittered, a_common.viewToClipMatrixPrev);
		ToColumnMajor(a_camera.view, a_common.worldToViewMatrix);
		ToColumnMajor(prevViewHere, a_common.worldToViewMatrixPrev);
		a_common.motionVectorScale[0] = 1.0f;  // kMOTION_VECTOR is already prevUV - currUV
		a_common.motionVectorScale[1] = 1.0f;
		a_common.motionVectorScale[2] = 0.0f;
		a_common.resourceSize[0] = a_common.resourceSizePrev[0] = static_cast<uint16_t>(width);
		a_common.resourceSize[1] = a_common.resourceSizePrev[1] = static_cast<uint16_t>(height);
		a_common.rectSize[0] = static_cast<uint16_t>(a_renderWidth);
		a_common.rectSize[1] = static_cast<uint16_t>(a_renderHeight);
		a_common.rectSizePrev[0] = static_cast<uint16_t>(a_historyValid ? prevRenderWidth : a_renderWidth);
		a_common.rectSizePrev[1] = static_cast<uint16_t>(a_historyValid ? prevRenderHeight : a_renderHeight);
		a_common.denoisingRange = kDenoisingRange;
		a_common.frameIndex = frameIndex;
		a_common.accumulationMode = !a_everCleared ? nrd::AccumulationMode::CLEAR_AND_RESTART :
		                            a_historyValid ? nrd::AccumulationMode::CONTINUE :
		                                             nrd::AccumulationMode::RESTART;

		// NRD's defaults are in meters; only the hit-distance constant is a length.
		a_reblur.hitDistanceParameters.A = 3.0f * kUnitsPerMeter;
		a_reblur.maxAccumulatedFrameNum = std::clamp<uint32_t>(a_params.maxAccumulatedFrames, 1, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
		a_reblur.maxFastAccumulatedFrameNum = std::min<uint32_t>(a_reblur.maxFastAccumulatedFrameNum, a_reblur.maxAccumulatedFrameNum);
		a_reblur.historyFixFrameNum = std::min<uint32_t>(a_reblur.historyFixFrameNum, a_reblur.maxFastAccumulatedFrameNum > 0 ? a_reblur.maxFastAccumulatedFrameNum - 1 : 0);
	}

	void GlobalIllumination::Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
		const BufferPool& a_meshPool, const SkinnedMeshes* a_skinned, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight,
		const GIParams& a_params, bool a_captureDump)
	{
		const bool historyValid = haveHistory && a_camera.gameFrame == historyGameFrame + 1;
		if (!historyValid)
			stats.historyResets++;
		frameIndex++;
		// M8 reflections: CopyInputs copied REFLECTANCE this frame when they're wanted and set up.
		const bool traceReflections = a_params.reflections && reflectionsReady && reflectance.resource12;
		const bool specularHistoryValid = specularHaveHistory && a_camera.gameFrame == specularHistoryGameFrame + 1;
		if (traceReflections && !specularHistoryValid)
			stats.reflectionHistoryResets++;

		auto* c = reinterpret_cast<GIConstants*>(uploadCpu[a_slot] + kConstantsOffset);
		std::memcpy(c->viewProjInverse, a_camera.viewProjInverse, sizeof(c->viewProjInverse));
		std::memcpy(c->view, a_camera.view, sizeof(c->view));
		std::memcpy(c->viewInverse, a_camera.viewInverse, sizeof(c->viewInverse));
		std::memcpy(c->toSun, a_params.toSun, sizeof(a_params.toSun));
		c->toSun[3] = 0.0f;
		std::memcpy(c->sunColor, a_params.sunColor, sizeof(a_params.sunColor));
		c->sunColor[3] = 0.0f;
		std::memcpy(c->ambientSH, a_params.ambientSH, sizeof(c->ambientSH));
		c->hitDistParams[0] = 3.0f * kUnitsPerMeter;  // must match ReblurSettings::hitDistanceParameters
		c->hitDistParams[1] = 0.1f;
		c->hitDistParams[2] = 20.0f;
		c->hitDistParams[3] = std::max(a_params.rayLength, 1.0f);
		c->renderSize[0] = a_renderWidth;
		c->renderSize[1] = a_renderHeight;
		c->frameIndex = frameIndex;
		c->casterMask = kMaskStatic | kMaskTerrain | kMaskActor | (a_params.alphaTestedCasters ? kMaskAlphaTested : 0u);
		c->normalBias = kNormalBias;
		c->distanceBias = kDistanceBias;
		c->skyViewZ = kSkyViewZ;
		c->linearLighting = a_params.linearLighting ? 1u : 0u;
		c->intensity = a_params.intensity;
		c->aoStrength = std::clamp(a_params.aoStrength, 0.0f, 1.0f);
		c->colorGamma = a_params.colorGamma;
		c->lightGamma = a_params.lightGamma;
		c->ambientGamma = a_params.ambientGamma;
		c->ambientMult = a_params.ambientMult;
		c->viewMode = a_params.viewMode;
		c->interior = a_params.interior ? 1u : 0u;
		const uint32_t pointLightCount = static_cast<uint32_t>(std::min<size_t>(a_params.pointLights.size(), kMaxPointLights));
		if (pointLightCount)
			std::memcpy(uploadCpu[a_slot] + kPointLightsOffset, a_params.pointLights.data(), sizeof(PointLight) * pointLightCount);
		c->pointLightCount = pointLightCount;
		c->pointLightShadows = a_params.pointLightShadows ? 1u : 0u;
		c->inverseSquare = a_params.inverseSquare ? 1u : 0u;
		c->directionalLightMult = a_params.directionalLightMult;
		c->skyLight = a_params.skyLight && !a_params.interior ? 1u : 0u;
		c->bounces = std::clamp(a_params.bounces, 1u, 3u);
		c->reflections = traceReflections ? 1u : 0u;
		c->reflectionMaxRoughness = std::clamp(a_params.reflectionMaxRoughness, 0.0f, 1.0f);
		c->reflectionHalfResolution = a_params.reflectionHalfResolution ? 1u : 0u;

		auto first = heap->GetCPUDescriptorHandleForHeapStart();
		UpdatePageDescriptors(device, a_meshPool, first, descriptorSize, describedPageSerials.data(), SkinnedMeshes::kFirstPageSlot);
		if (a_skinned) {
			first.ptr += static_cast<SIZE_T>(SkinnedMeshes::kFirstPageSlot) * descriptorSize;
			UpdatePageDescriptors(device, a_skinned->GetOutputPool(), first, descriptorSize, describedPageSerials.data() + SkinnedMeshes::kFirstPageSlot, SkinnedMeshes::kPageSlots);
		}

		const D3D12_GPU_VIRTUAL_ADDRESS uploadVA = uploads[a_slot]->GetGPUVirtualAddress();
		auto table = [&](uint32_t a_index) {
			auto h = heap->GetGPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<UINT64>(a_index) * descriptorSize;
			return h;
		};
		auto bind = [&] {
			ID3D12DescriptorHeap* heaps[] = { heap.get() };
			a_list->SetDescriptorHeaps(1, heaps);
			a_list->SetComputeRootSignature(rootSignature.get());
			a_list->SetComputeRootConstantBufferView(0, uploadVA + kConstantsOffset);
			a_list->SetComputeRootShaderResourceView(1, a_tlas);
			a_list->SetComputeRootShaderResourceView(2, a_instances);
			a_list->SetComputeRootUnorderedAccessView(3, counters->GetGPUVirtualAddress());
			a_list->SetComputeRootDescriptorTable(5, table(0));
			a_list->SetComputeRootShaderResourceView(6, uploadVA + kPointLightsOffset);
		};
		const uint32_t groupsX = (a_renderWidth + 7) / 8;
		const uint32_t groupsY = (a_renderHeight + 7) / 8;
		const uint32_t query = a_slot * kTimestampsPerSlot;

		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 0);

		// 1. Trace: NRD's noisy input and guides.
		SetPassMarker(a_list, L"SkyrimRT: GI trace");
		Barriers(a_list, { TransitionBarrier(counters.get(), kCommon, D3D12_RESOURCE_STATE_COPY_DEST) });
		a_list->CopyBufferRegion(counters.get(), 0, uploads[a_slot].get(), kZeroOffset, kCounterBytes);
		Barriers(a_list, { TransitionBarrier(counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, kUAV),
							 TransitionBarrier(rasterDepth, kCommon, kSRV),
							 TransitionBarrier(gbufferNormal.resource12.get(), kCommon, kSRV),
							 TransitionBarrier(motionVectors.resource12.get(), kCommon, kSRV),
							 TransitionBarrier(viewZ.get(), kSRV, kUAV),
							 TransitionBarrier(normalRoughness.get(), kSRV, kUAV),
							 TransitionBarrier(nrdMotionVectors.get(), kSRV, kUAV),
							 TransitionBarrier(noisy.get(), kSRV, kUAV) });
		if (alphaAtlas)
			Barriers(a_list, { TransitionBarrier(alphaAtlas, kCommon, kSRV) });  // M7c, filled on D3D11 at Prepass
		if (albedoAtlas)
			Barriers(a_list, { TransitionBarrier(albedoAtlas, kCommon, kSRV) });  // M8, likewise
		bind();
		a_list->SetPipelineState(tracePipeline.get());
		a_list->SetComputeRootDescriptorTable(4, table(kTraceTable));
		a_list->Dispatch(groupsX, groupsY, 1);
		Barriers(a_list, { TransitionBarrier(viewZ.get(), kUAV, kSRV),
							 TransitionBarrier(normalRoughness.get(), kUAV, kSRV),
							 TransitionBarrier(nrdMotionVectors.get(), kUAV, kSRV),
							 TransitionBarrier(noisy.get(), kUAV, kSRV) });
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);

		// 2. M8 reflections: REBLUR_SPECULAR's noisy input and guides, from the pixels with a reflection term.
		if (traceReflections) {
			SetPassMarker(a_list, L"SkyrimRT: reflection trace");
			Barriers(a_list, { TransitionBarrier(reflectance.resource12.get(), kCommon, kSRV),
								 TransitionBarrier(specularViewZ.get(), kSRV, kUAV),
								 TransitionBarrier(specularNormalRoughness.get(), kSRV, kUAV),
								 TransitionBarrier(specularNoisy.get(), kSRV, kUAV) });
			a_list->SetPipelineState(reflectionTracePipeline.get());
			a_list->SetComputeRootDescriptorTable(4, table(kReflectionTraceTable));
			if (a_params.reflectionHalfResolution)  // one thread per 2x2 block
				a_list->Dispatch((a_renderWidth + 15) / 16, (a_renderHeight + 15) / 16, 1);
			else
				a_list->Dispatch(groupsX, groupsY, 1);
			Barriers(a_list, { TransitionBarrier(reflectance.resource12.get(), kSRV, kCommon),
								 TransitionBarrier(specularViewZ.get(), kUAV, kSRV),
								 TransitionBarrier(specularNormalRoughness.get(), kUAV, kSRV),
								 TransitionBarrier(specularNoisy.get(), kUAV, kSRV) });
		}
		if (alphaAtlas)
			Barriers(a_list, { TransitionBarrier(alphaAtlas, kSRV, kCommon) });
		if (albedoAtlas)
			Barriers(a_list, { TransitionBarrier(albedoAtlas, kSRV, kCommon) });
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2);

		// 3. REBLUR, then (M8) REBLUR_SPECULAR, each its own NRD instance.
		SetPassMarker(a_list, L"SkyrimRT: GI REBLUR");
		nrd::CommonSettings common{};
		nrd::ReblurSettings reblur{};
		FillNrdSettings(a_camera, a_renderWidth, a_renderHeight, a_params, historyValid, everCleared, common, reblur);
		denoiser.Record(a_list, a_slot, common, reblur, { nrdMotionVectors.get(), normalRoughness.get(), viewZ.get(), noisy.get(), denoised.get() });
		everCleared = true;
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 3);
		if (traceReflections) {
			SetPassMarker(a_list, L"SkyrimRT: reflection REBLUR");
			nrd::CommonSettings specularCommon{};
			nrd::ReblurSettings specularReblur{};
			FillNrdSettings(a_camera, a_renderWidth, a_renderHeight, a_params, specularHistoryValid, specularEverCleared, specularCommon, specularReblur);
			// Half resolution leaves three of four pixels without a sample (hit distance 0): NRD's probabilistic-sampling
			// path, which needs hit-distance reconstruction and the pre-pass (on by default, specularPrepassBlurRadius).
			if (a_params.reflectionHalfResolution)
				specularReblur.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
			specularDenoiser.Record(a_list, a_slot, specularCommon, specularReblur,
				{ nrdMotionVectors.get(), specularNormalRoughness.get(), specularViewZ.get(), specularNoisy.get(), specularDenoised.get() });
			specularEverCleared = true;
		}
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 4);

		// 4. Resolve into the composite's Screen-Space GI inputs, and (M8) its reflection input.
		SetPassMarker(a_list, L"SkyrimRT: GI resolve");
		Barriers(a_list, { TransitionBarrier(ao.resource12.get(), kCommon, kUAV),
							 TransitionBarrier(y.resource12.get(), kCommon, kUAV),
							 TransitionBarrier(coCg.resource12.get(), kCommon, kUAV),
							 TransitionBarrier(view.resource12.get(), kCommon, kUAV) });
		bind();
		a_list->SetPipelineState(resolvePipeline.get());
		a_list->SetComputeRootDescriptorTable(4, table(kResolveTable));
		a_list->Dispatch(groupsX, groupsY, 1);
		if (traceReflections) {
			SetPassMarker(a_list, L"SkyrimRT: reflection resolve");
			Barriers(a_list, { TransitionBarrier(reflections.resource12.get(), kCommon, kUAV),
								 UavBarrier(view.resource12.get()) });  // both resolves write the debug view
			a_list->SetPipelineState(reflectionResolvePipeline.get());
			a_list->SetComputeRootDescriptorTable(4, table(kReflectionResolveTable));
			a_list->Dispatch(groupsX, groupsY, 1);
			Barriers(a_list, { TransitionBarrier(reflections.resource12.get(), kUAV, kCommon) });
		}
		a_list->EndQuery(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 5);

		// Shared textures back to COMMON before the fence signal, so D3D11 sees the writes.
		Barriers(a_list, { TransitionBarrier(ao.resource12.get(), kUAV, kCommon),
							 TransitionBarrier(y.resource12.get(), kUAV, kCommon),
							 TransitionBarrier(coCg.resource12.get(), kUAV, kCommon),
							 TransitionBarrier(view.resource12.get(), kUAV, kCommon),
							 TransitionBarrier(rasterDepth, kSRV, kCommon),
							 TransitionBarrier(gbufferNormal.resource12.get(), kSRV, kCommon),
							 TransitionBarrier(motionVectors.resource12.get(), kSRV, kCommon),
							 TransitionBarrier(counters.get(), kUAV, kCopySource) });
		a_list->CopyBufferRegion(countersReadback.get(), kCounterBytes * a_slot, counters.get(), 0, kCounterBytes);
		Barriers(a_list, { TransitionBarrier(counters.get(), kCopySource, kCommon) });
		a_list->ResolveQueryData(timestamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, query, kTimestampsPerSlot, timestampReadback.get(), sizeof(uint64_t) * query);

		if (a_captureDump) {
			ID3D12Resource* sources[kDumpImages] = { noisy.get(), denoised.get(), ao.resource12.get(), specularNoisy.get(), reflections.resource12.get() };
			const D3D12_RESOURCE_STATES rest[kDumpImages] = { kSRV, kSRV, kCommon, kSRV, kCommon };
			const uint32_t count = traceReflections ? kDumpImages : 3;
			uint64_t total = 0;
			for (uint32_t i = 0; i < count; i++) {
				const auto desc = sources[i]->GetDesc();
				UINT64 bytes = 0;
				device->GetCopyableFootprints(&desc, 0, 1, total, &dumpFootprints[i], nullptr, nullptr, &bytes);
				total = (dumpFootprints[i].Offset + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
			}
			if (dumpReadbackBytes < total) {
				dumpReadback = nullptr;
				dumpReadbackBytes = 0;
				if (SUCCEEDED(CreateBufferResource(device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, dumpReadback.put())))
					dumpReadbackBytes = total;
			}
			if (!dumpReadback) {
				logger::error("[SkyrimRT] Debug dump: cannot create GI readback buffer");
			} else {
				for (uint32_t i = 0; i < count; i++) {
					Barriers(a_list, { TransitionBarrier(sources[i], rest[i], kCopySource) });
					D3D12_TEXTURE_COPY_LOCATION dst{ .pResource = dumpReadback.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
					dst.PlacedFootprint = dumpFootprints[i];
					D3D12_TEXTURE_COPY_LOCATION src{ .pResource = sources[i], .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
					src.SubresourceIndex = 0;
					a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					Barriers(a_list, { TransitionBarrier(sources[i], kCopySource, rest[i]) });
				}
				dumpImageCount = count;
				dumpWidth = a_renderWidth;
				dumpHeight = a_renderHeight;
				dumpCaptured = true;
			}
		}

		slotPending[a_slot] = true;
		slotDispatches[a_slot] = denoiser.GetLastDispatchCount();
		slotReflections[a_slot] = traceReflections;
		slotReflectionDispatches[a_slot] = traceReflections ? specularDenoiser.GetLastDispatchCount() : 0;
		stats.params = a_params;
		stats.params.pointLights = {};
		stats.pointLights = pointLightCount;
		stats.pointLightsDropped = static_cast<uint32_t>(a_params.pointLights.size() - pointLightCount);
		if (a_captureDump)
			stats.lastPointLights.assign(a_params.pointLights.begin(), a_params.pointLights.begin() + pointLightCount);
		stats.renderWidth = a_renderWidth;
		stats.renderHeight = a_renderHeight;
		stats.framesTraced++;

		haveHistory = true;
		historyGameFrame = a_camera.gameFrame;
		std::memcpy(prevView, a_camera.view, sizeof(prevView));
		std::memcpy(prevProj, a_camera.projUnjittered, sizeof(prevProj));
		prevPosAdjust = a_camera.posAdjust;
		prevRenderWidth = a_renderWidth;
		prevRenderHeight = a_renderHeight;

		reflectionsRecorded = traceReflections;
		if (traceReflections) {
			stats.reflectionFramesTraced++;
			specularHaveHistory = true;
			specularHistoryGameFrame = a_camera.gameFrame;
		}
	}

	void GlobalIllumination::CollectResults(uint32_t a_slot)
	{
		if (!slotPending[a_slot])
			return;
		slotPending[a_slot] = false;

		const uint32_t* slotCounters = countersCpu + (kCounterBytes / sizeof(uint32_t)) * a_slot;
		for (uint32_t i = 0; i < kGICounterCount; i++)
			stats.counters[i] = slotCounters[i];
		// Timestamps: start, GI trace, reflection trace, REBLUR_DIFFUSE, REBLUR_SPECULAR, resolves (both).
		const uint64_t* t = timestampCpu + kTimestampsPerSlot * a_slot;
		stats.traceMs.Add(Ms(t[0], t[1], timestampFrequency));
		stats.denoiseMs.Add(Ms(t[2], t[3], timestampFrequency));
		stats.resolveMs.Add(Ms(t[4], t[5], timestampFrequency));
		stats.totalMs.Add(Ms(t[0], t[5], timestampFrequency));
		stats.nrdDispatches = slotDispatches[a_slot];
		stats.reflectionsLastSlot = slotReflections[a_slot];
		if (slotReflections[a_slot]) {
			stats.reflectionTraceMs.Add(Ms(t[1], t[2], timestampFrequency));
			stats.reflectionDenoiseMs.Add(Ms(t[3], t[4], timestampFrequency));
			stats.reflectionDispatches = slotReflectionDispatches[a_slot];
		}
		stats.haveResult = true;

		if (stats.framesTraced % 600 == 0) {
			logger::info("[SkyrimRT] GI: {:.1f}% of {} rays hit, {:.1f}% of hits sunlit, {:.1f}% sampled one of {} point lights ({:.1f}% occluded) | trace {:.3f} ms NRD {:.3f} ms ({} dispatches) resolve {:.3f} ms | hand-off {:.3f} ms | history resets {}",
				stats.HitPercent(), stats.counters[kGITraced], stats.SunLitHitPercent(), stats.LightSampledHitPercent(), stats.pointLights, stats.LightOccludedPercent(), stats.traceMs.Average(), stats.denoiseMs.Average(),
				stats.nrdDispatches, stats.resolveMs.Average(), stats.roundTripMs.Average(), stats.historyResets);
			if (slotReflections[a_slot]) {
				logger::info("[SkyrimRT] Reflections: {} reflective pixels, {} rays, {:.1f}% hit geometry | trace {:.3f} ms NRD {:.3f} ms ({} dispatches) | history resets {}",
					stats.counters[kGIReflectionTraced], stats.counters[kGIReflectionRays], stats.ReflectionHitPercent(), stats.reflectionTraceMs.Average(), stats.reflectionDenoiseMs.Average(),
					stats.reflectionDispatches, stats.reflectionHistoryResets);
			}
		}
	}

	void GlobalIllumination::ReadDumpImages(std::vector<DumpImage>& a_out)
	{
		if (!dumpCaptured || !dumpReadback || !dumpWidth || !dumpHeight)
			return;
		dumpCaptured = false;
		void* mapped = nullptr;
		if (FAILED(dumpReadback->Map(0, nullptr, &mapped)))
			return;
		const auto* base = static_cast<const uint8_t*>(mapped);
		const float intensity = stats.params.intensity;

		// RGBA16F images, tone-mapped per channel. REBLUR's inputs and outputs hold NRD's YCoCg; the reflection
		// composite input holds RGB.
		auto radianceImage = [&](const char* a_name, uint32_t a_index, bool a_ycocg, float a_scale) {
			DumpImage image{ a_name, dumpWidth, dumpHeight, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<uint8_t>(static_cast<size_t>(dumpWidth) * dumpHeight * 4) };
			const auto& fp = dumpFootprints[a_index];
			for (uint32_t row = 0; row < dumpHeight; row++) {
				const auto* halves = reinterpret_cast<const uint16_t*>(base + fp.Offset + static_cast<size_t>(row) * fp.Footprint.RowPitch);
				uint8_t* out = image.pixels.data() + static_cast<size_t>(row) * dumpWidth * 4;
				for (uint32_t x = 0; x < dumpWidth; x++) {
					std::array<float, 3> c{ DirectX::PackedVector::XMConvertHalfToFloat(halves[x * 4 + 0]), DirectX::PackedVector::XMConvertHalfToFloat(halves[x * 4 + 1]),
						DirectX::PackedVector::XMConvertHalfToFloat(halves[x * 4 + 2]) };
					if (a_ycocg)
						c = YCoCgToRGB(c[0], c[1], c[2]);
					for (uint32_t k = 0; k < 3; k++)
						out[x * 4 + k] = ToneMapByte(c[k] * a_scale);
					out[x * 4 + 3] = 255;
				}
			}
			a_out.push_back(std::move(image));
		};
		radianceImage("gi_noisy", 0, true, intensity);
		radianceImage("gi_denoised", 1, true, intensity);

		// AO texture holds occlusion; dumped as visibility (white = open).
		{
			DumpImage image{ "gi_ao", dumpWidth, dumpHeight, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<uint8_t>(static_cast<size_t>(dumpWidth) * dumpHeight * 4) };
			const auto& fp = dumpFootprints[2];
			for (uint32_t row = 0; row < dumpHeight; row++) {
				const uint8_t* source = base + fp.Offset + static_cast<size_t>(row) * fp.Footprint.RowPitch;
				uint8_t* out = image.pixels.data() + static_cast<size_t>(row) * dumpWidth * 4;
				for (uint32_t x = 0; x < dumpWidth; x++) {
					out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = static_cast<uint8_t>(255 - source[x]);
					out[x * 4 + 3] = 255;
				}
			}
			a_out.push_back(std::move(image));
		}

		// M8 reflections, when the dump frame traced them: REBLUR_SPECULAR's noisy input and the composite's input.
		if (dumpImageCount >= kDumpImages) {
			radianceImage("gi_reflections_noisy", 3, true, 1.0f);
			radianceImage("gi_reflections", 4, false, 1.0f);
		}
		D3D12_RANGE noWrite{ 0, 0 };
		dumpReadback->Unmap(0, &noWrite);
	}
}

#endif
