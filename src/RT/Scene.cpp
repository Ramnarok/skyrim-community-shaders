#include "Scene.h"

#include "Features/GrassOptimizations.h"

#include <DirectXPackedVector.h>

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

		/** @param a_acceptLOD M8: extract LOD shape types like triangle shapes (the distant-LOD walk); else they're kLOD. */
		GeometryCategory Classify(RE::BSGeometry* a_geometry, GeometryCandidate& a_candidate, bool a_acceptLOD = false)
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
				if (!a_acceptLOD)
					return GeometryCategory::kLOD;
				break;
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
			a_candidate.decal = lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kDecal, RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal);
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
			const LoadedArea* distantLOD = nullptr;  // M8: walking TES::lodLandRoot; the loaded cells, to find LOD reaching into them
			std::vector<const RE::BSGeometry*>* waterShapes = nullptr;  // M8 water census: water shapes met by the cell walk
		};

		// M8 distant LOD: one shape under TES::lodLandRoot. Opaque triangle geometry is traced (land and object LOD); tree
		// billboards (instanced), alpha-tested, blended and decal LOD, water and effects aren't.
		void CollectDistantLOD(RE::BSGeometry* a_geometry, WalkOutput& a_out)
		{
			auto& lod = a_out.stats.lod;
			const auto type = static_cast<uint32_t>(a_geometry->GetType().get());
			if (type < lod.byGeometryType.size())
				lod.byGeometryType[type]++;

			GeometryCandidate candidate;
			const auto category = Classify(a_geometry, candidate, true);
			if (category == GeometryCategory::kInstanced) {
				lod.skippedTrees++;
				return;
			}
			if (category != GeometryCategory::kStaticMesh && category != GeometryCategory::kTerrain) {
				lod.skippedOther++;
				return;
			}
			// Object LOD is alpha-tested (its atlas has alpha): traced with the alpha test like any alpha-tested mesh.
			if (candidate.alphaBlended || candidate.decal) {
				lod.skippedBlendedDecal++;
				return;
			}
			// The BLAS and hit lookups read float3 positions (M3: every loaded mesh). LOD buffers weren't audited then, so a
			// half-position LOD shape is left out and counted rather than traced as garbage.
			RE::BSGraphics::VertexDesc desc;
			std::memcpy(&desc, &candidate.vertexDesc, sizeof(desc));
			if (HasHalfPositions(desc)) {
				lod.skippedHalfPositions++;
				return;
			}

			bool landLOD = false;
			if (auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()); property && property->material) {
				const auto feature = property->material->GetFeature();
				landLOD = feature == Feature::kLODLand || feature == Feature::kLODLandNoise;
			}
			candidate.terrain = false;  // not the loaded terrain: its own mask, clipped to outside the loaded cells
			candidate.distantLOD = true;
			candidate.geometry = a_geometry;
			candidate.world = a_geometry->world;

			// Its XY bound circle against the loaded cells' rectangle: overlapping ones are clipped by the traces.
			const auto& area = *a_out.distantLOD;
			const auto& bound = a_geometry->worldBound;
			if (area.bounded) {
				const float dx = std::max({ area.min.x - bound.center.x, 0.0f, bound.center.x - area.max.x });
				const float dy = std::max({ area.min.y - bound.center.y, 0.0f, bound.center.y - area.max.y });
				candidate.lodClip = dx * dx + dy * dy <= bound.radius * bound.radius;
			}
			lod.clippedShapes += candidate.lodClip;
			lod.alphaTestedShapes += candidate.alphaTested;
			lod.triangles += candidate.triangleCount;
			(landLOD ? lod.terrainShapes : lod.objectShapes)++;
			a_out.candidates.push_back(candidate);
		}

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

		// M8 tree LOD census: one tree group, sampled raw (formats unknown until measured).
		void SampleTreeGroup(const RE::BGSTerrainNode& a_node, const RE::BGSDistantTreeBlock& a_block, const RE::BGSDistantTreeBlock::TreeGroup& a_group, SceneStats::TreeLODSample& a_out)
		{
			a_out.baseCellX = a_node.baseCellX;
			a_out.baseCellY = a_node.baseCellY;
			a_out.lodLevel = a_node.GetLODLevel();
			a_out.blockAttached = a_block.attached;
			a_out.blockAllVisible = a_block.allVisible;
			a_out.treeType = a_group.treeType;
			a_out.groupNum = a_group.num;
			a_out.instanceArraySize = a_group.instances.size();
			for (uint32_t i = 0; i < a_group.instances.size() && i < 3; i++) {
				const auto& instance = a_group.instances[i];
				a_out.firstInstances.push_back({ instance.id, instance.x, instance.y, instance.z, instance.rotZ, instance.scale, instance.hidden ? 1u : 0u });
			}

			auto* geometry = a_group.geometry.get();
			if (!geometry)
				return;
			a_out.geometryName = geometry->name.c_str() ? geometry->name.c_str() : "";
			if (const auto* rtti = geometry->GetRTTI())
				a_out.geometryRTTI = rtti->GetName() ? rtti->GetName() : "";
			a_out.ancestorFlags = geometry->GetFlags().underlying();
			int depth = 0;
			for (const RE::NiNode* node = geometry->parent; node; node = node->parent) {
				a_out.ancestorFlags |= node->GetFlags().underlying();
				if (depth++ < 5) {
					if (!a_out.parents.empty())
						a_out.parents += " < ";
					a_out.parents += node->name.c_str() ? node->name.c_str() : "";
					if (const auto* rtti = node->GetRTTI(); rtti && rtti->GetName())
						a_out.parents += std::format(" ({})", rtti->GetName());
				}
			}
			a_out.worldTranslate[0] = geometry->world.translate.x;
			a_out.worldTranslate[1] = geometry->world.translate.y;
			a_out.worldTranslate[2] = geometry->world.translate.z;
			a_out.worldScale = geometry->world.scale;
			a_out.boundCenter[0] = geometry->worldBound.center.x;
			a_out.boundCenter[1] = geometry->worldBound.center.y;
			a_out.boundCenter[2] = geometry->worldBound.center.z;
			a_out.boundRadius = geometry->worldBound.radius;
			const auto& geometryData = geometry->GetGeometryRuntimeData();
			if (const auto* property = geometryData.shaderProperty.get(); property && property->GetRTTI() && property->GetRTTI()->GetName())
				a_out.propertyRTTI = property->GetRTTI()->GetName();
			if (auto* rendererData = geometryData.rendererData) {
				std::memcpy(&a_out.vertexDesc, &rendererData->vertexDesc, sizeof(a_out.vertexDesc));
				a_out.stride = static_cast<uint32_t>(a_out.vertexDesc & 0xF) * 4;
				a_out.rawVertices = rendererData->rawVertexData != nullptr;
				a_out.rawIndices = rendererData->rawIndexData != nullptr;
				if (auto* triShape = geometry->AsTriShape()) {
					a_out.vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
					a_out.triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
				}
				if (rendererData->rawVertexData && a_out.stride >= 24) {
					const auto* bytes = static_cast<const uint8_t*>(static_cast<const void*>(rendererData->rawVertexData));
					for (uint32_t v = 0; v < std::min(a_out.vertexCount, 4u); v++) {
						float values[6];
						std::memcpy(values, bytes + static_cast<size_t>(v) * a_out.stride, sizeof(values));
						a_out.firstVertices.insert(a_out.firstVertices.end(), values, values + 6);
					}
				}
			}

			if (auto* multiStream = geometry) {  // TreeGroup::geometry is typed BSMultiStreamInstanceTriShape
				auto& runtime = multiStream->GetMultiStreamTrishapeRuntimeData();
				a_out.instanceGroups = runtime.instanceGroups.size();
				a_out.meshTriCount = runtime.meshTriCount;
				a_out.maxInstancesPerGroup = runtime.maxInstancesPerGroup;
				a_out.instanceCount = runtime.instanceCount;
				a_out.instanceSize = runtime.instanceSize;
				a_out.activeGroupCount = runtime.activeGroupCount;
				a_out.renderDistance = runtime.renderDistance;
				if (!runtime.instanceGroups.empty() && runtime.instanceGroups[0]) {
					const auto* group = runtime.instanceGroups[0];
					a_out.group0TriCount = group->triCount;
					a_out.group0InstanceCount = group->instanceCount;
					a_out.group0Visible = group->isVisible;
					if (const auto* buffer = group->vertexBuffer) {
						a_out.group0ByteWidth = buffer->byteWidth;
						a_out.group0CpuData = buffer->m_data != nullptr;
						const size_t bytes = std::min<size_t>({ static_cast<size_t>(runtime.instanceSize) * 2, buffer->byteWidth, 128 });
						if (buffer->m_data && runtime.instanceSize > 0) {
							const auto* data = static_cast<const uint8_t*>(buffer->m_data);
							for (size_t b = 0; b < bytes; b++)
								a_out.group0FirstBytes += std::format("{:02x}{}", data[b], (b + 1) % runtime.instanceSize == 0 ? " | " : "");
						}
					}
				}
			}
		}

		std::string HexBytes(const void* a_data, size_t a_size)
		{
			std::string out;
			const auto* bytes = static_cast<const uint8_t*>(a_data);
			for (size_t i = 0; i < a_size; i++)
				out += std::format("{:02x}{}", bytes[i], (i + 1) % 8 == 0 ? " " : "");
			return out;
		}

		// M8: BSDistantTreeShaderProperty::GetBaseTexture() returns null (measured: 803 of 803 groups), so the billboard texture
		// comes from ForEachTexture. CommonLib declares BSShaderProperty::ForEachVisitor's destructor without defining it, so it
		// can't be derived from here; this class has the same vtable shape (slot 0 destructor, slot 1 Accept), and the game
		// only calls Accept.
		class FirstTextureVisitor
		{
		public:
			virtual ~FirstTextureVisitor() = default;
			virtual uint32_t Accept(RE::NiSourceTexture* a_texture)
			{
				count++;
				if (!first && a_texture)
					first = a_texture;
				return 1;
			}
			RE::NiSourceTexture* first = nullptr;
			uint32_t count = 0;
		};

		// M8 tree LOD tracing: where the census's candidates go.
		struct TreeLODTarget
		{
			std::vector<GeometryCandidate>* candidates = nullptr;  // null: census only
			const LoadedArea* area = nullptr;
			float cameraX = 0.0f, cameraY = 0.0f;
			ID3D11ShaderResourceView* atlasSRV = nullptr;  // the worldspace's tree billboard atlas (TreeLODAtlas)
		};

		// M8: the tree billboard atlas. Neither GetBaseTexture() nor ForEachTexture of BSDistantTreeShaderProperty returns it
		// (measured: 0 of 803 groups), so it's loaded by its conventional path, textures\terrain\<ws>\trees\<ws>treelod.dds
		// of the worldspace owning the LOD, through BSShaderManager::GetTexture (CommonLib; its AE ID 105640 resolves in the
		// 1.7.104 Address Library, checked 2026-09-25). Loaded once per worldspace and kept.
		ID3D11ShaderResourceView* TreeLODAtlas(RE::TESWorldSpace* a_worldSpace, SceneStats::TreeLOD& a_stats)
		{
			static std::string loadedPath;
			static RE::NiPointer<RE::NiTexture> texture;
			const char* editorID = a_worldSpace ? a_worldSpace->GetFormEditorID() : nullptr;
			if (!editorID || !*editorID)
				return nullptr;
			const std::string path = std::format(R"(textures\terrain\{0}\trees\{0}treelod.dds)", editorID);
			if (path != loadedPath) {
				loadedPath = path;
				texture.reset();
				RE::BSShaderManager::GetTexture(path.c_str(), true, texture, false);
				logger::info("[SkyrimRT] Tree LOD atlas {}: {}", path, texture ? "loaded" : "not found");
			}
			a_stats.atlasPath = loadedPath;
			auto* source = texture ? netimmerse_cast<RE::NiSourceTexture*>(texture.get()) : nullptr;
			a_stats.atlasLoaded = source && source->rendererTexture && source->rendererTexture->resourceView;
			return a_stats.atlasLoaded ? source->rendererTexture->resourceView : nullptr;
		}

		// M8: per tree group, its trees' world transforms (decoding and composing them cost ~0.5 ms a frame for 7,000 trees).
		// Keyed by the group; valid while its instance array, size and block origin are unchanged. Render thread only.
		struct CachedTreeGroup
		{
			const void* instances = nullptr;
			uint32_t count = 0;
			RE::NiPoint3 origin;
			std::vector<RE::NiTransform> worlds;
			std::vector<bool> valid;
			uint64_t seenWalk = 0;
		};

		ankerl::unordered_dense::map<const void*, CachedTreeGroup>& TreeGroupCache()
		{
			static ankerl::unordered_dense::map<const void*, CachedTreeGroup> cache;
			return cache;
		}

		uint64_t& TreeGroupWalk()
		{
			static uint64_t walk = 0;
			return walk;
		}

		constexpr float kTreeLODMaxDistance = 60000.0f;    // beyond the traces' 50,000-unit rays, plus a margin
		constexpr float kTreeLODRadiusPerScale = 1500.0f;  // bound radius per unit of instance scale (billboards ~1,000-1,400 tall)

		// M8: one distant-tree group (a tree type's crossed-quad card, instanced) as one alpha-tested candidate per tree.
		// Measured (census, 2026-09-25): BGSDistantTreeBlock::InstanceData x/y/z, rotZ and scale are half floats, the
		// position relative to the group shape's world transform (the block origin), rotZ in radians. The DistantTree vertex
		// shader places a vertex at position + Rz(rotZ) x (scale x model position), so the world transform is
		// geometry->world * (Rz(rotZ) scale, position).
		void CollectTreeGroup(const RE::BGSDistantTreeBlock::TreeGroup& a_group, SceneStats::TreeLOD& a_stats, const TreeLODTarget& a_target)
		{
			auto* geometry = a_group.geometry.get();
			if (!geometry || geometry->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			const auto& geometryData = geometry->GetGeometryRuntimeData();
			auto* rendererData = geometryData.rendererData;
			if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				return;
			const auto& counts = geometry->GetTrishapeRuntimeData();
			if (counts.vertexCount == 0 || counts.triangleCount == 0)
				return;

			GeometryCandidate base;
			base.rendererData = rendererData;
			std::memcpy(&base.vertexDesc, &rendererData->vertexDesc, sizeof(base.vertexDesc));
			base.vertexCount = counts.vertexCount;
			base.triangleCount = counts.triangleCount;
			base.geometry = geometry;
			base.distantLOD = true;
			base.treeLOD = true;
			base.alphaTested = true;
			base.alphaThreshold = 128;
			if (auto* alpha = geometryData.alphaProperty.get(); alpha && alpha->alphaThreshold > 0)
				base.alphaThreshold = alpha->alphaThreshold;
			if (auto* property = netimmerse_cast<RE::BSDistantTreeShaderProperty*>(geometryData.shaderProperty.get())) {
				RE::NiSourceTexture* texture = property->GetBaseTexture();
				if (!texture) {
					FirstTextureVisitor visitor;
					property->ForEachTexture(*reinterpret_cast<RE::BSShaderProperty::ForEachVisitor*>(&visitor));
					texture = visitor.first;
					a_stats.texturesFromVisitor += texture != nullptr;
				}
				if (texture && texture->rendererTexture) {
					base.diffuseSRV = texture->rendererTexture->resourceView;
					if (a_stats.textureName.empty() && texture->name.c_str())
						a_stats.textureName = texture->name.c_str();
				}
			}
			if (!base.diffuseSRV)
				base.diffuseSRV = a_target.atlasSRV;
			// Without the billboard's alpha a tree would be traced as a solid crossed card (blocky shadows): skip the group.
			if (!base.diffuseSRV) {
				a_stats.groupsWithoutTexture++;
				return;
			}

			// The decoded transforms only change when the block reloads (a new instance array) or moves: cached per group.
			auto& cached = TreeGroupCache()[&a_group];
			const auto& origin = geometry->world.translate;
			const bool moved = cached.origin.x != origin.x || cached.origin.y != origin.y || cached.origin.z != origin.z;
			if (cached.instances != a_group.instances.data() || cached.count != a_group.instances.size() || moved) {
				cached.instances = a_group.instances.data();
				cached.count = a_group.instances.size();
				cached.origin = origin;
				cached.worlds.resize(cached.count);
				cached.valid.assign(cached.count, false);
				for (uint32_t i = 0; i < cached.count; i++) {
					const auto& instance = a_group.instances[i];
					using DirectX::PackedVector::XMConvertHalfToFloat;
					const RE::NiPoint3 local(XMConvertHalfToFloat(instance.x), XMConvertHalfToFloat(instance.y), XMConvertHalfToFloat(instance.z));
					const float rotZ = XMConvertHalfToFloat(instance.rotZ);
					const float scale = XMConvertHalfToFloat(instance.scale);
					if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(rotZ) || !(scale > 0.0f) || scale > 100.0f)
						continue;
					RE::NiTransform localTransform;
					const float c = std::cos(rotZ), s = std::sin(rotZ);
					localTransform.rotate = RE::NiMatrix3(RE::NiPoint3(c, -s, 0.0f), RE::NiPoint3(s, c, 0.0f), RE::NiPoint3(0.0f, 0.0f, 1.0f));
					localTransform.scale = scale;
					localTransform.translate = local;
					cached.worlds[i] = geometry->world * localTransform;
					cached.valid[i] = true;
				}
				a_stats.groupsDecoded++;
			}
			cached.seenWalk = TreeGroupWalk();

			const auto& area = *a_target.area;
			for (uint32_t i = 0; i < cached.count; i++) {
				if (a_group.instances[i].hidden) {
					a_stats.skippedHidden++;
					continue;
				}
				if (!cached.valid[i]) {
					a_stats.skippedInvalid++;
					continue;
				}
				const RE::NiTransform& world = cached.worlds[i];

				// Inside the loaded cells the game draws the full tree; beyond the rays' reach nothing can hit it.
				const float radius = kTreeLODRadiusPerScale * world.scale;
				const auto& p = world.translate;
				const bool insideLoaded = area.bounded && p.x >= area.min.x + radius && p.x <= area.max.x - radius && p.y >= area.min.y + radius && p.y <= area.max.y - radius;
				if (insideLoaded) {
					a_stats.skippedInsideLoaded++;
					continue;
				}
				const float cx = p.x - a_target.cameraX, cy = p.y - a_target.cameraY;
				if (cx * cx + cy * cy > kTreeLODMaxDistance * kTreeLODMaxDistance) {
					a_stats.skippedFar++;
					continue;
				}
				const float dx = std::max({ area.min.x - p.x, 0.0f, p.x - area.max.x });
				const float dy = std::max({ area.min.y - p.y, 0.0f, p.y - area.max.y });
				GeometryCandidate candidate = base;
				candidate.world = world;
				candidate.lodClip = area.bounded && dx * dx + dy * dy <= radius * radius;
				a_stats.clipped += candidate.lodClip;
				a_stats.traced++;
				a_target.candidates->push_back(candidate);
			}
		}

		// M8 tree LOD: walks the worldspace's terrain quadtree (BGSTerrainManager, CommonLib-defined) for its distant-tree
		// blocks, counts them with raw samples (census) and, with a target, adds each visible tree as a candidate.
		void CensusTreeLOD(RE::TESWorldSpace* a_worldSpace, SceneStats::TreeLOD& a_out, const TreeLODTarget& a_target)
		{
			a_out.walked = true;
			a_out.stage = 1;
			auto* manager = a_worldSpace ? a_worldSpace->GetTerrainManager() : nullptr;
			a_out.haveManager = manager != nullptr;
			if (!manager)
				return;
			a_out.stage = 2;
			a_out.managerAddress = reinterpret_cast<uint64_t>(manager);
			a_out.managerHex = HexBytes(manager, sizeof(RE::BGSTerrainManager));
			if (!manager->rootNode)
				return;
			a_out.stage = 3;
			a_out.rootNodeAddress = reinterpret_cast<uint64_t>(manager->rootNode);
			a_out.rootNodeHex = HexBytes(manager->rootNode, sizeof(RE::BGSTerrainNode));
			// The atlas belongs to the worldspace that owns the LOD (a child worldspace may use its parent's).
			TreeLODTarget target = a_target;
			const uint64_t walk = ++TreeGroupWalk();
			if (target.candidates)
				target.atlasSRV = TreeLODAtlas(manager->worldSpace, a_out);
			a_out.stage = 4;
			std::vector<const RE::BGSTerrainNode*> stack{ manager->rootNode };
			while (!stack.empty() && a_out.nodes < 50000) {
				const auto* node = stack.back();
				stack.pop_back();
				a_out.nodes++;
				// BGSTerrainNode::children points at the four child nodes themselves, stored contiguously (measured on 1.7.104:
				// root + 0x50), not at four pointers as CommonLib types it. Each child must name this manager and node.
				if (node->children) {
					const auto* first = reinterpret_cast<const RE::BGSTerrainNode*>(node->children);
					for (uint32_t i = 0; i < 4; i++) {
						const auto* child = first + i;
						if (child->manager == manager && child->parent == node)
							stack.push_back(child);
						else
							a_out.childMismatches++;
					}
				}
				const auto* layer = node->trees;
				if (!layer)
					continue;
				a_out.treeLayers++;
				const auto* block = layer->block;
				if (!block)
					continue;
				a_out.blocks++;
				a_out.blocksAttached += block->attached;
				for (const auto* group : block->treeGroups) {
					if (!group)
						continue;
					a_out.groups++;
					a_out.groupsWithGeometry += group->geometry != nullptr;
					a_out.instances += group->instances.size();
					for (const auto& instance : group->instances)
						a_out.hiddenInstances += instance.hidden;
					if (block->attached && group->geometry && a_out.samples.size() < 4) {
						a_out.stage = 5;
						SampleTreeGroup(*node, *block, *group, a_out.samples.emplace_back());
						a_out.stage = 4;
					}
					if (block->attached && a_target.candidates) {
						a_out.stage = 6;
						CollectTreeGroup(*group, a_out, target);
						a_out.stage = 4;
					}
				}
			}
			// Groups not seen this walk (their blocks detached) drop their cached transforms.
			if (target.candidates)
				std::erase_if(TreeGroupCache(), [walk](const auto& a_entry) { return a_entry.second.seenWalk != walk; });
			a_out.cachedGroups = static_cast<uint32_t>(TreeGroupCache().size());
		}

		// Its own guard: a fault here (layout mismatch) must not drop the frame's scene. Kept free of C++ objects for __try.
		bool CensusTreeLODGuarded(RE::TESWorldSpace* a_worldSpace, SceneStats::TreeLOD& a_out, const TreeLODTarget& a_target)
		{
			__try {
				CensusTreeLOD(a_worldSpace, a_out, a_target);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// M8 water census: one water object's shape, with its planes, placement and the extent of its raw positions.
		void SampleWater(const RE::TESWaterObject& a_object, RE::BSTriShape& a_shape, SceneStats::WaterSample& a_out, float& a_worldZMin, float& a_worldZMax)
		{
			a_out.objectFlags = a_object.flags;
			a_out.multiBounds = a_object.multiBounds.size();
			a_out.objectPlane[0] = a_object.plane.normal.x;
			a_out.objectPlane[1] = a_object.plane.normal.y;
			a_out.objectPlane[2] = a_object.plane.normal.z;
			a_out.objectPlane[3] = a_object.plane.constant;
			a_out.geometryName = a_shape.name.c_str() ? a_shape.name.c_str() : "";
			if (const auto* rtti = a_shape.GetRTTI(); rtti && rtti->GetName())
				a_out.geometryRTTI = rtti->GetName();
			a_out.ancestorFlags = a_shape.GetFlags().underlying();
			int depth = 0;
			for (const RE::NiNode* node = a_shape.parent; node; node = node->parent) {
				a_out.ancestorFlags |= node->GetFlags().underlying();
				if (depth++ < 5) {
					if (!a_out.parents.empty())
						a_out.parents += " < ";
					a_out.parents += node->name.c_str() ? node->name.c_str() : "";
					if (const auto* rtti = node->GetRTTI(); rtti && rtti->GetName())
						a_out.parents += std::format(" ({})", rtti->GetName());
				}
			}
			const auto& world = a_shape.world;
			a_out.worldTranslate[0] = world.translate.x;
			a_out.worldTranslate[1] = world.translate.y;
			a_out.worldTranslate[2] = world.translate.z;
			a_out.worldScale = world.scale;
			for (int c = 0; c < 3; c++)
				a_out.worldRotateZRow[c] = world.rotate.entry[2][c];
			a_out.boundCenter[0] = a_shape.worldBound.center.x;
			a_out.boundCenter[1] = a_shape.worldBound.center.y;
			a_out.boundCenter[2] = a_shape.worldBound.center.z;
			a_out.boundRadius = a_shape.worldBound.radius;

			const auto& geometryData = a_shape.GetGeometryRuntimeData();
			if (auto* property = geometryData.shaderProperty.get()) {
				if (const auto* rtti = property->GetRTTI(); rtti && rtti->GetName())
					a_out.propertyRTTI = rtti->GetName();
				if (auto* water = netimmerse_cast<RE::BSWaterShaderProperty*>(property)) {
					a_out.waterFlags = water->waterFlags.underlying();
					a_out.propertyPlane[0] = water->plane.normal.x;
					a_out.propertyPlane[1] = water->plane.normal.y;
					a_out.propertyPlane[2] = water->plane.normal.z;
					a_out.propertyPlane[3] = water->plane.constant;
				}
			}
			a_out.vertexCount = a_shape.GetTrishapeRuntimeData().vertexCount;
			a_out.triangleCount = a_shape.GetTrishapeRuntimeData().triangleCount;
			auto* rendererData = geometryData.rendererData;
			if (!rendererData)
				return;
			std::memcpy(&a_out.vertexDesc, &rendererData->vertexDesc, sizeof(a_out.vertexDesc));
			a_out.stride = static_cast<uint32_t>(a_out.vertexDesc & 0xF) * 4;
			a_out.rawVertices = rendererData->rawVertexData != nullptr;
			a_out.rawIndices = rendererData->rawIndexData != nullptr;
			if (!rendererData->rawVertexData || a_out.stride < 12 || a_out.vertexCount == 0)
				return;
			// Positions are float3 at offset 0 (M3).
			const auto* bytes = static_cast<const uint8_t*>(static_cast<const void*>(rendererData->rawVertexData));
			a_worldZMin = FLT_MAX;
			a_worldZMax = -FLT_MAX;
			for (int c = 0; c < 3; c++) {
				a_out.localMin[c] = FLT_MAX;
				a_out.localMax[c] = -FLT_MAX;
			}
			for (uint32_t v = 0; v < a_out.vertexCount; v++) {
				float p[3];
				std::memcpy(p, bytes + static_cast<size_t>(v) * a_out.stride, sizeof(p));
				for (int c = 0; c < 3; c++) {
					a_out.localMin[c] = std::min(a_out.localMin[c], p[c]);
					a_out.localMax[c] = std::max(a_out.localMax[c], p[c]);
				}
				const float z = world.translate.z + world.scale * (world.rotate.entry[2][0] * p[0] + world.rotate.entry[2][1] * p[1] + world.rotate.entry[2][2] * p[2]);
				a_worldZMin = std::min(a_worldZMin, z);
				a_worldZMax = std::max(a_worldZMax, z);
			}
			a_out.worldZMin = a_worldZMin;
			a_out.worldZMax = a_worldZMax;
		}

		void CensusWater(RE::TES* a_tes, const std::vector<const RE::BSGeometry*>& a_cellWalkShapes, SceneStats::Water& a_out)
		{
			a_out.walked = true;
			a_out.stage = 1;
			auto* system = RE::TESWaterSystem::GetSingleton();
			if (!system)
				return;
			a_out.haveSystem = true;
			a_out.enabled = system->enabled;
			a_out.playerUnderwater = system->playerUnderwater;
			a_out.underwaterHeight = system->underwaterHeight;
			a_out.reflections = system->waterReflections.size();
			a_out.displacements = system->waterDisplacement.size();
			a_out.normals = system->waterNormals.size();

			a_out.stage = 2;
			ankerl::unordered_dense::set<const RE::BSGeometry*> systemShapes;
			for (const auto& objectPtr : system->waterObjects) {
				const auto* object = objectPtr.get();
				if (!object)
					continue;
				a_out.objects++;
				auto* shape = object->shape.get();
				if (!shape)
					continue;
				a_out.withShape++;
				systemShapes.insert(shape);
				SceneStats::WaterSample sample;
				float zMin = 0.0f, zMax = 0.0f;
				SampleWater(*object, *shape, sample, zMin, zMax);
				sample.inCellWalk = std::find(a_cellWalkShapes.begin(), a_cellWalkShapes.end(), shape) != a_cellWalkShapes.end();
				a_out.inCellWalk += sample.inCellWalk;
				a_out.visible += (sample.ancestorFlags & static_cast<uint32_t>(RE::NiAVObject::Flag::kHidden)) == 0;
				a_out.rawBoth += sample.rawVertices && sample.rawIndices;
				a_out.flat += sample.rawVertices && zMax - zMin < 1.0f;
				a_out.triangles += sample.triangleCount;
				if (a_out.samples.size() < 12)
					a_out.samples.push_back(std::move(sample));
			}
			a_out.cellWalkShapes = static_cast<uint32_t>(a_cellWalkShapes.size());
			for (const auto* shape : a_cellWalkShapes)
				a_out.cellWalkUnmatched += !systemShapes.contains(shape);

			a_out.stage = 3;
			if (auto* lodWater = a_tes->objLODWaterRoot) {
				RE::BSVisit::TraverseScenegraphGeometries(lodWater, [&](RE::BSGeometry* a_geometry) {
					a_out.lodWaterShapes++;
					bool hidden = false;
					for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
						hidden |= object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
					a_out.lodWaterVisible += !hidden;
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}

		// Its own guard, as the tree census: UnifiedWater adds water objects from other threads.
		bool CensusWaterGuarded(RE::TES* a_tes, const std::vector<const RE::BSGeometry*>& a_cellWalkShapes, SceneStats::Water& a_out)
		{
			__try {
				CensusWater(a_tes, a_cellWalkShapes, a_out);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
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
			// A hidden node hides its whole subtree (disabled references, etc.; under the LOD root, blocks and cells the game
			// has app-culled, such as a LOD-4 block's cells that are loaded).
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				(a_out.distantLOD ? a_out.stats.lod.hiddenSubtrees : a_out.stats.hiddenSubtrees)++;
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
				if (a_out.distantLOD) {
					CollectDistantLOD(geometry, a_out);
					return;
				}
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
					a_out.stats.decalInstances += candidate.decal;
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
				case GeometryCategory::kEffectOrWater:
					if (a_out.waterShapes && netimmerse_cast<RE::BSWaterShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get()))
						a_out.waterShapes->push_back(geometry);
					break;  // effects, water and sky don't write the pre-water depth we compare against
				default:
					// Particles don't write the pre-water depth either.
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
			if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get()) {
				using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlag;
				object.materialAlpha = property->alpha;
				object.lastRenderPassState = property->lastRenderPassState;
				for (const auto* pass = property->renderPassList.head; pass && object.renderPasses < 16; pass = pass->next)
					object.renderPasses++;
				object.vertexAlpha = property->flags.any(ShaderFlag::kVertexAlpha);
				object.decal = property->flags.any(ShaderFlag::kDecal, ShaderFlag::kDynamicDecal);
			}
			RE::BSGraphics::VertexDesc desc;
			std::memcpy(&desc, &candidate.vertexDesc, sizeof(desc));
			object.vertexColors = desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS);
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
		std::vector<const RE::BSGeometry*> waterShapes;
		out.waterShapes = &waterShapes;
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

		// M8 water census (diagnostic): stops for the session after a fault and keeps what it had reached then.
		{
			static bool waterCensusFaulted = false;
			static SceneStats::Water faultedWater;
			if (!waterCensusFaulted && !CensusWaterGuarded(tes, waterShapes, a_stats.water)) {
				waterCensusFaulted = true;
				a_stats.water.faulted = true;
				faultedWater = a_stats.water;
				logger::warn("[SkyrimRT] Water census faulted at stage {} after {} water objects; disabled for this session", faultedWater.stage, faultedWater.objects);
			}
			if (waterCensusFaulted)
				a_stats.water = faultedWater;
		}
		out.waterShapes = nullptr;

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
		// M8 distant LOD: land and object LOD hang under TES::lodLandRoot (a direct TES member on every runtime). The game
		// detaches blocks it isn't drawing and app-culls (kHidden) the loaded cells of the finest blocks; what overlaps the
		// loaded cells anyway is clipped by the traces (CollectDistantLOD). The cell walk above filled a_area.
		if (!interior && a_options.distantLOD && a_area.bounded && tes->lodLandRoot) {
			out.distantLOD = &a_area;
			Walk(tes->lodLandRoot, out);
			out.distantLOD = nullptr;
			a_stats.lod.walked = true;
			// The census stops for the session after a fault and reports what it had reached then.
			static bool treeCensusFaulted = false;
			static SceneStats::TreeLOD faultedCensus;
			TreeLODTarget treeTarget;
			treeTarget.candidates = &a_out;
			treeTarget.area = &a_area;
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				treeTarget.cameraX = player->GetPosition().x;
				treeTarget.cameraY = player->GetPosition().y;
			}
			const size_t candidatesBefore = a_out.size();
			if (!treeCensusFaulted && !CensusTreeLODGuarded(worldSpace, a_stats.treeLOD, treeTarget)) {
				a_out.resize(candidatesBefore);  // a partial group: drop what this walk added
				treeCensusFaulted = true;
				a_stats.treeLOD.faulted = true;
				faultedCensus = a_stats.treeLOD;
				logger::warn("[SkyrimRT] Tree LOD census faulted at stage {} after {} quadtree nodes; disabled for this session", faultedCensus.stage, faultedCensus.nodes);
			}
			if (treeCensusFaulted)
				a_stats.treeLOD = faultedCensus;
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
