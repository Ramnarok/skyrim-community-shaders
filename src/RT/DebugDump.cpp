#include "DebugDump.h"

#include <DirectXTex.h>

namespace RT
{
	namespace
	{
		json TimingJson(const TimingSeries& a_series)
		{
			return { { "avg", a_series.Average() }, { "max", a_series.Max() }, { "samples", a_series.count } };
		}

		std::string VertexFlagNames(uint16_t a_flags)
		{
			using V = RE::BSGraphics::Vertex;
			constexpr std::pair<uint16_t, const char*> kNames[] = {
				{ V::VF_VERTEX, "VERTEX" }, { V::VF_UV, "UV" }, { V::VF_UV_2, "UV2" }, { V::VF_NORMAL, "NORMAL" },
				{ V::VF_TANGENT, "TANGENT" }, { V::VF_COLORS, "COLORS" }, { V::VF_SKINNED, "SKINNED" }, { V::VF_LANDDATA, "LANDDATA" },
				{ V::VF_EYEDATA, "EYEDATA" }, { V::VF_INSTANCEDATA, "INSTANCEDATA" }, { V::VF_FULLPREC, "FULLPREC" }
			};
			std::string names;
			for (const auto& [bit, name] : kNames) {
				if (a_flags & bit)
					names += names.empty() ? name : std::string("|") + name;
			}
			return names;
		}

		// M8: what the walk of TES::lodLandRoot found (BSGeometry::Type names as CommonLib's enum).
		json DistantLODJson(const SceneStats::DistantLOD& a_lod)
		{
			constexpr const char* kTypeNames[] = { "geometry", "particles", "strip_particles", "tri_shape", "dynamic_tri_shape", "mesh_lod_tri_shape",
				"lod_multi_index_tri_shape", "multi_index_tri_shape", "sub_index_tri_shape", "sub_index_land_tri_shape", "multi_stream_instance_tri_shape",
				"particle_shader_dynamic_tri_shape", "lines", "dynamic_lines", "instance_group" };
			json types = json::object();
			for (size_t i = 0; i < a_lod.byGeometryType.size(); i++) {
				if (a_lod.byGeometryType[i])
					types[i < std::size(kTypeNames) ? kTypeNames[i] : std::format("type_{}", i)] = a_lod.byGeometryType[i];
			}
			return {
				{ "walked", a_lod.walked },
				{ "traced", { { "land_shapes", a_lod.terrainShapes }, { "object_shapes", a_lod.objectShapes }, { "clipped_to_outside_loaded_cells", a_lod.clippedShapes },
								{ "alpha_tested", a_lod.alphaTestedShapes }, { "triangles", a_lod.triangles } } },
				{ "skipped", { { "tree_billboards", a_lod.skippedTrees }, { "blended_decal", a_lod.skippedBlendedDecal }, { "water_effects_other", a_lod.skippedOther }, { "half_positions", a_lod.skippedHalfPositions },
								 { "hidden_subtrees", a_lod.hiddenSubtrees } } },
				{ "shapes_by_type", types },
			};
		}

		// M8: the terrain quadtree's distant-tree blocks, with raw samples (diagnostic, before tree LOD is traced).
		json TreeLODJson(const SceneStats::TreeLOD& a_tree)
		{
			json samples = json::array();
			for (const auto& s : a_tree.samples) {
				json instances = json::array();
				for (const auto& i : s.firstInstances)
					instances.push_back({ { "id", std::format("{:08x}", i[0]) }, { "x", i[1] }, { "y", i[2] }, { "z", i[3] }, { "rot_z", i[4] }, { "scale", i[5] }, { "hidden", i[6] != 0 } });
				samples.push_back({ { "base_cell", { s.baseCellX, s.baseCellY } }, { "lod_level", s.lodLevel }, { "block_attached", s.blockAttached }, { "block_all_visible", s.blockAllVisible },
					{ "tree_type", s.treeType }, { "group_num", s.groupNum }, { "instance_array_size", s.instanceArraySize }, { "first_instances", instances },
					{ "geometry", { { "name", s.geometryName }, { "rtti", s.geometryRTTI }, { "property_rtti", s.propertyRTTI }, { "parents", s.parents },
									  { "ancestor_flags", std::format("{:08x}", s.ancestorFlags) }, { "world_translate", { s.worldTranslate[0], s.worldTranslate[1], s.worldTranslate[2] } },
									  { "world_scale", s.worldScale }, { "bound", { s.boundCenter[0], s.boundCenter[1], s.boundCenter[2], s.boundRadius } },
									  { "vertex_desc", std::format("{:016x}", s.vertexDesc) }, { "stride", s.stride }, { "vertices", s.vertexCount }, { "triangles", s.triangleCount },
									  { "raw_vertices", s.rawVertices }, { "raw_indices", s.rawIndices }, { "first_vertices_6_floats_each", s.firstVertices } } },
					{ "multi_stream", { { "instance_groups", s.instanceGroups }, { "mesh_tri_count", s.meshTriCount }, { "max_instances_per_group", s.maxInstancesPerGroup },
										  { "instance_count", s.instanceCount }, { "instance_size", s.instanceSize }, { "active_group_count", s.activeGroupCount },
										  { "render_distance", s.renderDistance }, { "group0_tri_count", s.group0TriCount }, { "group0_instance_count", s.group0InstanceCount },
										  { "group0_visible", s.group0Visible }, { "group0_cpu_data", s.group0CpuData }, { "group0_byte_width", s.group0ByteWidth },
										  { "group0_first_two_instances_hex", s.group0FirstBytes } } } });
			}
			return { { "walked", a_tree.walked }, { "terrain_manager", a_tree.haveManager }, { "faulted", a_tree.faulted }, { "stage_reached", a_tree.stage },
				{ "manager_address", std::format("{:016x}", a_tree.managerAddress) }, { "manager_hex", a_tree.managerHex },
				{ "root_node_address", std::format("{:016x}", a_tree.rootNodeAddress) }, { "root_node_hex", a_tree.rootNodeHex }, { "quadtree_nodes", a_tree.nodes }, { "child_mismatches", a_tree.childMismatches },
				{ "traced", { { "trees", a_tree.traced }, { "clipped", a_tree.clipped } } },
				{ "skipped", { { "hidden", a_tree.skippedHidden }, { "inside_loaded_cells", a_tree.skippedInsideLoaded }, { "beyond_60000_units", a_tree.skippedFar },
								 { "invalid_instance_data", a_tree.skippedInvalid } } },
				{ "groups_without_texture", a_tree.groupsWithoutTexture }, { "textures_from_visitor", a_tree.texturesFromVisitor }, { "texture_name", a_tree.textureName },
				{ "atlas", { { "path", a_tree.atlasPath }, { "loaded", a_tree.atlasLoaded } } },
				{ "transform_cache", { { "groups_decoded_this_frame", a_tree.groupsDecoded }, { "groups_cached", a_tree.cachedGroups } } }, { "tree_layers", a_tree.treeLayers },
				{ "blocks", a_tree.blocks }, { "blocks_attached", a_tree.blocksAttached }, { "groups", a_tree.groups }, { "groups_with_geometry", a_tree.groupsWithGeometry },
				{ "instances", a_tree.instances }, { "hidden_instances", a_tree.hiddenInstances }, { "samples", samples } };
		}

		// M8: TESWaterSystem's water objects and where water shapes live (diagnostic, before water is traced).
		json WaterJson(const SceneStats::Water& a_water)
		{
			auto f3 = [](const float* a_v) { return json{ a_v[0], a_v[1], a_v[2] }; };
			json samples = json::array();
			for (const auto& s : a_water.samples)
				samples.push_back({ { "name", s.geometryName }, { "rtti", s.geometryRTTI }, { "property_rtti", s.propertyRTTI }, { "parents", s.parents },
					{ "ancestor_flags", std::format("{:08x}", s.ancestorFlags) }, { "water_flags", std::format("{:08x}", s.waterFlags) }, { "object_flags", s.objectFlags },
					{ "multi_bounds", s.multiBounds }, { "in_cell_walk", s.inCellWalk },
					{ "object_plane", { s.objectPlane[0], s.objectPlane[1], s.objectPlane[2], s.objectPlane[3] } },
					{ "property_plane", { s.propertyPlane[0], s.propertyPlane[1], s.propertyPlane[2], s.propertyPlane[3] } },
					{ "world_translate", f3(s.worldTranslate) }, { "world_scale", s.worldScale }, { "world_rotate_z_row", f3(s.worldRotateZRow) },
					{ "bound", { s.boundCenter[0], s.boundCenter[1], s.boundCenter[2], s.boundRadius } },
					{ "vertex_desc", std::format("{:016x}", s.vertexDesc) }, { "stride", s.stride }, { "vertices", s.vertexCount }, { "triangles", s.triangleCount },
					{ "raw_vertices", s.rawVertices }, { "raw_indices", s.rawIndices }, { "local_min", f3(s.localMin) }, { "local_max", f3(s.localMax) },
					{ "world_z_range", { s.worldZMin, s.worldZMax } } });
			return { { "walked", a_water.walked }, { "water_system", a_water.haveSystem }, { "faulted", a_water.faulted }, { "stage_reached", a_water.stage },
				{ "enabled", a_water.enabled }, { "player_underwater", a_water.playerUnderwater }, { "underwater_height", a_water.underwaterHeight },
				{ "objects", { { "total", a_water.objects }, { "with_shape", a_water.withShape }, { "visible", a_water.visible }, { "flat_under_1_unit", a_water.flat },
								 { "raw_vertices_and_indices", a_water.rawBoth }, { "in_cell_walk", a_water.inCellWalk }, { "triangles", a_water.triangles } } },
				{ "reflections", a_water.reflections }, { "displacements", a_water.displacements }, { "normals", a_water.normals },
				{ "cell_walk_water_shapes", { { "total", a_water.cellWalkShapes }, { "not_in_water_system", a_water.cellWalkUnmatched } } },
				{ "lod_water", { { "shapes", a_water.lodWaterShapes }, { "visible", a_water.lodWaterVisible } } }, { "samples", samples } };
		}

		json SceneJson(const DebugDumpData& a_data)
		{
			const auto& s = a_data.scene;
			json instances = json::object();
			json exclusions = json::object();
			for (size_t i = 0; i < s.instances.size(); i++) {
				const std::string name(GetCategoryName(static_cast<GeometryCategory>(i)));
				instances[name] = s.instances[i];
				if (s.exclusionsByCategory[i])
					exclusions[name] = { { "bounds", s.exclusionsByCategory[i] }, { "max_radius", s.exclusionMaxRadius[i] } };
			}
			exclusions["rejected_radius_over_20000"] = s.exclusionsRejectedTooLarge;
			return {
				{ "in_world", a_data.inWorld },
				{ "cells", s.cells },
				{ "hidden_subtrees_skipped", s.hiddenSubtrees },
				{ "geometry_instances", instances },
				{ "exclusion_bounds_by_category", exclusions },
				{ "alpha_tested_instances", s.alphaTestedInstances },
				{ "alpha_blended_instances", s.alphaBlendedInstances },
				{ "decal_instances_not_traced", s.decalInstances },
				{ "water_instances", s.waterInstances },
				{ "grass", { { "walked", s.grassWalked }, { "exclusion_bounds", s.grassBounds } } },
				{ "unique_meshes", { { "static_mesh", s.uniqueStaticMeshes }, { "terrain", s.uniqueTerrainMeshes } } },
				{ "mesh_lod_shapes", { { "walked", s.meshLODShapes }, { "alternates_skipped", s.meshLODAlternates }, { "all_skipped_diagnostic", s.meshLODSkipped } } },
				{ "trees", { { "shapes", s.treeShapes },{ "leaf_anim_shapes", s.leafAnimShapes },{ "rest_pose_shapes", s.treeRestPoseShapes }, { "static_shapes", s.treeStaticShapes },
							   { "static_partitions", s.treeStaticPartitions }, { "static_unique_meshes", s.uniqueTreeMeshes },
							   { "half_position_partitions_skinned", s.treeHalfPositionPartitions } } },
				{ "instances_in_lit_rooms", s.instancesInRooms }, { "lit_room_nodes", s.roomNodes },
				{ "distant_lod", DistantLODJson(s.lod) },
				{ "tree_lod_census", TreeLODJson(s.treeLOD) },
				{ "water_census", WaterJson(s.water) },
				{ "traversal_ms", TimingJson(a_data.sceneTraversalMs) },
			};
		}

		json CacheJson(const MeshCacheStats& c)
		{
			json formats = json::array();
			for (const auto& f : c.formats) {
				formats.push_back({ { "flags", std::format("{:#06x}", f.flags) },
					{ "flag_names", VertexFlagNames(f.flags) },
					{ "stride_from_desc", f.strideFromDesc },
					{ "stride_from_getsize", f.strideFromGetSize },
					{ "vb_bytes_per_vertex", f.vbBytesPerVertex },
					{ "meshes", f.meshes } });
			}
			constexpr double kMB = 1024.0 * 1024.0;
			return {
				{ "entries", c.entries },
				{ "resident", c.resident },
				{ "pending", c.pending },
				{ "resident_vb_mb", c.residentVertexBytes / kMB },
				{ "resident_ib_mb", c.residentIndexBytes / kMB },
				{ "pool_reserved_mb", c.poolBytes / kMB },
				{ "pool_pages", c.poolPages },
				{ "per_frame",
					{ { "last_frame", { { "uploads", c.uploadsLastFrame }, { "evictions", c.evictionsLastFrame }, { "readbacks_started", c.readbacksLastFrame }, { "upload_mb", c.uploadBytesLastFrame / kMB } } },
						{ "uploads", TimingJson(c.uploadsPerFrame) },
						{ "evictions", TimingJson(c.evictionsPerFrame) },
						{ "upload_mb", TimingJson(c.uploadMBPerFrame) },
						{ "update_ms", TimingJson(c.updateMs) } } },
				{ "totals", { { "uploads", c.totalUploads }, { "evictions", c.totalEvictions }, { "readbacks", c.totalReadbacks }, { "failed", c.totalFailed }, { "deferred", c.totalDeferred } } },
				{ "upload_source", { { "raw_cpu", c.sourceRawCpu }, { "d3d11_readback", c.sourceD3D11Readback } } },
				{ "raw_pointers_per_new_mesh", { { "both", c.rawBoth }, { "vertex_only", c.rawVertexOnly }, { "index_only", c.rawIndexOnly }, { "neither", c.rawNeither } } },
				{ "raw_vs_gpu_check", { { "compared", c.rawCompared }, { "matched", c.rawMatched }, { "mismatched", c.rawMismatched }, { "copy_faults", c.rawCopyFaults } } },
				{ "stride_check", { { "vb_size_matches", c.strideMatchesVB }, { "vb_size_mismatches", c.strideMismatchesVB }, { "ib_size_matches", c.indexBytesMatchIB }, { "ib_size_mismatches", c.indexBytesMismatchIB } } },
				{ "vertex_formats", formats },
			};
		}

		json TraceJson(const DebugDumpData& a_data)
		{
			const auto& t = a_data.trace;
			const auto& c = a_data.cache;
			constexpr double kMB = 1024.0 * 1024.0;
			constexpr float kTargetPercent = 2.0f;
			json images = json::array();
			for (const auto& image : a_data.images) {
				if (image.name == "depth" || image.name == "instance" || image.name == "normal" || image.name == "diff")
					images.push_back(std::format("debug_{}_{}.png", image.name, a_data.gameFrame));
			}
			return {
				{ "traced_dump_frame", a_data.haveTrace },
				{ "have_result", t.haveResult },
				{ "render_size", { t.renderWidth, t.renderHeight } },
				{ "instances", t.instances },
				{ "instances_dropped", t.instancesDropped },
				{ "exclusion_bounds", t.exclusions },
				{ "pixels",
					{ { "render", t.counters[kRenderPixels] },
						{ "sky", t.counters[kSky] },
						{ "outside_loaded_cells", t.counters[kOutsideLoaded] },
						{ "excluded_occluder_not_in_tlas", t.counters[kExcluded] },
						{ "excluded_alpha_tested_mismatch", t.counters[kExcludedAlpha] },
						{ "excluded_terrain_clutter", t.counters[kExcludedClutter] },
						{ "excluded_wind_animated_foliage", t.counters[kExcludedWind] },
						{ "counted", t.counters[kCounted] },
						{ "matched", t.counters[kMatched] },
						{ "traced_nearer", t.counters[kTracedNearer] },
						{ "traced_farther", t.counters[kTracedFarther] },
						{ "traced_miss", t.counters[kTracedMiss] },
						{ "alpha_tested_counted", t.counters[kAlphaTestedCounted] },
						{ "alpha_tested_matched", t.counters[kAlphaTestedMatched] } } },
				{ "alpha_tested_mismatch_percent", t.counters[kAlphaTestedCounted] ? 100.0f * (t.counters[kAlphaTestedCounted] - t.counters[kAlphaTestedMatched]) / t.counters[kAlphaTestedCounted] : 0.0f },
				{ "mismatch_threshold_relative", Raytracer::kMismatchThreshold },
				{ "clutter_height_units", Raytracer::kClutterHeight },
				{ "depth_mismatch_percent", t.MismatchPercent() },
				{ "coverage_percent", t.CoveragePercent() },
				{ "target_percent", kTargetPercent },
				{ "within_target", t.haveResult && t.counters[kCounted] > 0 && t.MismatchPercent() < kTargetPercent },
				{ "window", { { "depth_mismatch_percent", TimingJson(t.mismatchPercent) }, { "coverage_percent", TimingJson(t.coveragePercent) } } },
				// M8 distant LOD: the same depth test outside the loaded cells (tree LOD billboards aren't traced: they count as
				// traced farther / miss here).
				{ "outside_loaded_cells",
					{ { "pixels", t.counters[kOutsideCounted] },
						{ "matched", t.counters[kOutsideMatched] },
						{ "traced_nearer", t.counters[kOutsideNearer] },
						{ "traced_farther", t.counters[kOutsideFarther] },
						{ "traced_miss", t.counters[kOutsideMiss] },
						{ "hit_distant_lod", t.counters[kOutsideHitLOD] },
						{ "mismatch_percent", t.OutsideMismatchPercent() } } },
				// M8 water: pixels whose camera ray meets a water plane (mask 0x80) in front of the opaque scene.
				{ "water", { { "pixels", t.counters[kWaterPixels] }, { "over_sky", t.counters[kWaterOverSky] },
							   { "percent_of_render", t.counters[kRenderPixels] ? 100.0f * t.counters[kWaterPixels] / t.counters[kRenderPixels] : 0.0f } } },
				{ "timings_ms", { { "blas_builds", TimingJson(t.blasBuildMs) }, { "tlas_build", TimingJson(t.tlasBuildMs) }, { "trace", TimingJson(t.traceMs) } } },
				{ "blas", { { "built", c.blasBuilt }, { "pending", c.blasPending }, { "built_last_frame", c.blasBuiltLastFrame }, { "total_built", c.blasTotalBuilt }, { "failed", c.blasFailed }, { "blas_mb", c.blasBytes / kMB }, { "as_pool_reserved_mb", c.asPoolBytes / kMB } } },
				{ "loaded_area", { { "bounded", t.area.bounded }, { "min_xy", { t.area.min.x, t.area.min.y } }, { "max_xy", { t.area.max.x, t.area.max.y } } } },
				{ "camera_pos_adjust", { t.posAdjust.x, t.posAdjust.y, t.posAdjust.z } },
				{ "images", images },
			};
		}

		json SunShadowsJson(const DebugDumpData& a_data)
		{
			const auto& s = a_data.shadows;
			const auto& c = s.comparedCounters;
			constexpr float kTargetMs = 2.0f;
			json images = json::array();
			for (const auto& image : a_data.images) {
				if (image.name.starts_with("shadow_") || image.name.starts_with("game_shadow") || image.name.starts_with("final"))
					images.push_back(std::format("debug_{}_{}.png", image.name, a_data.gameFrame));
			}
			return {
				{ "traced_dump_frame", a_data.haveShadows },
				{ "have_result", s.haveResult },
				{ "frames_traced", s.framesTraced },
				{ "history_resets", s.historyResets },
				{ "to_sun", { s.toSun[0], s.toSun[1], s.toSun[2] } },
				{ "cone_half_angle_degrees", s.coneHalfAngleDegrees },
				{ "pixels", { { "traced", s.counters[kShadowTraced] }, { "shadowed_raw", s.counters[kShadowShadowed] } } },
				{ "shadowed_percent_raw", s.ShadowedPercent() },
				{ "window", { { "shadowed_percent_raw", TimingJson(s.shadowedPercent) } } },
				{ "vs_game_shadow_mask",
					{ { "compared_dump_frame", s.haveComparison },
						{ "note", "raw RT visibility vs the game's kSHADOW_MASK < 0.5, non-sky pixels closer than compare_distance" },
						{ "compare_distance_units", SunShadows::kCompareDistance },
						{ "compared", c[kCompareCompared] },
						{ "both_lit", c[kCompareBothLit] },
						{ "both_shadowed", c[kCompareBothShadowed] },
						{ "rt_only_shadowed", c[kCompareRtOnly] },
						{ "map_only_shadowed", c[kCompareMapOnly] },
						{ "agreement_percent", s.AgreementPercent() } } },
				{ "timings_ms",
					{ { "trace", TimingJson(s.traceMs) },
						{ "temporal", TimingJson(s.temporalMs) },
						{ "spatial", TimingJson(s.spatialMs) },
						{ "total_shadow_passes", TimingJson(s.totalMs) },
						{ "note", "the frame-time cost is timings_ms.d3d11_round_trip (includes BLAS/TLAS, and the M4 debug trace when enabled)" } } },
				{ "target_ms", kTargetMs },
				{ "round_trip_within_target", a_data.stats.roundTripMs.count > 0 && a_data.stats.roundTripMs.Average() < kTargetMs },
				{ "images", images },
			};
		}

		json PointShadowsJson(const DebugDumpData& a_data)
		{
			const auto& s = a_data.pointShadows;
			const auto& c = s.counters;
			json images = json::array();
			for (const auto& image : a_data.images) {
				if (image.name.starts_with("point_shadow_"))
					images.push_back(std::format("debug_{}_{}.png", image.name, a_data.gameFrame));
			}
			const uint32_t farther = c[kPointOccluded] - c[kPointOccluderNear32] - c[kPointOccluderNear64] - c[kPointOccluderNear128];
			return {
				{ "available", a_data.pointShadowsAvailable },
				{ "traced_dump_frame", a_data.havePointShadows },
				{ "have_result", s.haveResult },
				{ "frames_traced", s.framesTraced },
				{ "history_resets", s.historyResets },
				{ "lights_uploaded", s.pointLights },
			{ "lights_by_kind", { { "traced", s.pointLightsTraced }, { "traced_room_limited", s.pointLightsPortalStrict }, { "shadow_mapped_traced", s.pointLightsShadowMapped } } },
				{ "source_disc_fraction_of_radius", s.pointLightSourceFraction },
				{ "note", "every light but disabled ones is ray-traced (M9: the shadow-mapped ones too, in place of the game's shadow map), portal-strict ones only for pixels in their rooms (primary-ray instance); raw = visibility of one light picked by unshadowed contribution" },
				{ "pixels", { { "traced", c[kPointTraced] }, { "sampled_light", c[kPointSampled] }, { "occluded", c[kPointOccluded] }, { "in_a_lit_room", c[kPointRoomKnown] } } },
				{ "light_filters", { { "note", "pixels with a traced light: within its radius / and facing / and applying in the pixel's room" }, { "in_range", c[kPointAnyInRange] }, { "facing", c[kPointAnyFacing] }, { "in_room", c[kPointAnyInRoom] } } },
				{ "sampled_percent", c[kPointTraced] ? 100.0 * c[kPointSampled] / c[kPointTraced] : 0.0 },
				// M8 ReSTIR phase-2 gate: one visibility ray per pixel stands in for all of these lights.
				{ "lights_per_sampled_pixel", { { "note", "lights SamplePointLight picks from (in range, facing, in the pixel's room)" },
												  { "mean", c[kPointAnyInRoom] ? static_cast<double>(c[kPointCandidateSum]) / c[kPointAnyInRoom] : 0.0 },
												  { "max", c[kPointCandidateMax] }, { "pixels_1", c[kPointCandidates1] }, { "pixels_2_to_3", c[kPointCandidates2to3] },
												  { "pixels_4_to_7", c[kPointCandidates4to7] }, { "pixels_8_plus", c[kPointCandidates8Plus] } } },
				{ "occluded_percent_of_sampled", c[kPointSampled] ? 100.0 * c[kPointOccluded] / c[kPointSampled] : 0.0 },
				{ "occluded_by_distance_to_light", { { "under_32", c[kPointOccluderNear32] }, { "32_to_64", c[kPointOccluderNear64] }, { "64_to_128", c[kPointOccluderNear128] }, { "128_and_over", farther } } },
				{ "timings_ms",
					{ { "trace", TimingJson(s.traceMs) },
						{ "temporal", TimingJson(s.temporalMs) },
						{ "spatial", TimingJson(s.spatialMs) },
						{ "total_passes", TimingJson(s.totalMs) },
						{ "note", "inside the Prepass round trip: timings_ms.d3d11_round_trip is the frame-time cost" } } },
				{ "images", images },
			};
		}

		// TLAS candidates around the camera: names what the traces see (e.g. an occluder the raster doesn't draw).
		json NearbyJson(const DebugDumpData& a_data)
		{
			json objects = json::array();
			for (const auto& o : a_data.nearby) {
				objects.push_back({ { "name", o.name }, { "parents", o.parents }, { "ref", std::format("{:08X}", o.refFormID) },
					{ "base", std::format("{:08X}", o.baseFormID) }, { "base_name", o.baseName },
					{ "flags", std::format("{:08X}", o.flags) }, { "ancestor_flags", std::format("{:08X}", o.ancestorFlags) },
					{ "not_visible", ((o.flags | o.ancestorFlags) & (1u << 20)) != 0 },  // NiAVObject::Flag::kNotVisible
					{ "min_fade", o.minFade }, { "distance", o.distance }, { "offset", { o.offset[0], o.offset[1], o.offset[2] } },
					{ "bound_radius", o.boundRadius }, { "triangles", o.triangles },
					{ "alpha_tested", o.alphaTested }, { "alpha_blended", o.alphaBlended }, { "wind_animated", o.windAnimated },
					{ "skinned", o.skinned }, { "terrain", o.terrain }, { "tree", o.tree },
					{ "material_alpha", o.materialAlpha }, { "render_passes", o.renderPasses }, { "last_render_pass_state", o.lastRenderPassState },
					{ "vertex_alpha", o.vertexAlpha }, { "vertex_colors", o.vertexColors }, { "decal", o.decal } });
			}
			return { { "note", "TLAS candidates within 512 units of the camera, nearest first; offset = bound centre - camera (z up)" },
				{ "objects", objects } };
		}

		json SkinnedJson(const DebugDumpData& a_data)
		{
			const auto& k = a_data.skinned;
			const auto& s = a_data.scene;
			return {
				{ "available", a_data.haveSkinned },
				{ "scene", { { "shapes", s.skinnedShapes }, { "partitions", s.skinnedPartitions }, { "bones", s.skinnedBones }, { "half_position_partitions", s.skinnedHalfPositions },
							   { "rejected_shapes", s.skinnedRejectedShapes }, { "rejected_partitions", s.skinnedRejectedPartitions }, { "invalid_pose_partitions", s.skinnedInvalidPoses }, { "dynamic_shapes", s.dynamicShapes }, { "dynamic_rejected_shapes", s.dynamicRejectedShapes } } },
				{ "dynamic", { { "partitions", k.dynamicPartitions }, { "uploads_last_frame", k.dynamicUploadsLastFrame }, { "upload_kb_last_frame", k.dynamicUploadBytesLastFrame / 1024.0 }, { "waiting", k.dynamicWaiting } } },
				{ "tlas_instances", k.instances },
				{ "skinned_last_frame", k.skinnedLastFrame },
				{ "vertices_last_frame", k.verticesLastFrame },
				{ "waiting_for_mesh", k.waitingForMesh },
				{ "blas", { { "built_last_frame", k.blasBuiltLastFrame }, { "refit_last_frame", k.blasRefitLastFrame }, { "skipped_scratch", k.blasSkippedScratch }, { "duplicate_partitions_skipped", k.duplicatePartitions }, { "total_built", k.totalBuilt }, { "failed", k.failed } } },
				{ "entries", k.entries },
				{ "memory_mb", { { "output", k.outputBytes / (1024.0 * 1024.0) }, { "blas", k.blasBytes / (1024.0 * 1024.0) } } },
				{ "timings_ms", { { "skin_and_blas", TimingJson(k.skinMs) } } },
			};
		}

		json GlobalIlluminationJson(const DebugDumpData& a_data)
		{
			const auto& g = a_data.gi;
			const auto& m = a_data.materials;
			const auto& p = g.params;
			json images = json::array();
			for (const auto& image : a_data.images) {
				if (image.name.starts_with("gi_"))
					images.push_back(std::format("debug_{}_{}.png", image.name, a_data.gameFrame));
			}
			json lights = json::array();
			for (const auto& light : g.lastPointLights) {
				lights.push_back({ { "position", { light.position[0], light.position[1], light.position[2] } }, { "radius", light.radius },
					{ "color", { light.color[0], light.color[1], light.color[2] } }, { "flags", light.flags },
					{ "room_flags", std::format("{:08x}{:08x}{:08x}{:08x}", light.roomFlags[3], light.roomFlags[2], light.roomFlags[1], light.roomFlags[0]) } });
			}
			return {
				{ "compiled_in_nrd", a_data.giCompiledIn },
				{ "available", a_data.giAvailable },
				{ "traced_dump_frame", a_data.haveGI },
				{ "have_result", g.haveResult },
				{ "frames_traced", g.framesTraced },
				{ "history_resets", g.historyResets },
				{ "render_size", { g.renderWidth, g.renderHeight } },
				{ "rays", { { "traced", g.counters[kGITraced] }, { "hit", g.counters[kGIHits] }, { "hit_sunlit", g.counters[kGISunLitHits] },
							  { "hit_point_light_sampled", g.counters[kGILightSampled] }, { "hit_point_light_occluded", g.counters[kGILightOccluded] } } },
				{ "hit_percent", g.HitPercent() },
				{ "sunlit_hit_percent", g.SunLitHitPercent() },
				// M8 multi-bounce: continuation-ray hits per first hit (0 with one bounce; at most bounces - 1).
				{ "multi_bounce", { { "bounces", p.bounces }, { "deeper_hits", g.counters[kGIDeeperHits] },
									  { "deeper_hits_per_first_hit", g.counters[kGIHits] ? static_cast<double>(g.counters[kGIDeeperHits]) / g.counters[kGIHits] : 0.0 } } },
				// M8 sky light: of all traced rays, those that missed and reached the sky (0 unless sky light is on).
				{ "sky_light", { { "enabled", p.skyLight }, { "rays_to_sky", g.counters[kGISkyVisible] },
								   { "rays_to_sky_percent", g.counters[kGITraced] ? 100.0 * g.counters[kGISkyVisible] / g.counters[kGITraced] : 0.0 } } },
				{ "point_lights", { { "count", g.pointLights }, { "dropped", g.pointLightsDropped }, { "sampled_hit_percent", g.LightSampledHitPercent() },
									  { "occluded_percent", g.LightOccludedPercent() }, { "shadows", p.pointLightShadows }, { "inverse_square", p.inverseSquare },
									  { "occluded_by_distance_to_light",
										  { { "under_32", g.counters[kGIOccluderNear32] }, { "32_to_64", g.counters[kGIOccluderNear64] }, { "64_to_128", g.counters[kGIOccluderNear128] },
											  { "128_and_over", g.counters[kGILightOccluded] - g.counters[kGIOccluderNear32] - g.counters[kGIOccluderNear64] - g.counters[kGIOccluderNear128] } } },
									  { "lights_camera_relative", std::move(lights) } } },
				// M8 reflections: one glossy ray per pixel with a reflection term (Dynamic Cubemaps' REFLECTANCE), replacing
				// the composite's cubemap reflection there. Counters are the last collected frame's.
				{ "reflections",
					{ { "enabled", p.reflections },
						{ "available", g.reflectionsAvailable },
						{ "failure", g.reflectionsFailure },
						{ "traced_last_frame", g.reflectionsLastSlot },
						{ "max_roughness", p.reflectionMaxRoughness },
						{ "path_vertices", std::min(p.bounces + 1, 3u) },
						{ "half_resolution", p.reflectionHalfResolution },
						{ "pixels_traced", g.counters[kGIReflectionTraced] },
						{ "pixels_traced_percent", g.renderWidth && g.renderHeight ? 100.0 * g.counters[kGIReflectionTraced] / (static_cast<double>(g.renderWidth) * g.renderHeight) : 0.0 },
						{ "rays", g.counters[kGIReflectionRays] },
						{ "rays_per_reflective_pixel", g.counters[kGIReflectionTraced] ? static_cast<double>(g.counters[kGIReflectionRays]) / g.counters[kGIReflectionTraced] : 0.0 },
						{ "hit_percent", g.ReflectionHitPercent() },
						{ "deeper_hits", g.counters[kGIReflectionDeeperHits] },
						// M8 water: pixels whose reflecting surface is a water plane (part of pixels_traced), for Water.hlsl t47.
						{ "water", { { "enabled", p.water }, { "roughness", p.waterRoughness }, { "pixels", g.counters[kGIWaterPixels] },
									   { "percent_of_render", g.renderWidth && g.renderHeight ? 100.0 * g.counters[kGIWaterPixels] / (static_cast<double>(g.renderWidth) * g.renderHeight) : 0.0 } } },
						// M8 water diagnostic: StaticMotionVector against the game's motion vectors on the G-buffer, frames with camera motion only.
						{ "motion_vector_check_px", { { "game_mean", TimingJson(g.motionGamePx) }, { "error", TimingJson(g.motionErrorPx) },
													  { "error_if_y_flipped", TimingJson(g.motionErrorFlipYPx) }, { "error_if_negated", TimingJson(g.motionErrorNegatedPx) } } },
						{ "camera_matrix_check_relative", { { "proj_times_view_vs_game", g.cameraCheckProjTimesView }, { "view_times_proj_vs_game", g.cameraCheckViewTimesProj },
															{ "our_previous_vs_game", g.cameraCheckPrevious },
															{ "projT_times_view", g.cameraCheckVariants[0] }, { "proj_times_viewT", g.cameraCheckVariants[1] },
															{ "projT_times_viewT", g.cameraCheckVariants[2] }, { "viewT_times_projT", g.cameraCheckVariants[3] } } },
						// The raw camera matrices of the last traced frame, 16 floats in memory order (row by row as CS stores them).
						{ "camera_matrices", { { "view", g.cameraMatrices[0] }, { "proj_unjittered", g.cameraMatrices[1] }, { "view_proj_unjittered", g.cameraMatrices[2] },
												 { "view_proj", g.cameraMatrices[3] }, { "view_inverse", g.cameraMatrices[4] }, { "prev_view_proj_unjittered", g.cameraMatrices[5] },
												 { "pos_adjust", { g.cameraPosAdjust.x, g.cameraPosAdjust.y, g.cameraPosAdjust.z } },
												 { "prev_pos_adjust", { g.cameraPrevPosAdjust.x, g.cameraPrevPosAdjust.y, g.cameraPrevPosAdjust.z } } } },
						{ "frames_traced", g.reflectionFramesTraced },
						{ "history_resets", g.reflectionHistoryResets },
						{ "nrd_dispatches", g.reflectionDispatches } } },
				{ "nrd_dispatches", g.nrdDispatches },
				{ "params",
					{ { "to_sun", { p.toSun[0], p.toSun[1], p.toSun[2] } },
						{ "sun_color", { p.sunColor[0], p.sunColor[1], p.sunColor[2] } },
						{ "ambient_sh_l0_rgb", { p.ambientSH[0][0], p.ambientSH[1][0], p.ambientSH[2][0] } },
						{ "linear_lighting", p.linearLighting },
						{ "intensity", p.intensity },
						{ "ao_strength", p.aoStrength },
						{ "ray_length_units", p.rayLength },
						{ "alpha_tested_casters", p.alphaTestedCasters },
						{ "max_accumulated_frames", p.maxAccumulatedFrames },
						{ "interior", p.interior } } },
				{ "material_table",
					{ { "textures", m.textures },
						{ "pending", m.pending },
						{ "unsupported", m.unsupported },
						{ "computed_last_frame", m.computedLastFrame },
						{ "candidates_without_texture", m.candidatesWithoutTexture },
						{ "candidates_defaulted", m.candidatesDefaulted } } },
				{ "timings_ms",
					{ { "trace", TimingJson(g.traceMs) },
						{ "nrd_reblur", TimingJson(g.denoiseMs) },
						{ "reflection_trace", TimingJson(g.reflectionTraceMs) },
						{ "reflection_nrd_reblur_specular", TimingJson(g.reflectionDenoiseMs) },
						{ "resolve", TimingJson(g.resolveMs) },
						{ "total_gi_passes", TimingJson(g.totalMs) },
						{ "d3d11_gi_hand_off", TimingJson(g.roundTripMs) },
						{ "note", "the GI frame-time cost is d3d11_gi_hand_off (a second D3D11 <-> D3D12 round trip)" } } },
				{ "images", images },
			};
		}

		// M8 materials at GI hits.
		json AlbedoAtlasJson(const DebugDumpData& a_data)
		{
			const auto& a = a_data.albedoAtlas;
			const auto& g = a_data.gi;
			json images = json::array();
			for (const auto& image : a_data.images) {
				if (image.name == "albedo_atlas")
					images.push_back(std::format("debug_{}_{}.png", image.name, a_data.gameFrame));
			}
			return {
				{ "available", a.available },
				{ "enabled", a.enabled },
				{ "tile_size", AlbedoAtlas::kTileSize },
				{ "tiles", { { "capacity", a.capacity }, { "used", a.tilesUsed }, { "filled_last_frame", a.filledLastFrame }, { "evicted_last_frame", a.evictedLastFrame }, { "total_fills", a.totalFills }, { "total_evictions", a.totalEvictions } } },
				{ "traced_candidates",
					{ { "total", a.candidates }, { "textured", a.candidatesTextured }, { "with_vertex_colors", a.candidatesVertexColors },
						{ "average_no_texture", a.candidatesNoTexture }, { "average_no_uv", a.candidatesNoUV }, { "average_unsupported_texture", a.candidatesUnsupported },
						{ "average_waiting_for_tile", a.candidatesWaiting } } },
				{ "gi_hits_textured_percent", g.counters[kGIHits] ? 100.0 * g.counters[kGITexturedHits] / g.counters[kGIHits] : 0.0 },
				{ "images", images },
			};
		}

		json AlphaAtlasJson(const DebugDumpData& a_data)
		{
			const auto& a = a_data.alphaAtlas;
			json images = json::array();
			for (const auto& image : a_data.images) {
				if (image.name == "alpha_atlas")
					images.push_back(std::format("debug_{}_{}.png", image.name, a_data.gameFrame));
			}
			return {
				{ "available", a.available },
				{ "enabled", a.enabled },
				{ "tile_size", AlphaAtlas::kTileSize },
				{ "tiles", { { "capacity", a.capacity }, { "used", a.tilesUsed }, { "filled_last_frame", a.filledLastFrame }, { "evicted_last_frame", a.evictedLastFrame }, { "total_fills", a.totalFills }, { "total_evictions", a.totalEvictions } } },
				{ "alpha_tested_candidates",
					{ { "total", a.candidates },
						{ "alpha_tested_in_traces", a.candidatesTested },
						{ "opaque_no_texture", a.candidatesNoTexture },
						{ "opaque_no_uv", a.candidatesNoUV },
						{ "opaque_zero_threshold", a.candidatesZeroThreshold },
						{ "opaque_unsupported_texture", a.candidatesUnsupported },
						{ "opaque_waiting_for_tile", a.candidatesWaiting } } },
				{ "images", images },
			};
		}

		json BuildJson(const DebugDumpData& a_data, const std::string& a_pngName, bool a_pngWritten)
		{
			const auto& s = a_data.stats;
			const auto& sp = a_data.spike;
			const float cost = s.roundTripMs.Average();
			const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

			return {
				{ "milestone", "M7" },
				{ "alpha_atlas", AlphaAtlasJson(a_data) },
				{ "albedo_atlas", AlbedoAtlasJson(a_data) },
				{ "skinned", SkinnedJson(a_data) },
				{ "global_illumination", GlobalIlluminationJson(a_data) },
				{ "sun_shadows", SunShadowsJson(a_data) },
				{ "point_light_shadows", PointShadowsJson(a_data) },
				{ "trace", TraceJson(a_data) },
				{ "scene", SceneJson(a_data) },
				{ "nearby_objects", NearbyJson(a_data) },
				{ "mesh_cache", CacheJson(a_data.cache) },
				{ "frame", a_data.gameFrame },
				{ "written_utc", std::format("{:%FT%TZ}", now) },
				{ "adapter", { { "name", a_data.caps.adapterName }, { "luid", FormatLuid(a_data.caps.adapterLuid) }, { "dxr_tier", GetTierName(a_data.caps.raytracingTier) } } },
				{ "frames", { { "submitted", s.framesSubmitted }, { "skipped_slot_busy", s.framesSkipped }, { "last_signaled_fence", s.lastSignaledFenceValue }, { "last_completed_fence", s.lastCompletedFenceValue } } },
				{ "timings_ms", { { "d3d11_round_trip", TimingJson(s.roundTripMs) }, { "d3d12_dispatch", TimingJson(s.d3d12DispatchMs) }, { "cpu_submit", TimingJson(s.cpuSubmitMs) }, { "frame_present_to_present", TimingJson(s.frameMs) } } },
				{ "cost_ms", cost },
				{ "budget_ms", kInteropBudgetMs },
				{ "within_budget", s.roundTripMs.count > 0 && cost < kInteropBudgetMs },
				{ "device_removed", { { "any", s.deviceRemoved }, { "d3d12", FormatHResult(s.d3d12RemovedReason) }, { "d3d11", FormatHResult(s.d3d11RemovedReason) } } },
				{ "interop_spike",
					{ { "ran", sp.ran },
						{ "texture_d3d12_to_d3d11", FormatHResult(sp.textureD3D12ToD3D11) },
						{ "texture_d3d11_to_d3d12", FormatHResult(sp.textureD3D11ToD3D12) },
						{ "texture_created_in", sp.textureCreatedInD3D12 ? "D3D12" : "D3D11" },
						{ "buffer_d3d12_to_d3d11_open", FormatHResult(sp.bufferD3D12ToD3D11Open) },
						{ "buffer_d3d12_to_d3d11_data_verified", sp.bufferD3D12ToD3D11Verified },
						{ "buffer_d3d12_to_d3d11_mismatches", sp.bufferMismatches },
						{ "buffer_d3d11_create_shared", FormatHResult(sp.bufferD3D11Create) },
						{ "buffer_d3d11_to_d3d12_open", FormatHResult(sp.bufferD3D11ToD3D12Open) } } },
				{ "test_pattern",
					{ { "width", a_data.width },
						{ "height", a_data.height },
						{ "pattern_frame", a_data.patternFrame },
						{ "verified_this_frame", a_data.patternVerified },
						{ "png", a_pngWritten ? a_pngName : "" } } },
			};
		}
	}

	void WriteDebugDumpAsync(DebugDumpData a_data)
	{
		std::thread([data = std::move(a_data)]() {
			const auto dir = GetDumpDirectory();
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			if (ec) {
				logger::error("[SkyrimRT] Debug dump: cannot create {}: {}", dir.string(), ec.message());
				return;
			}

			// WIC needs COM on this thread.
			const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

			const auto pngName = std::format("debug_testpattern_{}.png", data.gameFrame);
			DirectX::Image image{};
			image.width = data.width;
			image.height = data.height;
			image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			image.rowPitch = static_cast<size_t>(data.width) * 4;
			image.slicePitch = image.rowPitch * data.height;
			image.pixels = const_cast<uint8_t*>(data.pixels.data());
			const auto pngPath = dir / pngName;
			const HRESULT pngHr = DirectX::SaveToWICFile(image, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), pngPath.c_str());
			if (FAILED(pngHr))
				logger::error("[SkyrimRT] Debug dump: writing {} failed ({})", pngPath.string(), FormatHResult(pngHr));

			for (const auto& debugImage : data.images) {
				DirectX::Image view{};
				view.width = debugImage.width;
				view.height = debugImage.height;
				view.format = debugImage.format;
				view.rowPitch = static_cast<size_t>(debugImage.width) * (DirectX::BitsPerPixel(debugImage.format) / 8);
				view.slicePitch = view.rowPitch * debugImage.height;
				view.pixels = const_cast<uint8_t*>(debugImage.pixels.data());
				const auto viewPath = dir / std::format("debug_{}_{}.png", debugImage.name, data.gameFrame);

				// Captured framebuffers may be BGRA8, 10-bit or float; PNG gets RGBA8 (float is clamped, not tonemapped).
				// Their alpha channel is meaningless (often 0), so it's forced opaque.
				const bool framebuffer = debugImage.name.starts_with("final");
				DirectX::ScratchImage converted;
				const DirectX::Image* toSave = &view;
				if (view.format != DXGI_FORMAT_R8G8B8A8_UNORM || framebuffer) {
					const HRESULT hr = view.format != DXGI_FORMAT_R8G8B8A8_UNORM ?
					                       DirectX::Convert(view, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted) :
					                       converted.InitializeFromImage(view);
					if (FAILED(hr)) {
						logger::error("[SkyrimRT] Debug dump: converting {} (format {}) failed ({})", debugImage.name, static_cast<uint32_t>(view.format), FormatHResult(hr));
						continue;
					}
					if (framebuffer) {
						uint8_t* pixels = converted.GetPixels();
						for (size_t i = 3; i < converted.GetPixelsSize(); i += 4)
							pixels[i] = 255;
					}
					toSave = converted.GetImage(0, 0, 0);
				}
				if (const HRESULT hr = DirectX::SaveToWICFile(*toSave, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), viewPath.c_str()); FAILED(hr))
					logger::error("[SkyrimRT] Debug dump: writing {} failed ({})", viewPath.string(), FormatHResult(hr));
			}

			if (SUCCEEDED(comHr))
				CoUninitialize();

			const auto jsonPath = dir / std::format("frame_{}.json", data.gameFrame);
			std::ofstream out(jsonPath);
			if (!out) {
				logger::error("[SkyrimRT] Debug dump: cannot open {}", jsonPath.string());
				return;
			}
			out << BuildJson(data, pngName, SUCCEEDED(pngHr)).dump(2) << '\n';
			logger::info("[SkyrimRT] Debug dump written: {} (pattern verified: {})", jsonPath.string(), data.patternVerified);
		}).detach();
	}
}
