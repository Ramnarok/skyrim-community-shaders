#include "AlphaAtlas.h"

#include "RT.h"

namespace RT
{
	D3D12_DESCRIPTOR_RANGE GetAlphaAtlasRange(uint32_t a_offsetInTable)
	{
		return { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2, a_offsetInTable };
	}

	D3D12_STATIC_SAMPLER_DESC GetAlphaAtlasSampler()
	{
		D3D12_STATIC_SAMPLER_DESC desc{};
		desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		desc.MaxLOD = D3D12_FLOAT32_MAX;
		desc.ShaderRegister = 0;
		desc.RegisterSpace = 0;
		desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		return desc;
	}

	void WriteAlphaAtlasDescriptor(ID3D12Device* a_device, ID3D12Resource* a_atlas, D3D12_CPU_DESCRIPTOR_HANDLE a_handle)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{ .Format = DXGI_FORMAT_R8_UNORM, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
		desc.Texture2D.MipLevels = 1;
		a_device->CreateShaderResourceView(a_atlas, &desc, a_handle);
	}

	bool AlphaAtlas::Init(ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, ID3D12Device* a_device)
	{
		d3d11Device = a_d3d11Device;
		d3d11Context = a_d3d11Context;
		stats.capacity = kCapacity;

		std::string error;
		if (!CreateSharedTexture(d3d11Device, a_device, kAtlasSize, kAtlasSize, DXGI_FORMAT_R8_UNORM, "AlphaAtlas", atlas, error)) {
			logger::error("[SkyrimRT] Alpha atlas: {}", error);
			atlas = {};
			return false;
		}

		fillCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SkyrimRT\\AlphaAtlasFillCS.hlsl", {}, "cs_5_0")));
		if (!fillCS) {
			logger::error("[SkyrimRT] Alpha atlas: compiling Data\\Shaders\\SkyrimRT\\AlphaAtlasFillCS.hlsl failed");
			return false;
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		D3D11_BUFFER_DESC paramsDesc{ .ByteWidth = 16, .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE };
		if (FAILED(d3d11Device->CreateSamplerState(&samplerDesc, sampler.put())) ||
			FAILED(d3d11Device->CreateBuffer(&paramsDesc, nullptr, params.put()))) {
			logger::error("[SkyrimRT] Alpha atlas: creating the sampler or constant buffer failed");
			return false;
		}
		Util::SetResourceName(params.get(), "SkyrimRT::AlphaAtlasParams");

		tileOwners.assign(kCapacity, nullptr);
		freeTiles.reserve(kCapacity);
		for (uint32_t tile = kCapacity; tile-- > 0;)
			freeTiles.push_back(tile);  // popped from the back: tile 0 first

		stats.available = true;
		logger::info("[SkyrimRT] Alpha atlas ready: {}x{} R8, {} tiles of {}x{}", kAtlasSize, kAtlasSize, kCapacity, kTileSize, kTileSize);
		return true;
	}

	int32_t AlphaAtlas::AllocateTile(uint32_t a_frame)
	{
		if (!freeTiles.empty()) {
			const uint32_t tile = freeTiles.back();
			freeTiles.pop_back();
			return static_cast<int32_t>(tile);
		}

		// Full: recycle the tile of the texture seen longest ago. Textures seen this frame are in use by this frame's
		// instances; the previous frame's D3D12 reads finished before this D3D11 work (the hand-offs wait on the GPU).
		int32_t oldest = -1;
		uint32_t oldestFrame = a_frame;
		for (uint32_t tile = 0; tile < kCapacity; tile++) {
			auto it = entries.find(tileOwners[tile]);
			if (it != entries.end() && it->second.lastSeenFrame < oldestFrame) {
				oldestFrame = it->second.lastSeenFrame;
				oldest = static_cast<int32_t>(tile);
			}
		}
		if (oldest < 0)
			return -1;
		entries.erase(tileOwners[oldest]);
		tileOwners[oldest] = nullptr;
		stats.evictedLastFrame++;
		stats.totalEvictions++;
		return oldest;
	}

	void AlphaAtlas::Fill(ID3D11ShaderResourceView* a_srv, uint32_t a_tile)
	{
		auto* ctx = d3d11Context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(ctx->Map(params.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		const uint32_t data[4] = { (a_tile % kTilesPerRow) * kTileSize, (a_tile / kTilesPerRow) * kTileSize, 0, 0 };
		std::memcpy(mapped.pData, data, sizeof(data));
		ctx->Unmap(params.get(), 0);
		ctx->CSSetShaderResources(0, 1, &a_srv);
		ctx->Dispatch(kTileSize / 8, kTileSize / 8, 1);
		stats.filledLastFrame++;
		stats.totalFills++;
	}

	void AlphaAtlas::Update(std::vector<GeometryCandidate>& a_candidates, uint32_t a_frame, bool a_enabled)
	{
		stats.enabled = a_enabled;
		stats.filledLastFrame = 0;
		stats.evictedLastFrame = 0;
		stats.candidates = 0;
		stats.candidatesTested = 0;
		stats.candidatesNoTexture = 0;
		stats.candidatesNoUV = 0;
		stats.candidatesZeroThreshold = 0;
		stats.candidatesUnsupported = 0;
		stats.candidatesWaiting = 0;
		if (!a_enabled || !stats.available)
			return;  // alphaWord stays 0: traced opaque

		auto* ctx = d3d11Context;
		bool bound = false;
		for (auto& candidate : a_candidates) {
			// Foliage (trees are skinned: their branches sway on bones), hair and the like. Blended meshes aren't
			// traced, and the landscape shader doesn't alpha-test.
			if (!candidate.alphaTested || candidate.alphaBlended || candidate.terrain)
				continue;
			stats.candidates++;
			if (!candidate.diffuseSRV) {
				stats.candidatesNoTexture++;
				continue;
			}
			RE::BSGraphics::VertexDesc desc;
			std::memcpy(&desc, &candidate.vertexDesc, sizeof(desc));  // VertexDesc keeps its bits private
			if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_UV)) {
				stats.candidatesNoUV++;
				continue;
			}
			if (candidate.alphaThreshold == 0) {
				stats.candidatesZeroThreshold++;
				continue;
			}

			auto& entry = entries[candidate.diffuseSRV];
			if (entry.validatedFrame != a_frame) {
				entry.validatedFrame = a_frame;
				const TextureIdentity identity = GetTextureIdentity(candidate.diffuseSRV);
				if (!(identity == entry.identity)) {
					// New texture, or the SRV address was reused by another one: its old tile is stale.
					if (entry.tile >= 0) {
						tileOwners[entry.tile] = nullptr;
						freeTiles.push_back(static_cast<uint32_t>(entry.tile));
					}
					entry.identity = identity;
					entry.tile = -1;
				}
			}
			entry.lastSeenFrame = a_frame;
			if (!entry.identity.supported) {
				stats.candidatesUnsupported++;
				continue;
			}

			if (entry.tile < 0) {
				const int32_t tile = stats.filledLastFrame < kMaxFillsPerFrame ? AllocateTile(a_frame) : -1;
				if (tile < 0) {
					stats.candidatesWaiting++;
					continue;
				}
				if (!bound) {
					ID3D11UnorderedAccessView* uav = atlas.uav11.get();
					ID3D11SamplerState* samplerState = sampler.get();
					ID3D11Buffer* cb = params.get();
					ctx->CSSetShader(fillCS.get(), nullptr, 0);
					ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
					ctx->CSSetSamplers(0, 1, &samplerState);
					ctx->CSSetConstantBuffers(0, 1, &cb);
					bound = true;
				}
				Fill(candidate.diffuseSRV, static_cast<uint32_t>(tile));
				entry.tile = tile;
				tileOwners[tile] = candidate.diffuseSRV;
			}

			const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			candidate.alphaWord = (static_cast<uint32_t>(entry.tile) + 1) | (static_cast<uint32_t>(candidate.alphaThreshold) << 12) | (uvOffset << 20);
			stats.candidatesTested++;
		}

		if (bound) {
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			ID3D11SamplerState* nullSampler = nullptr;
			ID3D11Buffer* nullCB = nullptr;
			ctx->CSSetShaderResources(0, 1, &nullSRV);
			ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			ctx->CSSetSamplers(0, 1, &nullSampler);
			ctx->CSSetConstantBuffers(0, 1, &nullCB);
			ctx->CSSetShader(nullptr, nullptr, 0);
		}
		stats.tilesUsed = kCapacity - static_cast<uint32_t>(freeTiles.size());
	}

	void AlphaAtlas::CaptureForDump()
	{
		if (!stats.available)
			return;
		if (!dumpStaging) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kAtlasSize;
			desc.Height = kDumpRows * kTileSize;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(d3d11Device->CreateTexture2D(&desc, nullptr, dumpStaging.put()))) {
				logger::error("[SkyrimRT] Debug dump: cannot create the alpha atlas staging texture");
				return;
			}
			Util::SetResourceName(dumpStaging.get(), "SkyrimRT::AlphaAtlasDump");
		}
		const D3D11_BOX box{ 0, 0, 0, kAtlasSize, kDumpRows * kTileSize, 1 };
		d3d11Context->CopySubresourceRegion(dumpStaging.get(), 0, 0, 0, 0, atlas.texture11.get(), 0, &box);
		dumpPending = true;
	}

	void AlphaAtlas::ReadDumpImage(std::vector<DumpImage>& a_out)
	{
		if (!dumpPending)
			return;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = d3d11Context->Map(dumpStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			logger::warn("[SkyrimRT] Debug dump: alpha atlas copy not finished; image skipped");
			return;
		}
		dumpPending = false;
		if (FAILED(hr))
			return;

		// Half size (2x2 average), grey, so the PNG stays small and readable.
		constexpr uint32_t kWidth = kAtlasSize / 2;
		constexpr uint32_t kHeight = kDumpRows * kTileSize / 2;
		DumpImage image{ "alpha_atlas", kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<uint8_t>(static_cast<size_t>(kWidth) * kHeight * 4) };
		const auto* source = static_cast<const uint8_t*>(mapped.pData);
		for (uint32_t y = 0; y < kHeight; y++) {
			const uint8_t* row0 = source + static_cast<size_t>(y * 2) * mapped.RowPitch;
			const uint8_t* row1 = row0 + mapped.RowPitch;
			for (uint32_t x = 0; x < kWidth; x++) {
				const uint32_t sum = row0[x * 2] + row0[x * 2 + 1] + row1[x * 2] + row1[x * 2 + 1];
				const auto value = static_cast<uint8_t>((sum + 2) / 4);
				uint8_t* pixel = image.pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
				pixel[0] = pixel[1] = pixel[2] = value;
				pixel[3] = 255;
			}
		}
		d3d11Context->Unmap(dumpStaging.get(), 0);
		a_out.push_back(std::move(image));
	}
}
