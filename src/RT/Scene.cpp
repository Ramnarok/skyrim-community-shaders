#include "Scene.h"

#include "Features/GrassOptimizations.h"

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

				// M6 material table: the diffuse texture the lighting shader samples. Terrain blends up to five layers;
				// the first (base) layer stands in for all of them. The landscape check is an RTTI cast because True PBR's
				// PBR landscape also reports kMultiTexLandLODBlend with another layout; every lighting material (True
				// PBR's included) derives from BSLightingShaderMaterialBase, so its diffuse slot is always safe to read.
				RE::NiSourceTexture* diffuse = nullptr;
				if (auto* landscape = skyrim_cast<RE::BSLightingShaderMaterialLandscape*>(material)) {
					if (landscape->numLandscapeTextures > 0)
						diffuse = landscape->landscapeDiffuseTexture[0].get();
				} else {
					diffuse = static_cast<RE::BSLightingShaderMaterialBase*>(material)->diffuseTexture.get();
				}
				if (diffuse && diffuse->rendererTexture)
					a_candidate.diffuseSRV = diffuse->rendererTexture->resourceView;
			}
			if (auto* alpha = geometryData.alphaProperty.get()) {
				a_candidate.alphaTested = alpha->GetAlphaTesting();
				a_candidate.alphaBlended = alpha->GetAlphaBlending();
			}

			return a_candidate.terrain ? GeometryCategory::kTerrain : GeometryCategory::kStaticMesh;
		}

		struct WalkOutput
		{
			std::vector<GeometryCandidate>& candidates;
			std::vector<ExclusionBound>& exclusions;
			SceneStats& stats;
		};

		void AddExclusion(RE::NiAVObject* a_object, GeometryCategory a_category, WalkOutput& a_out)
		{
			// Geometry the TLAS doesn't contain but which writes depth: the M4 depth metric must not count
			// mismatches it causes. Oversized or empty bounds are ignored as bogus.
			const auto& bound = a_object->worldBound;
			if (bound.radius <= 0.0f)
				return;
			if (bound.radius >= 20000.0f) {
				a_out.stats.exclusionsRejectedTooLarge++;
				return;
			}
			const auto index = static_cast<size_t>(a_category);
			a_out.stats.exclusionsByCategory[index]++;
			a_out.stats.exclusionMaxRadius[index] = std::max(a_out.stats.exclusionMaxRadius[index], bound.radius);
			a_out.exclusions.push_back({ bound.center, bound.radius });
		}

		void Walk(RE::NiAVObject* a_object, WalkOutput& a_out)
		{
			if (!a_object)
				return;
			// A hidden node hides its whole subtree (disabled references, etc.).
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				a_out.stats.hiddenSubtrees++;
				return;
			}

			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren())
					Walk(child.get(), a_out);
				return;
			}

			if (auto* geometry = a_object->AsGeometry()) {
				GeometryCandidate candidate;
				const auto category = Classify(geometry, candidate);
				a_out.stats.instances[static_cast<size_t>(category)]++;
				switch (category) {
				case GeometryCategory::kStaticMesh:
				case GeometryCategory::kTerrain:
					// Alpha-tested meshes are in the TLAS (opaque until M7); the trace identifies them by the hit
					// instance, so they don't add exclusion bounds (their bounds often cover whole rooms).
					candidate.world = geometry->world;
					a_out.stats.alphaTestedInstances += candidate.alphaTested;
					a_out.stats.alphaBlendedInstances += candidate.alphaBlended;
					a_out.candidates.push_back(candidate);
					break;
				case GeometryCategory::kSkinned:
				case GeometryCategory::kDynamic:
				case GeometryCategory::kInstanced:
				case GeometryCategory::kLOD:
				case GeometryCategory::kOther:
					AddExclusion(geometry, category, a_out);
					break;
				default:
					// Particles, effects, water and sky don't write the pre-water depth we compare against.
					break;
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

	namespace
	{
		bool CollectSceneUnguarded(std::vector<GeometryCandidate>& a_out, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats);

		// Last line of defence: reading the game's scene graph can still race with cell loading in ways the
		// loading-menu check doesn't cover. An access violation drops this frame's scene instead of the game.
		// Kept free of C++ objects so __try is allowed.
		bool CollectSceneGuarded(std::vector<GeometryCandidate>& a_out, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, bool& a_faulted)
		{
			__try {
				return CollectSceneUnguarded(a_out, a_exclusions, a_area, a_stats);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_faulted = true;
				return false;
			}
		}
	}

	bool CollectScene(std::vector<GeometryCandidate>& a_out, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats)
	{
		a_out.clear();
		a_exclusions.clear();
		a_area = {};
		a_stats = {};

		// Loading screens (coc, doors, fast travel) keep presenting while cells are torn down and rebuilt,
		// so the scene graph must not be walked then (M4 crash: garbage child pointer after coc).
		auto* ui = RE::UI::GetSingleton();
		if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
			return false;

		bool faulted = false;
		const bool collected = CollectSceneGuarded(a_out, a_exclusions, a_area, a_stats, faulted);
		if (faulted) {
			static uint32_t faults = 0;
			if (++faults <= 10)
				logger::warn("[SkyrimRT] Scene walk hit an access violation (#{}); frame skipped", faults);
			a_out.clear();
			a_exclusions.clear();
			a_area = {};
			a_stats = {};
			return false;
		}
		return collected;
	}

	namespace
	{
		bool CollectSceneUnguarded(std::vector<GeometryCandidate>& a_out, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats)
		{
		auto* tes = RE::TES::GetSingleton();
		if (!tes || !RE::PlayerCharacter::GetSingleton() || !RE::PlayerCharacter::GetSingleton()->Is3DLoaded())
			return false;

		LARGE_INTEGER start, end, frequency;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);

		// Same lookup TES::ForEachCell uses for the worldspace's persistent "sky" cell, which is not a grid cell
		// and must not widen the loaded area.
		auto* worldSpace = tes->GetRuntimeData2().worldSpace;
		const RE::TESObjectCELL* skyCell = worldSpace ? worldSpace->GetSkyCell() : nullptr;
		const bool interior = tes->interiorCell != nullptr;

		WalkOutput out{ a_out, a_exclusions, a_stats };
		tes->ForEachCell([&](RE::TESObjectCELL* a_cell) {
			auto* loadedData = a_cell ? a_cell->GetRuntimeData().loadedData : nullptr;
			if (!loadedData || !loadedData->cell3D)
				return;
			a_stats.cells++;
			auto* cell3D = loadedData->cell3D.get();
			if (!interior && a_cell != skyCell && a_cell->IsExteriorCell()) {
				if (const auto* coordinates = a_cell->GetCoordinates()) {
					// Exterior cells are 4096 units square; the area is XY only (Z unused).
					constexpr float kCellSize = 4096.0f;
					const RE::NiPoint3 lo(coordinates->cellX * kCellSize, coordinates->cellY * kCellSize, 0.0f);
					const RE::NiPoint3 hi(lo.x + kCellSize, lo.y + kCellSize, 0.0f);
					if (!a_area.bounded) {
						a_area = { true, lo, hi };
					} else {
						a_area.min = { std::min(a_area.min.x, lo.x), std::min(a_area.min.y, lo.y), 0.0f };
						a_area.max = { std::max(a_area.max.x, hi.x), std::max(a_area.max.y, hi.y), 0.0f };
					}
				}
			}
			Walk(cell3D, out);
		});

		// Grass lives under BGSGrassManager::grassNode, outside the cells. The manager pointer comes from CS's
		// GrassOptimizations LoadGrassType hook (the game passes it in), not from a singleton ID, so no Address
		// Library ID is needed. Grass patches classify as kInstanced and become occluder bounds.
		if (!interior && globals::features::grassOptimizations.loaded) {
			if (auto* grassManager = GrassOptimizations::Hooks::LoadGrassType::lastGrassManager.load(std::memory_order_relaxed)) {
				if (auto* grassNode = grassManager->grassNode.get()) {
					const size_t before = a_exclusions.size();
					Walk(grassNode, out);
					a_stats.grassWalked = true;
					a_stats.grassBounds = static_cast<uint32_t>(a_exclusions.size() - before);
				}
			}
		}
		a_stats.exclusionBounds = static_cast<uint32_t>(a_exclusions.size());

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
}
