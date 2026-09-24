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
		const TextureAtlasConfig config{ .name = "AlphaAtlas", .tileSize = kTileSize, .tilesPerRow = kTilesPerRow, .format = DXGI_FORMAT_R8_UNORM, .albedo = false };
		stats.available = atlas.Init(a_d3d11Device, a_d3d11Context, a_device, config);
		stats.capacity = atlas.GetStats().capacity;
		return stats.available;
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

		atlas.BeginFrame();
		ID3D11ShaderResourceView* lastSRV = nullptr;
		TextureAtlas::Result lastResult{};
		uint32_t lastTile = 0;
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

			// Consecutive candidates with one texture (M8 tree LOD: thousands sharing the billboard atlas) reuse the last
			// tile lookup.
			uint32_t tile = 0;
			TextureAtlas::Result result;
			if (candidate.diffuseSRV == lastSRV) {
				result = lastResult;
				tile = lastTile;
			} else {
				result = atlas.Acquire(candidate.diffuseSRV, a_frame, tile);
				lastSRV = candidate.diffuseSRV;
				lastResult = result;
				lastTile = tile;
			}
			if (result == TextureAtlas::Result::kUnsupported) {
				stats.candidatesUnsupported++;
				continue;
			}
			if (result == TextureAtlas::Result::kWaiting) {
				stats.candidatesWaiting++;
				continue;
			}
			const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			candidate.alphaWord = (tile + 1) | (static_cast<uint32_t>(candidate.alphaThreshold) << 12) | (uvOffset << 20);
			stats.candidatesTested++;
		}
		atlas.EndFrame();

		const auto& atlasStats = atlas.GetStats();
		stats.tilesUsed = atlasStats.tilesUsed;
		stats.filledLastFrame = atlasStats.filledLastFrame;
		stats.evictedLastFrame = atlasStats.evictedLastFrame;
		stats.totalFills = atlasStats.totalFills;
		stats.totalEvictions = atlasStats.totalEvictions;
	}
}
