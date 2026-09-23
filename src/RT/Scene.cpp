#include "Scene.h"

namespace RT
{
	namespace
	{
		using Type = RE::BSGeometry::Type;
		using Feature = RE::BSShaderMaterial::Feature;

		GeometryCategory Classify(RE::BSGeometry* a_geometry, GeometryCandidate& a_candidate)
		{
			switch (a_geometry->GetType().get()) {
			case Type::kParticles:
			case Type::kStripParticles:
			case Type::kParticleShaderDynamicTriShape:
				return GeometryCategory::kParticles;
			case Type::kDynamicTriShape:
				return GeometryCategory::kDynamic;
			case Type::kMultiStreamInstanceTriShape:
			case Type::kInstanceGroup:
				return GeometryCategory::kInstanced;
			case Type::kMeshLODTriShape:
			case Type::kLODMultiIndexTriShape:
				return GeometryCategory::kLOD;
			case Type::kTriShape:
			case Type::kMultiIndexTriShape:
			case Type::kSubIndexTriShape:
			case Type::kSubIndexLandTriShape:
				break;
			default:
				return GeometryCategory::kOther;
			}

			const auto& geometryData = a_geometry->GetGeometryRuntimeData();
			if (geometryData.skinInstance)
				return GeometryCategory::kSkinned;

			auto* shaderProperty = geometryData.shaderProperty.get();
			auto* lightingProperty = netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty);
			if (!lightingProperty)
				return shaderProperty ? GeometryCategory::kEffectOrWater : GeometryCategory::kOther;

			auto* triShape = a_geometry->AsTriShape();
			auto* rendererData = geometryData.rendererData;
			if (!triShape || !rendererData)
				return GeometryCategory::kNoRendererData;

			const auto& counts = triShape->GetTrishapeRuntimeData();
			if (counts.vertexCount == 0 || counts.triangleCount == 0 || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				return GeometryCategory::kNoRendererData;

			std::memcpy(&a_candidate.vertexDesc, &rendererData->vertexDesc, sizeof(a_candidate.vertexDesc));  // VertexDesc keeps its bits private
			if (rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
				return GeometryCategory::kSkinned;

			a_candidate.rendererData = rendererData;
			a_candidate.vertexCount = counts.vertexCount;
			a_candidate.triangleCount = counts.triangleCount;

			// Same test CS uses for landscape (TruePBR.cpp): the lighting material's feature.
			if (auto* material = lightingProperty->material) {
				const auto feature = material->GetFeature();
				a_candidate.terrain = feature == Feature::kMultiTexLand || feature == Feature::kMultiTexLandLODBlend;
			}
			if (auto* alpha = geometryData.alphaProperty.get())
				a_candidate.alphaTested = alpha->GetAlphaTesting();

			return a_candidate.terrain ? GeometryCategory::kTerrain : GeometryCategory::kStaticMesh;
		}

		void Walk(RE::NiAVObject* a_object, std::vector<GeometryCandidate>& a_out, SceneStats& a_stats)
		{
			if (!a_object)
				return;
			// A hidden node hides its whole subtree (disabled references, etc.).
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				a_stats.hiddenSubtrees++;
				return;
			}

			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren())
					Walk(child.get(), a_out, a_stats);
				return;
			}

			if (auto* geometry = a_object->AsGeometry()) {
				GeometryCandidate candidate;
				const auto category = Classify(geometry, candidate);
				a_stats.instances[static_cast<size_t>(category)]++;
				if (category == GeometryCategory::kStaticMesh || category == GeometryCategory::kTerrain) {
					a_stats.alphaTestedInstances += candidate.alphaTested;
					a_out.push_back(candidate);
				}
			}
		}
	}

	std::string_view GetCategoryName(GeometryCategory a_category)
	{
		switch (a_category) {
		case GeometryCategory::kStaticMesh:
			return "static_mesh"sv;
		case GeometryCategory::kTerrain:
			return "terrain"sv;
		case GeometryCategory::kSkinned:
			return "skinned"sv;
		case GeometryCategory::kDynamic:
			return "dynamic"sv;
		case GeometryCategory::kInstanced:
			return "instanced"sv;
		case GeometryCategory::kParticles:
			return "particles"sv;
		case GeometryCategory::kEffectOrWater:
			return "effect_water_sky_grass"sv;
		case GeometryCategory::kLOD:
			return "lod"sv;
		case GeometryCategory::kNoRendererData:
			return "no_renderer_data"sv;
		default:
			return "other"sv;
		}
	}

	bool CollectScene(std::vector<GeometryCandidate>& a_out, SceneStats& a_stats)
	{
		a_out.clear();
		a_stats = {};

		auto* tes = RE::TES::GetSingleton();
		if (!tes || !RE::PlayerCharacter::GetSingleton() || !RE::PlayerCharacter::GetSingleton()->Is3DLoaded())
			return false;

		LARGE_INTEGER start, end, frequency;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);

		tes->ForEachCell([&](RE::TESObjectCELL* a_cell) {
			auto* loadedData = a_cell ? a_cell->GetRuntimeData().loadedData : nullptr;
			if (!loadedData || !loadedData->cell3D)
				return;
			a_stats.cells++;
			Walk(loadedData->cell3D.get(), a_out, a_stats);
		});

		// Unique meshes: instances of the same mesh share rendererData.
		ankerl::unordered_dense::set<const void*> staticMeshes;
		ankerl::unordered_dense::set<const void*> terrainMeshes;
		for (const auto& candidate : a_out)
			(candidate.terrain ? terrainMeshes : staticMeshes).insert(candidate.rendererData);
		a_stats.uniqueStaticMeshes = static_cast<uint32_t>(staticMeshes.size());
		a_stats.uniqueTerrainMeshes = static_cast<uint32_t>(terrainMeshes.size());

		QueryPerformanceCounter(&end);
		a_stats.traversalMs = static_cast<float>(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
		return true;
	}
}
