#include "MaterialTable.h"

#include "RT.h"

namespace RT
{
	namespace
	{
		constexpr uint32_t kFloat4PerSlot = 2;
		constexpr uint32_t kOutputBytes = MaterialTable::kSlotsPerBatch * kFloat4PerSlot * sizeof(float) * 4;

		uint32_t PackRGBA8(const float* a_rgba)
		{
			uint32_t packed = 0;
			for (uint32_t i = 0; i < 4; i++) {
				const float value = std::clamp(a_rgba[i], 0.0f, 1.0f);
				packed |= static_cast<uint32_t>(value * 255.0f + 0.5f) << (8 * i);
			}
			return packed;
		}
	}

	bool MaterialTable::Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		device = a_device;
		context = a_context;

		shader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SkyrimRT\\AverageAlbedoCS.hlsl", {}, "cs_5_0")));
		if (!shader) {
			logger::error("[SkyrimRT] Material table: compiling Data\\Shaders\\SkyrimRT\\AverageAlbedoCS.hlsl failed");
			return false;
		}

		D3D11_BUFFER_DESC outputDesc{ .ByteWidth = kOutputBytes, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(float) * 4 };
		D3D11_BUFFER_DESC paramsDesc{ .ByteWidth = 16, .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE };
		D3D11_BUFFER_DESC stagingDesc{ .ByteWidth = kOutputBytes, .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ };
		if (FAILED(device->CreateBuffer(&outputDesc, nullptr, output.put())) ||
			FAILED(device->CreateUnorderedAccessView(output.get(), nullptr, outputUAV.put())) ||
			FAILED(device->CreateBuffer(&paramsDesc, nullptr, params.put()))) {
			logger::error("[SkyrimRT] Material table: creating buffers failed");
			return false;
		}
		Util::SetResourceName(output.get(), "SkyrimRT::AlbedoAverages");
		Util::SetResourceName(outputUAV.get(), "SkyrimRT::AlbedoAverages UAV");
		Util::SetResourceName(params.get(), "SkyrimRT::AlbedoParams");
		for (auto& batch : batches) {
			if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, batch.staging.put()))) {
				logger::error("[SkyrimRT] Material table: creating staging buffers failed");
				return false;
			}
			Util::SetResourceName(batch.staging.get(), "SkyrimRT::AlbedoReadback");
			batch.keys.reserve(kSlotsPerBatch);
		}
		return true;
	}

	bool MaterialTable::Validate(ID3D11ShaderResourceView* a_srv, Entry& a_entry)
	{
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		a_srv->GetDesc(&viewDesc);

		uint32_t width = 0;
		uint32_t height = 0;
		bool supported = false;
		if (viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
			if (auto texture = resource.try_as<ID3D11Texture2D>()) {
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				width = desc.Width;
				height = desc.Height;
				supported = desc.SampleDesc.Count == 1;
			}
		}

		// A different resource (or size) behind the same SRV address means the old texture unloaded and the
		// address was reused: start over.
		if (a_entry.resource != resource.get() || a_entry.width != width || a_entry.height != height) {
			a_entry = Entry{ .resource = resource.get(), .width = width, .height = height, .supported = supported };
		}
		return a_entry.supported;
	}

	void MaterialTable::CollectBatches()
	{
		for (auto& batch : batches) {
			if (!batch.inFlight)
				continue;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT hr = context->Map(batch.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
				continue;
			batch.inFlight = false;
			if (FAILED(hr)) {
				batch.keys.clear();
				continue;
			}
			const auto* values = static_cast<const float*>(mapped.pData);
			for (uint32_t slot = 0; slot < batch.keys.size(); slot++) {
				auto it = entries.find(batch.keys[slot]);
				if (it == entries.end())
					continue;
				const float* weighted = values + (slot * kFloat4PerSlot + 0) * 4;
				const float* plain = values + (slot * kFloat4PerSlot + 1) * 4;
				// Fully transparent textures have no weighted average; fall back to the plain one.
				it->second.weighted = PackRGBA8(weighted[3] > 1e-3f ? weighted : plain);
				it->second.plain = PackRGBA8(plain);
				it->second.ready = true;
				stats.computedLastFrame++;
			}
			context->Unmap(batch.staging.get(), 0);
			batch.keys.clear();
		}
	}

	void MaterialTable::Update(std::vector<GeometryCandidate>& a_candidates, uint32_t a_frame)
	{
		stats.computedLastFrame = 0;
		stats.candidatesWithoutTexture = 0;
		stats.candidatesDefaulted = 0;
		CollectBatches();

		// The queue only ever holds SRVs seen this frame: game textures may unload between frames.
		queue.clear();
		for (auto& candidate : a_candidates) {
			candidate.albedo = kDefaultAlbedo;
			if (!candidate.diffuseSRV) {
				stats.candidatesWithoutTexture++;
				continue;
			}
			auto& entry = entries[candidate.diffuseSRV];
			if (entry.validatedFrame != a_frame) {
				entry.validatedFrame = a_frame;
				if (Validate(candidate.diffuseSRV, entry) && !entry.ready)
					queue.push_back(candidate.diffuseSRV);
			}
			if (entry.ready)
				candidate.albedo = candidate.alphaTested ? entry.weighted : entry.plain;
			else
				stats.candidatesDefaulted++;
		}

		// Skip textures already waiting in a batch.
		std::erase_if(queue, [&](ID3D11ShaderResourceView* a_srv) {
			return std::ranges::any_of(batches, [&](const Batch& a_batch) { return a_batch.inFlight && std::ranges::find(a_batch.keys, a_srv) != a_batch.keys.end(); });
		});

		auto& batch = batches[nextBatch];
		if (!queue.empty() && !batch.inFlight) {
			const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(queue.size()), kSlotsPerBatch);
			ID3D11UnorderedAccessView* uav = outputUAV.get();
			ID3D11Buffer* cb = params.get();
			context->CSSetShader(shader.get(), nullptr, 0);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetConstantBuffers(0, 1, &cb);
			for (uint32_t slot = 0; slot < count; slot++) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
					break;
				const uint32_t data[4] = { slot, 0, 0, 0 };
				std::memcpy(mapped.pData, data, sizeof(data));
				context->Unmap(cb, 0);
				ID3D11ShaderResourceView* srv = queue[slot];
				context->CSSetShaderResources(0, 1, &srv);
				context->Dispatch(1, 1, 1);
				batch.keys.push_back(queue[slot]);
			}
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			context->CSSetShader(nullptr, nullptr, 0);
			context->CopyResource(batch.staging.get(), output.get());
			batch.inFlight = !batch.keys.empty();
			nextBatch = (nextBatch + 1) % kBatchesInFlight;
		}

		stats.textures = 0;
		stats.pending = 0;
		stats.unsupported = 0;
		for (const auto& [srv, entry] : entries) {
			stats.textures += entry.ready;
			stats.pending += entry.supported && !entry.ready;
			stats.unsupported += !entry.supported;
		}
	}
}
