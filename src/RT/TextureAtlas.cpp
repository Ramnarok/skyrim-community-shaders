#include "TextureAtlas.h"

#include "RT.h"

namespace RT
{
	bool TextureAtlas::Init(ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, ID3D12Device* a_device, const TextureAtlasConfig& a_config)
	{
		config = a_config;
		d3d11Device = a_d3d11Device;
		d3d11Context = a_d3d11Context;
		const uint32_t capacity = config.tilesPerRow * config.tilesPerRow;
		const uint32_t size = config.tileSize * config.tilesPerRow;
		stats.capacity = capacity;

		std::string error;
		if (!CreateSharedTexture(d3d11Device, a_device, size, size, config.format, config.name, atlas, error)) {
			logger::error("[SkyrimRT] {}: {}", config.name, error);
			atlas = {};
			return false;
		}

		std::vector<std::pair<const char*, const char*>> defines;
		if (config.albedo)
			defines.push_back({ "ALBEDO", nullptr });
		fillCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SkyrimRT\\AtlasFillCS.hlsl", defines, "cs_5_0")));
		if (!fillCS) {
			logger::error("[SkyrimRT] {}: compiling Data\\Shaders\\SkyrimRT\\AtlasFillCS.hlsl failed", config.name);
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
			logger::error("[SkyrimRT] {}: creating the sampler or constant buffer failed", config.name);
			return false;
		}
		Util::SetResourceName(params.get(), std::format("SkyrimRT::{}Params", config.name).c_str());

		tileOwners.assign(capacity, nullptr);
		freeTiles.reserve(capacity);
		for (uint32_t tile = capacity; tile-- > 0;)
			freeTiles.push_back(tile);  // popped from the back: tile 0 first

		stats.available = true;
		logger::info("[SkyrimRT] {} ready: {}x{}, {} tiles of {}x{}", config.name, size, size, capacity, config.tileSize, config.tileSize);
		return true;
	}

	void TextureAtlas::BeginFrame()
	{
		stats.filledLastFrame = 0;
		stats.evictedLastFrame = 0;
	}

	int32_t TextureAtlas::AllocateTile(uint32_t a_frame)
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
		for (uint32_t tile = 0; tile < stats.capacity; tile++) {
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

	void TextureAtlas::Fill(ID3D11ShaderResourceView* a_srv, uint32_t a_tile)
	{
		auto* ctx = d3d11Context;
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
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(ctx->Map(params.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		const uint32_t data[4] = { (a_tile % config.tilesPerRow) * config.tileSize, (a_tile / config.tilesPerRow) * config.tileSize, config.tileSize, 0 };
		std::memcpy(mapped.pData, data, sizeof(data));
		ctx->Unmap(params.get(), 0);
		ctx->CSSetShaderResources(0, 1, &a_srv);
		ctx->Dispatch(config.tileSize / 8, config.tileSize / 8, 1);
		stats.filledLastFrame++;
		stats.totalFills++;
	}

	TextureAtlas::Result TextureAtlas::Acquire(ID3D11ShaderResourceView* a_srv, uint32_t a_frame, uint32_t& a_tile)
	{
		auto& entry = entries[a_srv];
		if (entry.validatedFrame != a_frame) {
			entry.validatedFrame = a_frame;
			const TextureIdentity identity = GetTextureIdentity(a_srv);
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
		if (!entry.identity.supported)
			return Result::kUnsupported;

		if (entry.tile < 0) {
			const int32_t tile = stats.filledLastFrame < config.maxFillsPerFrame ? AllocateTile(a_frame) : -1;
			if (tile < 0)
				return Result::kWaiting;
			Fill(a_srv, static_cast<uint32_t>(tile));
			entry.tile = tile;
			tileOwners[tile] = a_srv;
		}
		a_tile = static_cast<uint32_t>(entry.tile);
		return Result::kTile;
	}

	void TextureAtlas::EndFrame()
	{
		if (bound) {
			auto* ctx = d3d11Context;
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			ID3D11SamplerState* nullSampler = nullptr;
			ID3D11Buffer* nullCB = nullptr;
			ctx->CSSetShaderResources(0, 1, &nullSRV);
			ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			ctx->CSSetSamplers(0, 1, &nullSampler);
			ctx->CSSetConstantBuffers(0, 1, &nullCB);
			ctx->CSSetShader(nullptr, nullptr, 0);
			bound = false;
		}
		stats.tilesUsed = stats.capacity - static_cast<uint32_t>(freeTiles.size());
	}

	void TextureAtlas::CaptureForDump()
	{
		if (!stats.available)
			return;
		const uint32_t size = config.tileSize * config.tilesPerRow;
		const uint32_t rows = std::min(config.dumpRows, config.tilesPerRow) * config.tileSize;
		if (!dumpStaging) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = size;
			desc.Height = rows;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = config.format;
			desc.SampleDesc = { 1, 0 };
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(d3d11Device->CreateTexture2D(&desc, nullptr, dumpStaging.put()))) {
				logger::error("[SkyrimRT] Debug dump: cannot create the {} staging texture", config.name);
				return;
			}
			Util::SetResourceName(dumpStaging.get(), std::format("SkyrimRT::{}Dump", config.name).c_str());
		}
		const D3D11_BOX box{ 0, 0, 0, size, rows, 1 };
		d3d11Context->CopySubresourceRegion(dumpStaging.get(), 0, 0, 0, 0, atlas.texture11.get(), 0, &box);
		dumpPending = true;
	}

	void TextureAtlas::ReadDumpImage(const char* a_name, std::vector<DumpImage>& a_out)
	{
		if (!dumpPending)
			return;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = d3d11Context->Map(dumpStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			logger::warn("[SkyrimRT] Debug dump: {} copy not finished; image skipped", config.name);
			return;
		}
		dumpPending = false;
		if (FAILED(hr))
			return;

		// Half size (2x2 average), so the PNG stays small and readable. Alpha atlases are shown grey.
		const uint32_t width = config.tileSize * config.tilesPerRow / 2;
		const uint32_t height = std::min(config.dumpRows, config.tilesPerRow) * config.tileSize / 2;
		const uint32_t texelBytes = config.format == DXGI_FORMAT_R8_UNORM ? 1 : 4;
		DumpImage image{ a_name, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4) };
		const auto* source = static_cast<const uint8_t*>(mapped.pData);
		for (uint32_t y = 0; y < height; y++) {
			const uint8_t* row0 = source + static_cast<size_t>(y * 2) * mapped.RowPitch;
			const uint8_t* row1 = row0 + mapped.RowPitch;
			for (uint32_t x = 0; x < width; x++) {
				uint8_t* pixel = image.pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
				for (uint32_t c = 0; c < 3; c++) {
					const uint32_t channel = texelBytes == 1 ? 0 : c;
					const uint32_t a = (x * 2) * texelBytes + channel, b = (x * 2 + 1) * texelBytes + channel;
					pixel[c] = static_cast<uint8_t>((row0[a] + row0[b] + row1[a] + row1[b] + 2) / 4);
				}
				pixel[3] = 255;
			}
		}
		d3d11Context->Unmap(dumpStaging.get(), 0);
		a_out.push_back(std::move(image));
	}
}
