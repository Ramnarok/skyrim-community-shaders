#include "Scene.h"

#include "Features/GrassOptimizations.h"

namespace RT
{
	namespace
	{
		void TransformTo3x4(const RE::NiTransform& a_transform, float* a_out)
		{
			const float translate[3] = { a_transform.translate.x, a_transform.translate.y, a_transform.translate.z };
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++)
					a_out[r * 4 + c] = a_transform.rotate.entry[r][c] * a_transform.scale;
				a_out[r * 4 + 3] = translate[r];
			}
		}

		// A bone palette row (3x4, absolute world space) that skinning can use: finite, a rotation-and-scale part no
		// larger than a 1000x scale, and a translation within 10^7 units (Skyrim's worldspaces span ~10^6).
		bool IsSanePaletteRow(const float (&a_row)[12])
		{
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 4; c++) {
					const float v = a_row[r * 4 + c];
					if (!std::isfinite(v) || std::abs(v) > (c == 3 ? 1.0e7f : 1.0e3f))
						return false;
				}
			}
			return true;
		}

		bool IsUnderTree(const RE::NiAVObject* a_object)
		{
			for (auto* node = a_object ? a_object->parent : nullptr; node; node = node->parent) {
				if (netimmerse_cast<RE::BSTreeNode*>(node))
					return true;
			}
			return false;
		}

		using Type = RE::BSGeometry::Type;
		using Feature = RE::BSShaderMaterial::Feature;

		void FillMaterial(RE::BSLightingShaderProperty* a_property, const RE::BSGeometry::GEOMETRY_RUNTIME_DATA& a_geometryData, GeometryCandidate& a_candidate);

		GeometryCategory Classify(RE::BSGeometry* a_geometry, GeometryCandidate& a_candidate)
		{
			switch (a_geometry->GetType().get()) {
			case Type::kParticles:
			case Type::kStripParticles:
			case Type::kParticleShaderDynamicTriShape:
				return GeometryCategory::kParticles;
			case Type::kDynamicTriShape:
				break;  // M7b: FaceGen heads, extracted as skinned with CPU-side positions
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
			auto* shaderProperty = geometryData.shaderProperty.get();
			auto* lightingProperty = netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty);
			const bool dynamic = a_geometry->GetType().get() == Type::kDynamicTriShape;
			if (!lightingProperty)
				return dynamic ? GeometryCategory::kDynamic : geometryData.skinInstance ? GeometryCategory::kSkinned : shaderProperty ? GeometryCategory::kEffectOrWater : GeometryCategory::kOther;

			// Material first: skinned shapes need it too (M7).
			FillMaterial(lightingProperty, geometryData, a_candidate);
			if (dynamic)
				return GeometryCategory::kDynamic;
			if (geometryData.skinInstance)
				return GeometryCategory::kSkinned;

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
			return a_candidate.terrain ? GeometryCategory::kTerrain : GeometryCategory::kStaticMesh;
		}

		void FillMaterial(RE::BSLightingShaderProperty* a_property, const RE::BSGeometry::GEOMETRY_RUNTIME_DATA& a_geometryData, GeometryCandidate& a_candidate)
		{
			auto* lightingProperty = a_property;
			const auto& geometryData = a_geometryData;
			a_candidate.windAnimated = lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim);
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
				a_candidate.alphaThreshold = alpha->alphaThreshold;
			}
		}

		struct WalkOutput
		{
			std::vector<GeometryCandidate>& candidates;
			SkinnedScene& skinned;
			std::vector<ExclusionBound>& exclusions;
			SceneStats& stats;
			bool treeRestPose = true;  // M7: trees in their rest pose (see CollectSkinned)
		};

		/**
		 * @brief M7: one candidate per skin partition (bind-pose buffers + this shape's material) and its bone palette,
		 * boneWorld * skinToBone for each palette bone, computed now while the game's pointers are valid.
		 * @return False if nothing usable was found; the caller then keeps an exclusion bound as before.
		 */
		bool CollectSkinned(RE::BSGeometry* a_geometry, const GeometryCandidate& a_material, WalkOutput& a_out)
		{
			auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
			auto* skinData = skin ? skin->skinData.get() : nullptr;
			auto* partitions = skin ? skin->skinPartition.get() : nullptr;
			if (!skinData || !partitions || !skin->boneWorldTransforms || partitions->numPartitions == 0)
				return false;

			// M7b: a dynamic shape's positions come from its CPU-side array, which the game rewrites for face morphs.
			const uint8_t* dynamicData = nullptr;
			uint32_t dynamicStride = 0;
			uint32_t dynamicVertexCount = 0;
			uint32_t dynamicVersion = 0;
			if (auto* dynamicShape = a_geometry->AsDynamicTriShape()) {
				const auto& dynamicRuntime = dynamicShape->GetDynamicTrishapeRuntimeData();
				dynamicVertexCount = dynamicShape->GetTrishapeRuntimeData().vertexCount;
				dynamicStride = dynamicVertexCount ? dynamicRuntime.dataSize / dynamicVertexCount : 0;
				dynamicData = static_cast<const uint8_t*>(dynamicRuntime.dynamicData);
				dynamicVersion = dynamicRuntime.frameCount;
				if (!dynamicData || dynamicStride < 12 || (dynamicStride % 4) != 0)
					return false;
			}
			const uint32_t skinBones = skinData->GetBoneCount();

			// Trees are skinned: their branches sway on bones. Every camera that culls a tree (the view, the shadow cascades,
			// Skylighting's occlusion pass) re-poses those bones (BSLeafAnimNode::OnVisible), so the pose left at this walk
			// belongs to whichever camera culled last, not necessarily the view (measured: trunks a full width off). A tree
			// is traced in its rest pose instead, every bone at rootParent * inverse(rootParentToSkin); the root doesn't sway.
			const bool tree = IsUnderTree(a_geometry);
			float restRow[12];
			const bool restPose = tree && a_out.treeRestPose && skin->rootParent;
			if (restPose)
				TransformTo3x4(skin->rootParent->world * skinData->rootParentToSkin.Invert(), restRow);

			uint32_t accepted = 0;
			for (uint32_t p = 0; p < partitions->numPartitions; p++) {
				const auto& partition = partitions->partitions[p];
				auto* buffers = partition.buffData;
				bool usable = buffers && buffers->vertexBuffer && buffers->indexBuffer && partition.triangles > 0 && partition.numBones > 0 && partition.bones &&
				              buffers->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
				uint32_t stride = 0;
				uint32_t vertexCount = 0;
				if (usable) {
					stride = static_cast<uint32_t>(*reinterpret_cast<const uint64_t*>(&buffers->vertexDesc) & 0xF) * 4;
					// The game's buffer size is authoritative for the vertex count (partitions may share one buffer).
					D3D11_BUFFER_DESC desc{};
					reinterpret_cast<::ID3D11Buffer*>(buffers->vertexBuffer)->GetDesc(&desc);
					vertexCount = stride ? desc.ByteWidth / stride : 0;
					usable = vertexCount > 0 && vertexCount <= 65536 && (!dynamicData || vertexCount <= dynamicVertexCount);
				}
				const uint32_t paletteOffset = static_cast<uint32_t>(a_out.skinned.palettes.size());
				for (uint32_t k = 0; usable && k < partition.numBones; k++) {
					const uint32_t bone = partition.bones[k];
					const RE::NiTransform* boneWorld = bone < skinBones ? skin->boneWorldTransforms[bone] : nullptr;
					if (!boneWorld) {
						usable = false;
						break;
					}
					float row[12];
					if (restPose)
						std::memcpy(row, restRow, sizeof(row));
					else
						TransformTo3x4(*boneWorld * skinData->GetBoneDataSkinToBone(bone), row);
					// A non-finite or absurd bone (seen on dying actors' skeletons is the suspicion) would make the refit
					// invalid (SkinCS.hlsl): skip the partition this frame, keeping its last good BLAS.
					if (!IsSanePaletteRow(row)) {
						usable = false;
						a_out.stats.skinnedInvalidPoses++;
						break;
					}
					a_out.skinned.palettes.insert(a_out.skinned.palettes.end(), row, row + 12);
				}
				if (!usable) {
					a_out.skinned.palettes.resize(paletteOffset);
					a_out.stats.skinnedRejectedPartitions++;
					continue;
				}

				GeometryCandidate candidate = a_material;
				candidate.rendererData = buffers;
				std::memcpy(&candidate.vertexDesc, &buffers->vertexDesc, sizeof(candidate.vertexDesc));
				candidate.vertexCount = vertexCount;
				candidate.triangleCount = partition.triangles;
				candidate.skinned = true;
				candidate.world = a_geometry->world;

				const auto& desc = buffers->vertexDesc;
				SkinnedPartition entry{};
				entry.skinInstance = skin;
				entry.partitionIndex = p;
				entry.candidateIndex = static_cast<uint32_t>(a_out.candidates.size());
				entry.paletteOffset = paletteOffset;
				entry.boneCount = partition.numBones;
				entry.skinningOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
				// Positions are float3 + pad (16 B) unless the next attribute starts at 8 (4 x half).
				entry.dynamicData = dynamicData;
				entry.dynamicStride = dynamicStride;
				entry.dynamicVertexCount = dynamicVertexCount;
				entry.dynamicVersion = dynamicVersion;
				entry.halfPositions = desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0) == 8 :
				                                                                      entry.skinningOffset == 8;
				a_out.candidates.push_back(candidate);
				entry.tree = tree;
				a_out.skinned.partitions.push_back(entry);
				a_out.stats.skinnedPartitions++;
				a_out.stats.skinnedBones += partition.numBones;
				a_out.stats.skinnedHalfPositions += entry.halfPositions;
				accepted++;
			}
			if (accepted > 0) {
				a_out.stats.skinnedShapes++;
				a_out.stats.treeShapes += tree;
				a_out.stats.treeRestPoseShapes += restPose;
			}
			return accepted > 0;
		}

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
					// Alpha-tested meshes are in the TLAS (alpha-tested against the M7c atlas once it has their
					// texture); the trace identifies them by the hit instance, so they don't add exclusion bounds
					// (their bounds often cover whole rooms).
					candidate.world = geometry->world;
					a_out.stats.alphaTestedInstances += candidate.alphaTested;
					a_out.stats.alphaBlendedInstances += candidate.alphaBlended;
					a_out.candidates.push_back(candidate);
					break;
				case GeometryCategory::kSkinned:
					if (!CollectSkinned(geometry, candidate, a_out)) {
						a_out.stats.skinnedRejectedShapes++;
						AddExclusion(geometry, category, a_out);
					}
					break;
				case GeometryCategory::kDynamic:
					if (CollectSkinned(geometry, candidate, a_out)) {
						a_out.stats.dynamicShapes++;
					} else {
						a_out.stats.dynamicRejectedShapes++;
						AddExclusion(geometry, category, a_out);
					}
					break;
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
		bool CollectSceneUnguarded(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, bool a_treeRestPose);

		// Last line of defence: reading the game's scene graph can still race with cell loading in ways the
		// loading-menu check doesn't cover. An access violation drops this frame's scene instead of the game.
		// Kept free of C++ objects so __try is allowed.
		bool CollectSceneGuarded(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, bool a_treeRestPose, bool& a_faulted)
		{
			__try {
				return CollectSceneUnguarded(a_out, a_skinned, a_exclusions, a_area, a_stats, a_treeRestPose);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_faulted = true;
				return false;
			}
		}
	}

	bool CollectScene(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, bool a_treeRestPose)
	{
		a_out.clear();
		a_skinned.partitions.clear();
		a_skinned.palettes.clear();
		a_exclusions.clear();
		a_area = {};
		a_stats = {};

		// Loading screens (coc, doors, fast travel) keep presenting while cells are torn down and rebuilt,
		// so the scene graph must not be walked then (M4 crash: garbage child pointer after coc).
		auto* ui = RE::UI::GetSingleton();
		if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
			return false;

		bool faulted = false;
		const bool collected = CollectSceneGuarded(a_out, a_skinned, a_exclusions, a_area, a_stats, a_treeRestPose, faulted);
		if (faulted) {
			static uint32_t faults = 0;
			if (++faults <= 10)
				logger::warn("[SkyrimRT] Scene walk hit an access violation (#{}); frame skipped", faults);
			a_out.clear();
			a_skinned.partitions.clear();
			a_skinned.palettes.clear();
			a_exclusions.clear();
			a_area = {};
			a_stats = {};
			return false;
		}
		return collected;
	}

	namespace
	{
		bool CollectSceneUnguarded(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, bool a_treeRestPose)
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

		WalkOutput out{ a_out, a_skinned, a_exclusions, a_stats, a_treeRestPose };
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
			if (!candidate.skinned)
				(candidate.terrain ? terrainMeshes : staticMeshes).insert(candidate.rendererData);
		a_stats.uniqueStaticMeshes = static_cast<uint32_t>(staticMeshes.size());
		a_stats.uniqueTerrainMeshes = static_cast<uint32_t>(terrainMeshes.size());

		QueryPerformanceCounter(&end);
		a_stats.traversalMs = static_cast<float>(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
		return true;
		}
	}
}
