#include "Sidecar.h"

#include "DebugDump.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>

namespace RT
{
	namespace
	{
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

		D3D12_RESOURCE_DESC BufferDesc(uint64_t a_bytes)
		{
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = a_bytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc = { 1, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return desc;
		}

		HRESULT CreateBuffer(ID3D12Device* a_device, D3D12_HEAP_TYPE a_heap, D3D12_HEAP_FLAGS a_flags, uint64_t a_bytes, D3D12_RESOURCE_STATES a_state, ID3D12Resource** a_out)
		{
			D3D12_HEAP_PROPERTIES heap{ .Type = a_heap };
			auto desc = BufferDesc(a_bytes);
			return a_device->CreateCommittedResource(&heap, a_flags, &desc, a_state, nullptr, IID_PPV_ARGS(a_out));
		}

		std::string BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP a_op)
		{
			switch (a_op) {
			case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:
				return "SetMarker";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:
				return "BeginEvent";
			case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:
				return "EndEvent";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:
				return "Dispatch";
			case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
				return "CopyBufferRegion";
			case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
				return "CopyTextureRegion";
			case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:
				return "CopyResource";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:
				return "ClearUnorderedAccessView";
			case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
				return "ResourceBarrier";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:
				return "ResolveQueryData";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:
				return "BeginSubmission";
			case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:
				return "EndSubmission";
			case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
				return "BuildRaytracingAccelerationStructure";
			case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO:
				return "EmitRaytracingAccelerationStructurePostbuildInfo";
			case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE:
				return "CopyRaytracingAccelerationStructure";
			default:
				return std::format("op#{}", static_cast<int>(a_op));
			}
		}

		float ElapsedMs(LARGE_INTEGER a_start, LARGE_INTEGER a_end, LARGE_INTEGER a_frequency)
		{
			return static_cast<float>(static_cast<double>(a_end.QuadPart - a_start.QuadPart) * 1000.0 / static_cast<double>(a_frequency.QuadPart));
		}
	}

	bool Sidecar::Init(winrt::com_ptr<ID3D12Device> a_device, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context, uint32_t a_screenWidth, uint32_t a_screenHeight)
	{
		device = std::move(a_device);
		QueryPerformanceFrequency(&qpcFrequency);
		QueryPerformanceCounter(&startTime);

		auto fail = [this](std::string a_reason) {
			failureReason = std::move(a_reason);
			logger::error("[SkyrimRT] Sidecar setup failed: {}", failureReason);
			return false;
		};

		HRESULT hr = a_d3d11Device->QueryInterface(IID_PPV_ARGS(d3d11Device.put()));
		if (FAILED(hr))
			return fail(std::format("ID3D11Device5 unavailable ({})", FormatHResult(hr)));
		hr = a_d3d11Context->QueryInterface(IID_PPV_ARGS(d3d11Context.put()));
		if (FAILED(hr))
			return fail(std::format("ID3D11DeviceContext4 unavailable ({})", FormatHResult(hr)));

		device->SetName(L"SkyrimRT::Device");
		// DXR 1.1 was verified by the probe, so ID3D12Device5 exists.
		if (hr = device->QueryInterface(IID_PPV_ARGS(device5.put())); FAILED(hr))
			return fail(std::format("ID3D12Device5 unavailable ({})", FormatHResult(hr)));

		D3D12_COMMAND_QUEUE_DESC queueDesc{ .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
		if (hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())); FAILED(hr))
			return fail(std::format("CreateCommandQueue failed ({})", FormatHResult(hr)));
		queue->SetName(L"SkyrimRT::Queue");

		for (uint32_t i = 0; i < kFramesInFlight; i++) {
			if (hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocators[i].put())); FAILED(hr))
				return fail(std::format("CreateCommandAllocator failed ({})", FormatHResult(hr)));
		}
		if (hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].get(), nullptr, IID_PPV_ARGS(commandList.put())); FAILED(hr))
			return fail(std::format("CreateCommandList failed ({})", FormatHResult(hr)));
		commandList->SetName(L"SkyrimRT::CommandList");
		commandList->Close();
		if (hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].get(), nullptr, IID_PPV_ARGS(uploadList.put())); FAILED(hr))
			return fail(std::format("CreateCommandList(upload) failed ({})", FormatHResult(hr)));
		uploadList->SetName(L"SkyrimRT::UploadList");
		uploadList->Close();

		// One shared fence, used in both directions: D3D11 signals odd values for D3D12 to wait on,
		// D3D12 signals the next value for D3D11 to wait on.
		if (hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.put())); FAILED(hr))
			return fail(std::format("CreateFence(SHARED) failed ({})", FormatHResult(hr)));
		fence->SetName(L"SkyrimRT::SharedFence");
		{
			HANDLE sharedHandle = nullptr;
			if (hr = device->CreateSharedHandle(fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle); FAILED(hr))
				return fail(std::format("CreateSharedHandle(fence) failed ({})", FormatHResult(hr)));
			hr = d3d11Device->OpenSharedFence(sharedHandle, IID_PPV_ARGS(d3d11Fence.put()));
			CloseHandle(sharedHandle);
			if (FAILED(hr))
				return fail(std::format("ID3D11Device5::OpenSharedFence failed ({})", FormatHResult(hr)));
		}

		if (!CreatePatternTexture() || !CreatePipeline() || !CreateTimingObjects())
			return false;

		RunSharedBufferSpike();

		if (!meshCache.Init(device5.get(), d3d11Device.get(), d3d11Context.get()))
			return fail("mesh cache upload ring could not be created");

		// M7c. A failure only leaves alpha-tested meshes traced as solid cards.
		alphaAtlasReady = alphaAtlas.Init(d3d11Device.get(), d3d11Context.get(), device5.get());
		ID3D12Resource* atlas12 = alphaAtlasReady ? alphaAtlas.GetResource() : nullptr;

		// M4 ray tracing. A failure here only disables tracing; the M2/M3 interop keeps running.
		raytracerReady = raytracer.Init(device5.get(), d3d11Device.get(), d3d11Context.get(), a_screenWidth, a_screenHeight, atlas12);
		raytracer.SetTimestampFrequency(d3d12TimestampFrequency);

		// M6. Failures only disable GI.
		materialTableReady = materialTable.Init(d3d11Device.get(), d3d11Context.get());
#if defined(SKYRIMRT_NRD)
		if (raytracerReady) {
			gi = std::make_unique<GlobalIllumination>();
			if (gi->Init(device5.get(), d3d11Device.get(), d3d11Context.get(), a_screenWidth, a_screenHeight, raytracer.GetRasterDepth(), atlas12))
				gi->SetTimestampFrequency(d3d12TimestampFrequency);
			else
				gi.reset();
		}
#else
		logger::info("[SkyrimRT] Ray-traced GI not compiled in (build with SKYRIMRT_NRD=ON, private builds only)");
#endif
		{
			D3D11_QUERY_DESC disjointDesc{ .Query = D3D11_QUERY_TIMESTAMP_DISJOINT };
			D3D11_QUERY_DESC timestampDesc{ .Query = D3D11_QUERY_TIMESTAMP };
			for (uint32_t i = 0; i < kFramesInFlight; i++) {
				if (FAILED(d3d11Device->CreateQuery(&disjointDesc, giDisjoint[i].put())) ||
					FAILED(d3d11Device->CreateQuery(&timestampDesc, giBegin[i].put())) ||
					FAILED(d3d11Device->CreateQuery(&timestampDesc, giEnd[i].put())))
					return fail("creating D3D11 GI timestamp queries failed");
			}
		}

		StartWatchdog();
		logger::info("[SkyrimRT] Sidecar running: shared fence OK, test texture created in {} ({}x{})",
			spike.textureCreatedInD3D12 ? "D3D12, opened in D3D11" : "D3D11, opened in D3D12", kPatternSize, kPatternSize);
		return true;
	}

	void Sidecar::StartWatchdog()
	{
		watchdog = std::jthread([this](std::stop_token a_stop) { WatchdogLoop(a_stop); });
	}

	void Sidecar::WatchdogLoop(std::stop_token a_stop)
	{
		using Clock = std::chrono::steady_clock;
		uint64_t lastCompleted = 0;
		auto lastProgress = Clock::now();
		while (!a_stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

			// ID3D12Device and ID3D12Fence are free-threaded; the render thread keeps using them meanwhile.
			const HRESULT removedReason = device->GetDeviceRemovedReason();
			const uint64_t completed = fence->GetCompletedValue();  // UINT64_MAX once the device is removed
			const uint64_t awaited = awaitedFenceValue.load();
			if (removedReason != S_OK) {
				// A removed device's fences read UINT64_MAX to every opener, D3D11 included (measured), so every D3D11
				// wait on ours is already released; the render thread notices on its next round trip.
				logger::critical("[SkyrimRT] Watchdog: D3D12 device removed ({}) while D3D11 awaited fence value {}; D3D11 sees the fence at {:#x}",
					FormatHResult(removedReason), awaited, d3d11Fence->GetCompletedValue());
				hangRescued = true;
				return;
			}
			const auto now = Clock::now();
			if (completed != lastCompleted || completed >= awaited) {
				lastCompleted = completed;
				lastProgress = now;
				continue;
			}
			if (now - lastProgress <= std::chrono::seconds(kStallSeconds))
				continue;
			// GPU waits never time out, so a queue stuck on a fence would never TDR: remove the device ourselves, which
			// sets its fences to UINT64_MAX and releases D3D11. The sidecar has its own device (RT::CreateSidecarDevice),
			// so frame generation's device, the one the game presents through, is unaffected.
			logger::critical("[SkyrimRT] Watchdog: D3D12 made no progress for {} s (fence {} of {} awaited by D3D11); removing the sidecar device",
				kStallSeconds, completed, awaited);
			device5->RemoveDevice();
			logger::critical("[SkyrimRT] Watchdog: device removed ({}); D3D11 sees the fence at {:#x}; ray tracing is off until restart",
				FormatHResult(device->GetDeviceRemovedReason()), d3d11Fence->GetCompletedValue());
			hangRescued = true;
			return;
		}
	}

	bool Sidecar::CreatePatternTexture()
	{
		auto fail = [this](std::string a_reason) {
			failureReason = std::move(a_reason);
			logger::error("[SkyrimRT] Sidecar setup failed: {}", failureReason);
			return false;
		};

		// Preferred direction (ARCHITECTURE §2): create in D3D12 on a shared heap, open in D3D11.
		D3D12_HEAP_PROPERTIES defaultHeap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC texDesc{};
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Width = kPatternSize;
		texDesc.Height = kPatternSize;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texDesc.SampleDesc = { 1, 0 };
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		HRESULT hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(patternTexture.put()));
		if (SUCCEEDED(hr)) {
			HANDLE sharedHandle = nullptr;
			hr = device->CreateSharedHandle(patternTexture.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
			if (SUCCEEDED(hr)) {
				hr = d3d11Device->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(patternTexture11.put()));
				CloseHandle(sharedHandle);
			}
		}
		spike.textureD3D12ToD3D11 = hr;
		spike.textureCreatedInD3D12 = SUCCEEDED(hr);

		if (FAILED(hr)) {
			// Fallback: the direction CS's DX12SwapChain::WrappedResource already uses.
			logger::warn("[SkyrimRT] D3D12->D3D11 texture sharing failed ({}), trying D3D11->D3D12", FormatHResult(hr));
			patternTexture = nullptr;
			patternTexture11 = nullptr;

			D3D11_TEXTURE2D_DESC desc11{};
			desc11.Width = kPatternSize;
			desc11.Height = kPatternSize;
			desc11.MipLevels = 1;
			desc11.ArraySize = 1;
			desc11.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc11.SampleDesc = { 1, 0 };
			desc11.Usage = D3D11_USAGE_DEFAULT;
			desc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			hr = d3d11Device->CreateTexture2D(&desc11, nullptr, patternTexture11.put());
			if (SUCCEEDED(hr)) {
				winrt::com_ptr<IDXGIResource1> dxgiResource;
				hr = patternTexture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
				HANDLE sharedHandle = nullptr;
				if (SUCCEEDED(hr))
					hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
				if (SUCCEEDED(hr)) {
					hr = device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(patternTexture.put()));
					CloseHandle(sharedHandle);
				}
			}
			spike.textureD3D11ToD3D12 = hr;
			if (FAILED(hr))
				return fail(std::format("texture sharing failed in both directions (D3D12->D3D11 {}, D3D11->D3D12 {})", FormatHResult(spike.textureD3D12ToD3D11), FormatHResult(hr)));
		}
		patternTexture->SetName(L"SkyrimRT::TestPattern");
		Util::SetResourceName(patternTexture11.get(), "SkyrimRT::TestPattern");

		if (hr = d3d11Device->CreateShaderResourceView(patternTexture11.get(), nullptr, patternSRV11.put()); FAILED(hr))
			return fail(std::format("CreateShaderResourceView(test pattern) failed ({})", FormatHResult(hr)));
		Util::SetResourceName(patternSRV11.get(), "SkyrimRT::TestPattern SRV");

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = 1, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		if (hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(descriptorHeap.put())); FAILED(hr))
			return fail(std::format("CreateDescriptorHeap failed ({})", FormatHResult(hr)));

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(patternTexture.get(), nullptr, &uavDesc, descriptorHeap->GetCPUDescriptorHandleForHeapStart());

		// Readback buffer for the debug dump PNG.
		auto actualDesc = patternTexture->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT64 totalBytes = 0;
		device->GetCopyableFootprints(&actualDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
		patternRowPitch = footprint.Footprint.RowPitch;
		if (hr = CreateBuffer(device.get(), D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE, totalBytes, D3D12_RESOURCE_STATE_COPY_DEST, patternReadback.put()); FAILED(hr))
			return fail(std::format("create pattern readback buffer failed ({})", FormatHResult(hr)));
		patternReadback->SetName(L"SkyrimRT::TestPatternReadback");

		return true;
	}

	bool Sidecar::CreatePipeline()
	{
		auto fail = [this](std::string a_reason) {
			failureReason = std::move(a_reason);
			logger::error("[SkyrimRT] Sidecar setup failed: {}", failureReason);
			return false;
		};

		// SM 5.1 DXBC through D3DCompile is enough for the M2 test pattern; RayQuery (M4) will need DXC / SM 6.5.
#ifndef NDEBUG
		const UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
		const UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
		winrt::com_ptr<ID3DBlob> shader;
		winrt::com_ptr<ID3DBlob> errors;
		HRESULT hr = D3DCompileFromFile(L"Data\\Shaders\\SkyrimRT\\TestPatternCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1", compileFlags, 0, shader.put(), errors.put());
		if (FAILED(hr))
			return fail(std::format("compiling Data\\Shaders\\SkyrimRT\\TestPatternCS.hlsl failed ({}): {}", FormatHResult(hr),
				errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "no compiler output"s));

		D3D12_DESCRIPTOR_RANGE uavRange{};
		uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRange.NumDescriptors = 1;
		uavRange.BaseShaderRegister = 0;
		uavRange.OffsetInDescriptorsFromTableStart = 0;

		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0: Time, FrameIndex, Size
		params[0].Constants = { .ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 4 };
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // u0: Output
		params[1].DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &uavRange };
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{ .NumParameters = 2, .pParameters = params };
		winrt::com_ptr<ID3DBlob> rootBlob;
		errors = nullptr;
		if (hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), errors.put()); FAILED(hr))
			return fail(std::format("D3D12SerializeRootSignature failed ({})", FormatHResult(hr)));
		if (hr = device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())); FAILED(hr))
			return fail(std::format("CreateRootSignature failed ({})", FormatHResult(hr)));

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
		if (hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pipeline.put())); FAILED(hr))
			return fail(std::format("CreateComputePipelineState failed ({})", FormatHResult(hr)));
		pipeline->SetName(L"SkyrimRT::TestPatternPSO");

		return true;
	}

	bool Sidecar::CreateTimingObjects()
	{
		auto fail = [this](std::string a_reason) {
			failureReason = std::move(a_reason);
			logger::error("[SkyrimRT] Sidecar setup failed: {}", failureReason);
			return false;
		};

		D3D12_QUERY_HEAP_DESC heapDesc{ .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2 * kFramesInFlight };
		HRESULT hr = device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(timestampHeap.put()));
		if (FAILED(hr))
			return fail(std::format("CreateQueryHeap failed ({})", FormatHResult(hr)));
		if (hr = CreateBuffer(device.get(), D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE, sizeof(uint64_t) * 2 * kFramesInFlight, D3D12_RESOURCE_STATE_COPY_DEST, timestampReadback.put()); FAILED(hr))
			return fail(std::format("create timestamp readback failed ({})", FormatHResult(hr)));
		// Readback heaps may stay mapped; each slot is only read after its fence value completes.
		void* mapped = nullptr;
		if (hr = timestampReadback->Map(0, nullptr, &mapped); FAILED(hr))
			return fail(std::format("map timestamp readback failed ({})", FormatHResult(hr)));
		timestampData = static_cast<const uint64_t*>(mapped);
		queue->GetTimestampFrequency(&d3d12TimestampFrequency);

		D3D11_QUERY_DESC disjointDesc{ .Query = D3D11_QUERY_TIMESTAMP_DISJOINT };
		D3D11_QUERY_DESC timestampDesc{ .Query = D3D11_QUERY_TIMESTAMP };
		for (uint32_t i = 0; i < kFramesInFlight; i++) {
			if (FAILED(d3d11Device->CreateQuery(&disjointDesc, d3d11Disjoint[i].put())) ||
				FAILED(d3d11Device->CreateQuery(&timestampDesc, d3d11Begin[i].put())) ||
				FAILED(d3d11Device->CreateQuery(&timestampDesc, d3d11End[i].put())))
				return fail("creating D3D11 timestamp queries failed");
		}
		return true;
	}

	bool Sidecar::WaitForFenceBlocking(uint64_t a_value)
	{
		if (fence->GetCompletedValue() >= a_value)
			return true;
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!event)
			return false;
		fence->SetEventOnCompletion(a_value, event);
		const bool signaled = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
		CloseHandle(event);
		return signaled;
	}

	void Sidecar::RunSharedBufferSpike()
	{
		// ARCHITECTURE §2 spike: can geometry buffers be shared between the devices? Runs once while the game
		// is loading, so the CPU waits below are acceptable (they are never on the frame path).
		spike.ran = true;
		constexpr uint32_t kCount = 16384;
		constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
		auto expected = [](uint32_t i) { return (i * 2654435761u) ^ 0xA5A5A5A5u; };

		// A) D3D12 shared-heap buffer, opened in D3D11, data verified through a D3D11 staging copy.
		winrt::com_ptr<ID3D12Resource> buffer12;
		winrt::com_ptr<ID3D11Buffer> buffer11;
		HRESULT hr = CreateBuffer(device.get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED, kBytes, D3D12_RESOURCE_STATE_COMMON, buffer12.put());
		if (SUCCEEDED(hr)) {
			HANDLE sharedHandle = nullptr;
			hr = device->CreateSharedHandle(buffer12.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
			if (SUCCEEDED(hr)) {
				hr = d3d11Device->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(buffer11.put()));
				CloseHandle(sharedHandle);
			}
		}
		spike.bufferD3D12ToD3D11Open = hr;

		if (SUCCEEDED(hr)) {
			winrt::com_ptr<ID3D12Resource> upload;
			void* mapped = nullptr;
			if (SUCCEEDED(CreateBuffer(device.get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ, upload.put())) &&
				SUCCEEDED(upload->Map(0, nullptr, &mapped))) {
				auto* values = static_cast<uint32_t*>(mapped);
				for (uint32_t i = 0; i < kCount; i++)
					values[i] = expected(i);
				upload->Unmap(0, nullptr);

				allocators[0]->Reset();
				commandList->Reset(allocators[0].get(), nullptr);
				commandList->CopyBufferRegion(buffer12.get(), 0, upload.get(), 0, kBytes);  // COMMON promotes to COPY_DEST for buffers
				commandList->Close();
				ID3D12CommandList* lists[] = { commandList.get() };
				queue->ExecuteCommandLists(1, lists);
				const uint64_t written = ++fenceValue;
				queue->Signal(fence.get(), written);

				D3D11_BUFFER_DESC stagingDesc{ .ByteWidth = static_cast<UINT>(kBytes), .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ };
				winrt::com_ptr<ID3D11Buffer> staging;
				if (SUCCEEDED(d3d11Device->CreateBuffer(&stagingDesc, nullptr, staging.put()))) {
					d3d11Context->Wait(d3d11Fence.get(), written);
					d3d11Context->CopyResource(staging.get(), buffer11.get());
					D3D11_MAPPED_SUBRESOURCE read{};
					if (SUCCEEDED(d3d11Context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &read))) {
						const auto* values11 = static_cast<const uint32_t*>(read.pData);
						for (uint32_t i = 0; i < kCount; i++)
							spike.bufferMismatches += values11[i] != expected(i);
						d3d11Context->Unmap(staging.get(), 0);
						spike.bufferD3D12ToD3D11Verified = spike.bufferMismatches == 0;
					}
				}
				WaitForFenceBlocking(written);
			}
		}

		// B) D3D11 buffer with an NT shared handle, opened in D3D12.
		D3D11_BUFFER_DESC desc11{};
		desc11.ByteWidth = static_cast<UINT>(kBytes);
		desc11.Usage = D3D11_USAGE_DEFAULT;
		desc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		winrt::com_ptr<ID3D11Buffer> shared11;
		spike.bufferD3D11Create = d3d11Device->CreateBuffer(&desc11, nullptr, shared11.put());
		if (SUCCEEDED(spike.bufferD3D11Create)) {
			winrt::com_ptr<IDXGIResource1> dxgiResource;
			HANDLE sharedHandle = nullptr;
			hr = shared11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
			if (SUCCEEDED(hr))
				hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
			if (SUCCEEDED(hr)) {
				winrt::com_ptr<ID3D12Resource> opened;
				hr = device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(opened.put()));
				CloseHandle(sharedHandle);
			}
			spike.bufferD3D11ToD3D12Open = hr;
		}

		logger::info("[SkyrimRT] Shared-buffer spike: D3D12->D3D11 open {} data {} ({} mismatches of {}); D3D11->D3D12 create {} open {}",
			FormatHResult(spike.bufferD3D12ToD3D11Open),
			spike.bufferD3D12ToD3D11Verified ? "verified" : "NOT verified",
			spike.bufferMismatches, kCount,
			FormatHResult(spike.bufferD3D11Create),
			FormatHResult(spike.bufferD3D11ToD3D12Open));
	}

	void Sidecar::CheckDeviceRemoved()
	{
		stats.d3d12RemovedReason = device->GetDeviceRemovedReason();
		stats.d3d11RemovedReason = d3d11Device->GetDeviceRemovedReason();
		if (stats.d3d12RemovedReason == S_OK && stats.d3d11RemovedReason == S_OK && !hangRescued)
			return;

		deviceRemoved = true;
		stats.deviceRemoved = true;
		logger::critical("[SkyrimRT] Device removed: D3D12 {} D3D11 {}; interop stopped{}", FormatHResult(stats.d3d12RemovedReason), FormatHResult(stats.d3d11RemovedReason),
			hangRescued ? " (seen first by the watchdog)" : "");
		LogDeviceRemovedDetails();
	}

	void Sidecar::BeginMarkerLog(ID3D12GraphicsCommandList* a_list)
	{
		auto& log = markerLogs[nextMarkerLog];
		nextMarkerLog = (nextMarkerLog + 1) % markerLogs.size();
		log.list = a_list;
		log.names.clear();
		CurrentPassMarkerLog() = &log;
	}

	void Sidecar::EndMarkerLog()
	{
		CurrentPassMarkerLog() = nullptr;
	}

	void Sidecar::LogDeviceRemovedDetails()
	{
		// DRED is on in debug builds and with SkyrimRT's "GPU hang diagnostics" setting (RT::EnableDebugLayer).
		winrt::com_ptr<ID3D12DeviceRemovedExtendedData2> dred;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(dred.put())))) {
			logger::critical("[SkyrimRT] DRED: unavailable (enable GPU hang diagnostics and restart to record where the GPU stopped)");
			return;
		}
		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
		if (FAILED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)) || !breadcrumbs.pHeadAutoBreadcrumbNode) {
			logger::critical("[SkyrimRT] DRED: no command list was executing (e.g. the queue was stuck on a fence wait), or GPU hang diagnostics are off");
		}
		auto narrow = [](const wchar_t* a_text) { return a_text ? stl::utf16_to_utf8(a_text).value_or("?") : std::string("?"); };
		for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
			const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			logger::critical("[SkyrimRT] DRED: list '{}' on queue '{}': {} of {} ops completed", narrow(node->pCommandListDebugNameW),
				narrow(node->pCommandQueueDebugNameW), done, node->BreadcrumbCount);
			if (done >= node->BreadcrumbCount || !node->pCommandHistory)
				continue;
			// The pass the GPU was in: the last marker at or before the first unfinished op. DRED's own context strings
			// when it has them, else the nth SetMarker op matched to the nth name recorded for a list with that many.
			std::vector<uint32_t> markerOps;
			for (uint32_t i = 0; i < node->BreadcrumbCount; i++) {
				if (node->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_SETMARKER)
					markerOps.push_back(i);
			}
			const PassMarkerLog* names = nullptr;
			for (const auto& log : markerLogs) {
				if (log.list == node->pCommandList && log.names.size() == markerOps.size())
					names = &log;  // lists of one kind record the same markers every frame, so any match names them
			}
			std::string lastMarker = "(none)";
			for (uint32_t c = 0; c < node->BreadcrumbContextsCount; c++) {
				const auto& context = node->pBreadcrumbContexts[c];
				if (context.BreadcrumbIndex <= done && context.pContextString)
					lastMarker = narrow(context.pContextString);
			}
			for (size_t m = 0; m < markerOps.size() && markerOps[m] <= done; m++)
				lastMarker = names ? narrow(names->names[m]) : std::format("marker #{} (op {})", m, markerOps[m]);
			logger::critical("[SkyrimRT] DRED:   stopped in pass '{}' ({} markers at ops {})", lastMarker, markerOps.size(),
				[&] {
					std::string s;
					for (size_t m = 0; m < markerOps.size(); m++)
						s += std::format("{}{}{}", m ? ", " : "", markerOps[m], names ? std::format(" '{}'", narrow(names->names[m])) : "");
					return s;
				}());
			const uint32_t first = done > 8 ? done - 8 : 0;
			const uint32_t last = std::min(node->BreadcrumbCount, done + 8);
			for (uint32_t i = first; i < last; i++) {
				std::string marker;
				for (uint32_t c = 0; c < node->BreadcrumbContextsCount; c++) {
					if (node->pBreadcrumbContexts[c].BreadcrumbIndex == i && node->pBreadcrumbContexts[c].pContextString)
						marker = " '" + narrow(node->pBreadcrumbContexts[c].pContextString) + "'";
				}
				if (const auto it = std::ranges::find(markerOps, i); marker.empty() && names && it != markerOps.end())
					marker = " '" + narrow(names->names[it - markerOps.begin()]) + "'";
				logger::critical("[SkyrimRT] DRED:   {} op {} {}{}", i < done ? "done" : (i == done ? ">>> " : "    "), i,
					BreadcrumbOpName(node->pCommandHistory[i]), marker);
			}
		}
		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA) {
			logger::critical("[SkyrimRT] DRED: page fault at VA {:#x}", pageFault.PageFaultVA);
			for (auto* node = pageFault.pHeadExistingAllocationNode; node; node = node->pNext)
				logger::critical("[SkyrimRT] DRED:   existing allocation there: {}", node->ObjectNameA ? node->ObjectNameA : "?");
			for (auto* node = pageFault.pHeadRecentFreedAllocationNode; node; node = node->pNext)
				logger::critical("[SkyrimRT] DRED:   recently freed allocation there: {}", node->ObjectNameA ? node->ObjectNameA : "?");
		}
	}

	void Sidecar::CollectTimings(uint32_t a_slot)
	{
		if (!slotHasTimings[a_slot])
			return;
		slotHasTimings[a_slot] = false;

		// This slot's fence value has completed, so its resolved D3D12 timestamps are valid.
		const uint64_t begin12 = timestampData[a_slot * 2];
		const uint64_t end12 = timestampData[a_slot * 2 + 1];
		if (end12 > begin12 && d3d12TimestampFrequency)
			stats.d3d12DispatchMs.Add(static_cast<float>(static_cast<double>(end12 - begin12) * 1000.0 / static_cast<double>(d3d12TimestampFrequency)));

		// D3D11 queries are polled without flushing; a sample that isn't ready is dropped, never waited for.
		auto* ctx = d3d11Context.get();
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		UINT64 begin11 = 0;
		UINT64 end11 = 0;
		if (ctx->GetData(d3d11Disjoint[a_slot].get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			!disjoint.Disjoint && disjoint.Frequency &&
			ctx->GetData(d3d11Begin[a_slot].get(), &begin11, sizeof(begin11), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			ctx->GetData(d3d11End[a_slot].get(), &end11, sizeof(end11), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			end11 >= begin11)
			stats.roundTripMs.Add(static_cast<float>(static_cast<double>(end11 - begin11) * 1000.0 / static_cast<double>(disjoint.Frequency)));

#if defined(SKYRIMRT_NRD)
		if (slotHasGITimings[a_slot] && gi) {
			slotHasGITimings[a_slot] = false;
			if (ctx->GetData(giDisjoint[a_slot].get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				!disjoint.Disjoint && disjoint.Frequency &&
				ctx->GetData(giBegin[a_slot].get(), &begin11, sizeof(begin11), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				ctx->GetData(giEnd[a_slot].get(), &end11, sizeof(end11), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				end11 >= begin11)
				gi->GetStats().roundTripMs.Add(static_cast<float>(static_cast<double>(end11 - begin11) * 1000.0 / static_cast<double>(disjoint.Frequency)));
		}
#endif
	}

	void Sidecar::FinishDumpIfReady()
	{
		if (dumpStage != DumpStage::kFinishing || dumpFenceValue > stats.lastCompletedFenceValue)
			return;
		auto* ctx = d3d11Context.get();
		if (!captureOn.Poll(ctx) || (!captureOff.IsIdle() && !captureOff.Poll(ctx)))
			return;
		dumpStage = DumpStage::kIdle;

		DebugDumpData data;
		data.gameFrame = dumpGameFrame;
		data.patternFrame = dumpPatternFrame;
		data.width = kPatternSize;
		data.height = kPatternSize;
		data.pixels.resize(static_cast<size_t>(kPatternSize) * kPatternSize * 4);

		void* mapped = nullptr;
		D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(patternRowPitch) * kPatternSize };
		if (FAILED(patternReadback->Map(0, &readRange, &mapped))) {
			logger::error("[SkyrimRT] Debug dump: mapping the pattern readback failed");
			return;
		}
		for (uint32_t y = 0; y < kPatternSize; y++)
			memcpy(data.pixels.data() + static_cast<size_t>(y) * kPatternSize * 4, static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * patternRowPitch, kPatternSize * 4);
		D3D12_RANGE noWrite{ 0, 0 };
		patternReadback->Unmap(0, &noWrite);

		// The shader's top-left checker is white on odd pattern frames and black on even ones, so pixel (0,0)
		// proves the readback holds the dispatch of this exact frame rather than a stale copy.
		const uint8_t expected = (dumpPatternFrame & 1) ? 255 : 0;
		data.patternVerified = data.pixels[0] == expected && data.pixels[1] == expected && data.pixels[2] == expected;

		data.caps = GetCapabilities();
		data.stats = stats;
		data.spike = spike;
		data.inWorld = inWorld;
		data.scene = sceneStats;
		data.sceneTraversalMs = sceneTraversalMs;
		data.cache = meshCache.GetStats();
		if (raytracerReady) {
			data.haveTrace = dumpHasTrace;
			data.trace = raytracer.GetStats();
			raytracer.ReadDumpImages(data.images);  // only what this dump's round trip captured
			if (raytracer.SunShadowsReady()) {
				data.haveShadows = dumpShadowsTraced;
				data.shadows = raytracer.GetSunShadows().GetStats();
			}
			if (raytracer.PointShadowsReady()) {
				data.pointShadowsAvailable = true;
				data.havePointShadows = dumpPointShadowsTraced;
				data.pointShadows = raytracer.GetPointShadows().GetStats();
			}
		}
		if (const auto* skinned = raytracerReady ? raytracer.GetSkinned() : nullptr) {
			data.haveSkinned = true;
			data.skinned = skinned->GetStats();
		}
		data.giCompiledIn = IsGICompiledIn();
		data.materials = materialTable.GetStats();
		data.alphaAtlas = alphaAtlas.GetStats();
		alphaAtlas.ReadDumpImage(data.images);
#if defined(SKYRIMRT_NRD)
		if (gi) {
			data.giAvailable = true;
			data.haveGI = dumpGITraced;
			data.gi = gi->GetStats();
			gi->ReadDumpImages(data.images);
		}
#endif
		for (auto* capture : { &captureOn, &captureOff }) {
			if (capture->IsIdle())
				continue;
			auto image = capture->Take();
			if (!image.pixels.empty())
				data.images.push_back(std::move(image));
		}
		WriteDebugDumpAsync(std::move(data));
	}

	ID3D11ShaderResourceView* Sidecar::AcquireSunShadowMask(uint32_t a_gameFrame)
	{
		if (!raytracerReady || !raytracer.SunShadowsReady())
			return nullptr;
		auto& shadows = raytracer.GetSunShadows();
		// Traced this frame, or last frame when this frame's round trip had to skip a busy slot.
		const bool fresh = shadowTracedEver && (lastShadowGameFrame == a_gameFrame || lastShadowGameFrame + 1 == a_gameFrame);
		if (fresh) {
			maskClearedWhileStale = false;
		} else if (!maskClearedWhileStale) {
			shadows.ClearMask();
			maskClearedWhileStale = true;
		}
		return shadows.GetMaskSRV();
	}

	ID3D11ShaderResourceView* Sidecar::AcquirePointLightShadowMask(uint32_t a_gameFrame)
	{
		if (!raytracerReady || !raytracer.PointShadowsReady())
			return nullptr;
		auto& shadows = raytracer.GetPointShadows();
		const bool fresh = pointShadowTracedEver && (lastPointShadowGameFrame == a_gameFrame || lastPointShadowGameFrame + 1 == a_gameFrame);
		if (fresh) {
			pointMaskClearedWhileStale = false;
		} else if (!pointMaskClearedWhileStale) {
			shadows.ClearMask();
			pointMaskClearedWhileStale = true;
		}
		return shadows.GetMaskSRV();
	}

	void Sidecar::OnPresent(uint32_t a_gameFrame)
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (lastPresent.QuadPart)
			stats.frameMs.Add(ElapsedMs(lastPresent, now, qpcFrequency));
		lastPresent = now;

		if (deviceRemoved)
			return;

		// No-op when SkyrimRT::Prepass already ran this frame's round trip.
		Submit(a_gameFrame, FrameCamera{}, false, nullptr, nullptr, false);
		if (deviceRemoved)
			return;

		auto* ctx = d3d11Context.get();
		if (dumpStage == DumpStage::kCaptureOn) {
			const bool anyRT = dumpShadowsTraced || dumpPointShadowsTraced || dumpGITraced;
			captureOn.Begin(d3d11Device.get(), ctx, anyRT ? "final_rt_on" : "final");
			if (anyRT) {
				dumpStage = DumpStage::kSuppressing;
				suppressUntilFrame = a_gameFrame + kSuppressFrames;
			} else {
				dumpStage = DumpStage::kFinishing;
			}
		} else if (dumpStage == DumpStage::kSuppressing && a_gameFrame >= suppressUntilFrame) {
			captureOff.Begin(d3d11Device.get(), ctx, "final_rt_off");
			dumpStage = DumpStage::kFinishing;
		}

		stats.lastCompletedFenceValue = fence->GetCompletedValue();
		FinishDumpIfReady();
	}

	void Sidecar::Submit(uint32_t a_gameFrame, const FrameCamera& a_camera, bool a_debugTrace, const SunShadowParams* a_shadows,
		const PointShadowParams* a_pointShadows, bool a_buildForGI)
	{
		if (deviceRemoved || (haveSubmitted && lastSubmitGameFrame == a_gameFrame))
			return;
		haveSubmitted = true;
		lastSubmitGameFrame = a_gameFrame;

		LARGE_INTEGER cpuStart;
		QueryPerformanceCounter(&cpuStart);

		CheckDeviceRemoved();
		if (deviceRemoved)
			return;

		stats.lastCompletedFenceValue = fence->GetCompletedValue();

		const uint32_t slot = framesSubmitted % kFramesInFlight;
		if (slotFenceValues[slot] > stats.lastCompletedFenceValue) {
			stats.framesSkipped++;  // GPU still busy with this slot: skip rather than wait on the CPU
			return;
		}
		CollectTimings(slot);
		if (raytracerReady)
			raytracer.CollectResults(slot);
#if defined(SKYRIMRT_NRD)
		if (gi)
			gi->CollectResults(slot);
#endif

		// Scene first: the TLAS is built from this frame's instances.
		inWorld = CollectScene(candidates, skinnedScene, exclusions, loadedArea, sceneStats, treeRestPose);
		if (inWorld)
			sceneTraversalMs.Add(sceneStats.traversalMs);
		// Albedos for the instance data; the candidates' texture pointers are only valid this frame.
		if (inWorld && materialTableReady)
			materialTable.Update(candidates, a_gameFrame);
		// M7c: new alpha-atlas tiles are filled here, on D3D11 before the signal below, so this frame's traces see them.
		if (inWorld && alphaAtlasReady)
			alphaAtlas.Update(candidates, a_gameFrame, alphaTestEnabled);

		// Trace only when the camera was captured for this very frame (SkyrimRT::Prepass), so matrices, depth and
		// transforms all describe the same frame.
		const bool buildScene = raytracerReady && inWorld && a_camera.valid && a_camera.gameFrame == a_gameFrame && (a_debugTrace || a_shadows || a_pointShadows || a_buildForGI);
		const bool debugTrace = buildScene && a_debugTrace;
		const SunShadowParams* shadows = (buildScene && a_shadows && raytracer.SunShadowsReady()) ? a_shadows : nullptr;
		const PointShadowParams* pointShadows = (buildScene && a_pointShadows && raytracer.PointShadowsReady()) ? a_pointShadows : nullptr;
		const bool dumpThisFrame = dumpRequested && dumpStage == DumpStage::kIdle;
		const bool compareShadowMap = dumpThisFrame && shadows;

		auto* allocator = allocators[slot].get();
		allocator->Reset();
		commandList->Reset(allocator, pipeline.get());
		BeginMarkerLog(commandList.get());
		commandList->SetComputeRootSignature(rootSignature.get());
		ID3D12DescriptorHeap* heaps[] = { descriptorHeap.get() };
		commandList->SetDescriptorHeaps(1, heaps);

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		struct
		{
			float time;
			uint32_t frameIndex;
			uint32_t width;
			uint32_t height;
		} constants{ ElapsedMs(startTime, now, qpcFrequency) / 1000.0f, framesSubmitted, kPatternSize, kPatternSize };
		SetPassMarker(commandList.get(), L"SkyrimRT: interop test pattern");
		commandList->SetComputeRoot32BitConstants(0, 4, &constants, 0);
		commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

		commandList->EndQuery(timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
		Transition(commandList.get(), patternTexture.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->Dispatch((kPatternSize + 7) / 8, (kPatternSize + 7) / 8, 1);
		// Back to COMMON before signalling, so D3D11 sees the writes.
		Transition(commandList.get(), patternTexture.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		commandList->EndQuery(timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
		commandList->ResolveQueryData(timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, timestampReadback.get(), sizeof(uint64_t) * slot * 2);

		if (dumpThisFrame) {
			Transition(commandList.get(), patternTexture.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION dst{ .pResource = patternReadback.get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kPatternSize, kPatternSize, 1, patternRowPitch };
			D3D12_TEXTURE_COPY_LOCATION src{ .pResource = patternTexture.get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			src.SubresourceIndex = 0;
			commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			Transition(commandList.get(), patternTexture.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		}

		// M4/M5: BLAS builds, TLAS, debug trace, sun shadows, all before the signal D3D11 waits on, so this frame's
		// opaque pass lights with this frame's mask.
		if (buildScene)
			raytracer.Record(commandList.get(), slot, framesSubmitted, meshCache, candidates, skinnedScene, exclusions, loadedArea, a_camera,
				debugTrace, shadows, pointShadows, compareShadowMap, dumpThisFrame);
		EndMarkerLog();
		commandList->Close();

		// D3D11 -> D3D12: everything the game queued so far (including last frame's reads of the shared textures)
		// happens before D3D12 may touch them again. Signalled only after recording: the D3D11 GPU waits from here
		// until D3D12 finishes, so CPU recording time must not fall inside that window (M7: ~3,600 skinning/refit
		// commands made it ~1 ms).
		auto* ctx = d3d11Context.get();
		ctx->Begin(d3d11Disjoint[slot].get());
		ctx->End(d3d11Begin[slot].get());
		if (buildScene)
			raytracer.CopyInputs(compareShadowMap);
		const uint64_t toD3D12 = ++fenceValue;
		ctx->Signal(d3d11Fence.get(), toD3D12);
		ctx->Flush();  // let the D3D12 queue start as soon as possible instead of at Present

		queue->Wait(fence.get(), toD3D12);
		if (simulateHang.exchange(false)) {
			// Debug (RT::SimulateGpuHang): wait on a value nothing signals, as a hung queue would, until the watchdog removes the device.
			logger::warn("[SkyrimRT] Simulating a GPU hang: the D3D12 queue now waits until the watchdog steps in");
			queue->Wait(fence.get(), kSimulatedHangValue);
		}
		ID3D12CommandList* lists[] = { commandList.get() };
		queue->ExecuteCommandLists(1, lists);
		const uint64_t toD3D11 = ++fenceValue;
		queue->Signal(fence.get(), toD3D11);
		slotFenceValues[slot] = toD3D11;

		// D3D12 -> D3D11: the overlay samples the pattern after this point in the D3D11 queue.
		awaitedFenceValue = toD3D11;
		ctx->Wait(d3d11Fence.get(), toD3D11);
		ctx->End(d3d11End[slot].get());
		ctx->End(d3d11Disjoint[slot].get());
		slotHasTimings[slot] = true;

		// M3: stream new meshes into the cache. The upload list runs after the signal D3D11 waits on, so uploads
		// never lengthen the D3D11 round trip.
		uploadList->Reset(allocator, nullptr);
		BeginMarkerLog(uploadList.get());
		SetPassMarker(uploadList.get(), L"SkyrimRT: mesh uploads");
		const uint64_t uploadFence = fenceValue + 1;
		const bool uploadsRecorded = meshCache.Update(candidates, framesSubmitted, stats.lastCompletedFenceValue, uploadList.get(), uploadFence);
		EndMarkerLog();
		uploadList->Close();
		if (uploadsRecorded) {
			ID3D12CommandList* uploadLists[] = { uploadList.get() };
			queue->ExecuteCommandLists(1, uploadLists);
			queue->Signal(fence.get(), ++fenceValue);
			slotFenceValues[slot] = fenceValue;
		}

		if (shadows) {
			shadowTracedEver = true;
			lastShadowGameFrame = a_gameFrame;
		}
		if (pointShadows) {
			pointShadowTracedEver = true;
			lastPointShadowGameFrame = a_gameFrame;
		}
		if (buildScene) {
			sceneBuilt = true;
			sceneGameFrame = a_gameFrame;
			sceneSlot = slot;
			sceneCamera = a_camera;
			sceneRenderWidth = std::min(a_camera.renderWidth, raytracer.GetTextureWidth());
			sceneRenderHeight = std::min(a_camera.renderHeight, raytracer.GetTextureHeight());
		}

		if (dumpThisFrame) {
			if (alphaAtlasReady)
				alphaAtlas.CaptureForDump();
			dumpHasTrace = debugTrace;
			dumpShadowsTraced = shadows != nullptr;
			dumpPointShadowsTraced = pointShadows != nullptr;
			dumpGITraced = false;  // set by SubmitGI later this frame
			dumpRequested = false;
			dumpStage = DumpStage::kCaptureOn;
			dumpFenceValue = toD3D11;
			dumpPatternFrame = framesSubmitted;
			dumpGameFrame = a_gameFrame;
		}

		stats.lastSignaledFenceValue = fenceValue;
		stats.framesSubmitted = ++framesSubmitted;

		LARGE_INTEGER cpuEnd;
		QueryPerformanceCounter(&cpuEnd);
		stats.cpuSubmitMs.Add(ElapsedMs(cpuStart, cpuEnd, qpcFrequency));
	}

	bool Sidecar::IsGICompiledIn() const
	{
#if defined(SKYRIMRT_NRD)
		return true;
#else
		return false;
#endif
	}

	bool Sidecar::CanTraceGI([[maybe_unused]] uint32_t a_gameFrame) const
	{
#if defined(SKYRIMRT_NRD)
		return !deviceRemoved && gi && sceneBuilt && sceneGameFrame == a_gameFrame;
#else
		return false;
#endif
	}

	const GIStats* Sidecar::GetGIStats() const
	{
#if defined(SKYRIMRT_NRD)
		return gi ? &gi->GetStats() : nullptr;
#else
		return nullptr;
#endif
	}

	ID3D11ShaderResourceView* Sidecar::GetGIViewSRV() const
	{
#if defined(SKYRIMRT_NRD)
		return gi ? gi->GetViewSRV() : nullptr;
#else
		return nullptr;
#endif
	}

	GIOutputs Sidecar::SubmitGI([[maybe_unused]] uint32_t a_gameFrame, [[maybe_unused]] const GIParams& a_params)
	{
#if defined(SKYRIMRT_NRD)
		if (!CanTraceGI(a_gameFrame))
			return {};
		CheckDeviceRemoved();
		if (deviceRemoved)
			return {};

		// Same frame slot as this frame's Submit: its allocator already holds this frame's lists (not reset), and
		// the TLAS and instance data GI reads are that slot's.
		const uint32_t slot = sceneSlot;
		auto* ctx = d3d11Context.get();
		// The copies are queued on D3D11 now (they also create the shared inputs on first use, which Record needs);
		// the signal waits until the list is recorded, so CPU recording time isn't spent with the D3D11 GPU stalled.
		if (!gi->CopyInputs())
			return {};

		const bool captureDump = dumpStage == DumpStage::kCaptureOn && dumpGameFrame == a_gameFrame;
		commandList->Reset(allocators[slot].get(), nullptr);
		BeginMarkerLog(commandList.get());
		gi->Record(commandList.get(), slot, raytracer.GetTlasAddress(), raytracer.GetInstanceDataAddress(slot), meshCache.GetMeshPool(), raytracer.GetSkinned(),
			sceneCamera, sceneRenderWidth, sceneRenderHeight, a_params, captureDump);
		EndMarkerLog();
		commandList->Close();

		ctx->Begin(giDisjoint[slot].get());
		ctx->End(giBegin[slot].get());
		const uint64_t toD3D12 = ++fenceValue;
		ctx->Signal(d3d11Fence.get(), toD3D12);
		ctx->Flush();

		queue->Wait(fence.get(), toD3D12);
		ID3D12CommandList* lists[] = { commandList.get() };
		queue->ExecuteCommandLists(1, lists);
		const uint64_t toD3D11 = ++fenceValue;
		queue->Signal(fence.get(), toD3D11);
		slotFenceValues[slot] = toD3D11;

		awaitedFenceValue = toD3D11;
		ctx->Wait(d3d11Fence.get(), toD3D11);
		ctx->End(giEnd[slot].get());
		ctx->End(giDisjoint[slot].get());
		// Without a flush the closing timestamp waits in the D3D11 command buffer until the game's next flush, and
		// the measured hand-off then includes GPU idle time of a CPU-bound frame (3-11 ms measured, vs 0.5 ms of work).
		ctx->Flush();
		slotHasGITimings[slot] = true;

		if (captureDump) {
			dumpGITraced = true;
			dumpFenceValue = toD3D11;
		}
		stats.lastSignaledFenceValue = fenceValue;
		return gi->GetOutputs();
#else
		return {};
#endif
	}
}
