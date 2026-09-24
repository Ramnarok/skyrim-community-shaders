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

		// Skinned vertices: positions are float3 + pad (16 B) unless the next attribute starts at 8 (4 x half).
		bool HasHalfPositions(const RE::BSGraphics::VertexDesc& a_desc)
		{
			return a_desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) ? a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0) == 8 :
			                                                         a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING) == 8;
		}

		// M8: a kMeshLOD shape whose name starts with "L<digit>_" is a pure lower-detail copy ("L2_WRMainRoadPlains02Parts:15").
		// Merged shapes whose name starts with the full-detail part ("Bricks:11 - L2_WRMainRoadPlains02Parts:11", a Whiterun
		// curb) carry the flag too but are drawn up close. Empirical (Whiterun road, rock piles, houses, shrubs; 2026-09-24).
		bool IsPureLODShapeName(const char* a_name)
		{
			return a_name && a_name[0] == 'L' && a_name[1] >= '0' && a_name[1] <= '9' && a_name[2] == '_';
		}

		enum class SwayRoot : uint8_t
		{
			kNone,
			kTree,      // BSTreeNode
			kLeafAnim,  // M8: a plain BSLeafAnimNode (BSTreeNode's base): swaying plants, re-posed by the same OnVisible
		};

		SwayRoot FindSwayRoot(const RE::NiAVObject* a_object)
		{
			for (auto* node = a_object ? a_object->parent : nullptr; node; node = node->parent) {
				if (netimmerse_cast<RE::BSTreeNode*>(node))
					return SwayRoot::kTree;
				if (netimmerse_cast<RE::BSLeafAnimNode*>(node))
					return SwayRoot::kLeafAnim;
			}
			return SwayRoot::kNone;
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
			SceneOptions options;
			const RE::NiNode* room = nullptr;  // M8: nearest BSMultiBoundRoom / BSPortalSharedNode above the walk position
			const RE::NiNode* objectRoot = nullptr;  // M8: outermost BSFadeNode above the walk position (the object's root)
		};

		// M8: LightLimitFix.cpp's GetParentRoomNode test (same RTTI): Lighting.hlsl culls portal-strict lights by the room of
		// the node found this way above the drawn geometry.
		bool IsRoomNode(const RE::NiAVObject* a_object)
		{
			static const auto* roomRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
			static const auto* portalRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();
			const auto* rtti = a_object->GetRTTI();
			return rtti == roomRtti || rtti == portalRtti;
		}

		/**
		 * @brief M7: one candidate per skin partition (bind-pose buffers + this shape's material) and its bone palette,
		 * boneWorld * skinToBone for each palette bone, computed now while the game's pointers are valid. M8: a rest-pose
		 * tree's partitions become static candidates instead (TreeMode::kRestStatic), with no palette.
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
			// M8: BSTreeNode derives from BSLeafAnimNode, and BSLeafAnimNode::OnVisible is what re-poses the bones, so plants
			// under a plain BSLeafAnimNode have the trees' problem and get the same treatment.
			const SwayRoot swayRoot = FindSwayRoot(a_geometry);
			const bool tree = swayRoot != SwayRoot::kNone;
			float restRow[12];
			RE::NiTransform restTransform;
			const bool restPose = tree && a_out.options.treeMode != TreeMode::kLiveBones && skin->rootParent;
			if (restPose) {
				restTransform = skin->rootParent->world * skinData->rootParentToSkin.Invert();
				TransformTo3x4(restTransform, restRow);
			}
			// M8: with every bone at the same matrix, linear-blend skinning (SkinCS.hlsl: weights normalized, bone 0 when
			// none) is that one matrix for every vertex. The tree is then a static instance of its bind-pose buffers:
			// one BLAS per mesh built once, no per-frame skinning, refit or palette. SkinCS reads half positions, the
			// static BLAS and hit lookups don't, so those partitions stay skinned.
			const bool staticTree = restPose && a_out.options.treeMode == TreeMode::kRestStatic;

			uint32_t accepted = 0;
			uint32_t acceptedStatic = 0;
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
				if (usable && staticTree) {
					if (!HasHalfPositions(buffers->vertexDesc)) {
						if (!IsSanePaletteRow(restRow)) {
							a_out.stats.skinnedInvalidPoses++;
							a_out.stats.skinnedRejectedPartitions++;
							continue;
						}
						GeometryCandidate candidate = a_material;
						candidate.rendererData = buffers;
						std::memcpy(&candidate.vertexDesc, &buffers->vertexDesc, sizeof(candidate.vertexDesc));
						candidate.vertexCount = vertexCount;
						candidate.triangleCount = partition.triangles;
						candidate.world = restTransform;
						candidate.tree = true;
						a_out.candidates.push_back(candidate);
						a_out.stats.treeStaticPartitions++;
						acceptedStatic++;
						accepted++;
						continue;
					}
					a_out.stats.treeHalfPositionPartitions++;
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
				entry.dynamicData = dynamicData;
				entry.dynamicStride = dynamicStride;
				entry.dynamicVertexCount = dynamicVertexCount;
				entry.dynamicVersion = dynamicVersion;
				entry.halfPositions = HasHalfPositions(desc);
				a_out.candidates.push_back(candidate);
				entry.tree = tree;
				a_out.skinned.partitions.push_back(entry);
				a_out.stats.skinnedPartitions++;
				a_out.stats.skinnedBones += partition.numBones;
				a_out.stats.skinnedHalfPositions += entry.halfPositions;
				accepted++;
			}
			if (accepted > 0) {
				a_out.stats.skinnedShapes += accepted > acceptedStatic;
				a_out.stats.treeShapes += tree;
				a_out.stats.leafAnimShapes += swayRoot == SwayRoot::kLeafAnim;
				a_out.stats.treeRestPoseShapes += restPose;
				a_out.stats.treeStaticShapes += acceptedStatic > 0;
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
				const auto* outerRoom = a_out.room;
				const auto* outerObjectRoot = a_out.objectRoot;
				if (IsRoomNode(node))
					a_out.room = node;
				if (!a_out.objectRoot && node->AsFadeNode())
					a_out.objectRoot = node;
				for (auto& child : node->GetChildren())
					Walk(child.get(), a_out);
				a_out.room = outerRoom;
				a_out.objectRoot = outerObjectRoot;
				return;
			}

			if (auto* geometry = a_object->AsGeometry()) {
				// M8: shapes named L1_/L2_ carry kMeshLOD (lower levels of detail in the object's own NIF). CollectScene
				// drops the ones that are alternates of plain geometry; the diagnostic option drops them all.
				const bool meshLOD = geometry->GetFlags().any(RE::NiAVObject::Flag::kMeshLOD);
				if (meshLOD) {
					a_out.stats.meshLODShapes++;
					if (a_out.options.skipMeshLOD) {
						a_out.stats.meshLODSkipped++;
						return;
					}
				}
				GeometryCandidate candidate;
				const auto category = Classify(geometry, candidate);
				candidate.room = a_out.room;
				candidate.geometry = geometry;
				candidate.objectRoot = a_out.objectRoot;
				candidate.meshLOD = meshLOD && IsPureLODShapeName(geometry->name.c_str());
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

	void DescribeNearby(const std::vector<GeometryCandidate>& a_candidates, const RE::NiPoint3& a_center, float a_radius, size_t a_maxCount,
		std::vector<NearbyObject>& a_out)
	{
		a_out.clear();
		struct Hit
		{
			const GeometryCandidate* candidate;
			float distance;
		};
		std::vector<Hit> hits;
		ankerl::unordered_dense::set<const RE::BSGeometry*> seen;
		for (const auto& candidate : a_candidates) {
			const auto* geometry = candidate.geometry;
			if (!geometry || !seen.insert(geometry).second)
				continue;  // skinned partitions share their shape
			const auto& bound = geometry->worldBound;
			const float distance = std::max(0.0f, (bound.center - a_center).Length() - bound.radius);
			if (distance <= a_radius)
				hits.push_back({ &candidate, distance });
		}
		std::ranges::sort(hits, {}, &Hit::distance);
		if (hits.size() > a_maxCount)
			hits.resize(a_maxCount);

		for (const auto& hit : hits) {
			const auto& candidate = *hit.candidate;
			const auto* geometry = candidate.geometry;
			NearbyObject object;
			object.name = geometry->name.c_str() ? geometry->name.c_str() : "";
			object.flags = geometry->GetFlags().underlying();
			int depth = 0;
			for (const RE::NiNode* node = geometry->parent; node; node = node->parent) {
				object.ancestorFlags |= node->GetFlags().underlying();
				if (auto* fade = netimmerse_cast<const RE::BSFadeNode*>(node))
					object.minFade = std::min(object.minFade, fade->GetRuntimeData().currentFade);
				if (depth++ < 4) {
					if (!object.parents.empty())
						object.parents += " < ";
					object.parents += node->name.c_str() ? node->name.c_str() : "";
				}
			}
			if (const auto* reference = geometry->GetUserData()) {
				object.refFormID = reference->GetFormID();
				if (const auto* base = reference->GetBaseObject()) {
					object.baseFormID = base->GetFormID();
					const char* baseName = base->GetName();
					object.baseName = baseName ? baseName : "";
				}
			}
			const auto& bound = geometry->worldBound;
			object.distance = hit.distance;
			object.offset[0] = bound.center.x - a_center.x;
			object.offset[1] = bound.center.y - a_center.y;
			object.offset[2] = bound.center.z - a_center.z;
			object.boundRadius = bound.radius;
			object.triangles = candidate.triangleCount;
			object.alphaTested = candidate.alphaTested;
			object.alphaBlended = candidate.alphaBlended;
			object.windAnimated = candidate.windAnimated;
			object.skinned = candidate.skinned;
			object.terrain = candidate.terrain;
			object.tree = candidate.tree;
			a_out.push_back(std::move(object));
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
		bool CollectSceneUnguarded(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, const SceneOptions& a_options);

		// Last line of defence: reading the game's scene graph can still race with cell loading in ways the
		// loading-menu check doesn't cover. An access violation drops this frame's scene instead of the game.
		// Kept free of C++ objects so __try is allowed.
		bool CollectSceneGuarded(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, const SceneOptions& a_options, bool& a_faulted)
		{
			__try {
				return CollectSceneUnguarded(a_out, a_skinned, a_exclusions, a_area, a_stats, a_options);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_faulted = true;
				return false;
			}
		}
	}

	bool CollectScene(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, const SceneOptions& a_options)
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
		const bool collected = CollectSceneGuarded(a_out, a_skinned, a_exclusions, a_area, a_stats, a_options, faulted);
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
		bool CollectSceneUnguarded(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats, const SceneOptions& a_options)
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

		WalkOutput out{ a_out, a_skinned, a_exclusions, a_stats, a_options };
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

		// M8: an object whose NIF has other shapes draws those up close; its pure lower-detail copies (kMeshLOD, named
		// L1_/L2_) are alternates the raster skips, e.g. a road's L2_ edge card floating above the cobbles (Whiterun; 84k
		// pixels of RT-only shadow). An object with only such shapes (shrubs, thickets, ferns) draws them: they stay.
		{
			ankerl::unordered_dense::set<const RE::NiNode*> rootsWithPlainShapes;
			for (const auto& candidate : a_out)
				if (!candidate.meshLOD && candidate.objectRoot)
					rootsWithPlainShapes.insert(candidate.objectRoot);
			std::vector<uint32_t> newIndex(a_out.size(), UINT32_MAX);
			size_t kept = 0;
			for (size_t i = 0; i < a_out.size(); i++) {
				const auto& candidate = a_out[i];
				if (candidate.meshLOD && candidate.objectRoot && rootsWithPlainShapes.contains(candidate.objectRoot)) {
					a_stats.meshLODAlternates++;
					continue;
				}
				newIndex[i] = static_cast<uint32_t>(kept);
				if (kept != i)
					a_out[kept] = a_out[i];
				kept++;
			}
			a_out.resize(kept);
			// Skinned partitions point into the candidates; a dropped one (a skinned kMeshLOD alternate) goes too.
			std::erase_if(a_skinned.partitions, [&](SkinnedPartition& a_partition) {
				a_partition.candidateIndex = newIndex[a_partition.candidateIndex];
				return a_partition.candidateIndex == UINT32_MAX;
			});
		}

		// Unique meshes: instances of the same mesh share rendererData.
		ankerl::unordered_dense::set<const void*> staticMeshes;
		ankerl::unordered_dense::set<const void*> terrainMeshes;
		ankerl::unordered_dense::set<const void*> treeMeshes;
		for (const auto& candidate : a_out)
			if (!candidate.skinned)
				(candidate.tree ? treeMeshes : candidate.terrain ? terrainMeshes : staticMeshes).insert(candidate.rendererData);
		a_stats.uniqueStaticMeshes = static_cast<uint32_t>(staticMeshes.size());
		a_stats.uniqueTerrainMeshes = static_cast<uint32_t>(terrainMeshes.size());
		a_stats.uniqueTreeMeshes = static_cast<uint32_t>(treeMeshes.size());

		QueryPerformanceCounter(&end);
		a_stats.traversalMs = static_cast<float>(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
		return true;
		}
	}
}
