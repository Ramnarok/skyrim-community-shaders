#include "AlbedoAtlas.h"

#include "RT.h"

namespace RT
{
	void WriteAlbedoAtlasDescriptor(ID3D12Device* a_device, ID3D12Resource* a_atlas, D3D12_CPU_DESCRIPTOR_HANDLE a_handle)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{ .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
		desc.Texture2D.MipLevels = 1;
		a_device->CreateShaderResourceView(a_atlas, &desc, a_handle);
	}

	bool AlbedoAtlas::Init(ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, ID3D12Device* a_device)
	{
		const TextureAtlasConfig config{ .name = "AlbedoAtlas", .tileSize = kTileSize, .tilesPerRow = kTilesPerRow, .format = DXGI_FORMAT_R8G8B8A8_UNORM, .albedo = true };
		stats.available = atlas.Init(a_d3d11Device, a_d3d11Context, a_device, config);
		stats.capacity = atlas.GetStats().capacity;
		return stats.available;
	}

	void AlbedoAtlas::Update(std::vector<GeometryCandidate>& a_candidates, uint32_t a_frame, bool a_enabled)
	{
		stats.enabled = a_enabled;
		stats.filledLastFrame = 0;
		stats.evictedLastFrame = 0;
		stats.candidates = 0;
		stats.candidatesTextured = 0;
		stats.candidatesVertexColors = 0;
		stats.candidatesNoTexture = 0;
		stats.candidatesNoUV = 0;
		stats.candidatesUnsupported = 0;
		stats.candidatesWaiting = 0;
		if (!a_enabled || !stats.available)
			return;  // albedoWord stays 0: the average albedo

		atlas.BeginFrame();
		ID3D11ShaderResourceView* lastSRV = nullptr;
		TextureAtlas::Result lastResult{};
		uint32_t lastTile = 0;
		for (auto& candidate : a_candidates) {
			if (candidate.alphaBlended || candidate.decal)
				continue;  // in no trace
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

			// Consecutive candidates with one texture (M8 tree LOD) reuse the last tile lookup.
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
			// Bits 0-11 tile + 1, 12-19 UV byte offset, 20-23 vertex-colour offset / 4 (0 = none: offset 0 is the position).
			const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			const uint32_t colorWords = desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR) / 4 : 0;
			candidate.albedoWord = (tile + 1) | (std::min(uvOffset, 0xFFu) << 12) | (std::min(colorWords, 0xFu) << 20);
			stats.candidatesTextured++;
			stats.candidatesVertexColors += colorWords > 0;
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
