#include "entity_scene_streaming.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

namespace {

struct CellCandidate {
	EntityScene::CellKey cell;
	double distance = 0.0;
};

struct CellCandidateSorter {
	bool operator()(const CellCandidate &p_a, const CellCandidate &p_b) const {
		return p_a.distance < p_b.distance;
	}
};

double camera_distance(const AABB &p_aabb, const Vector<Vector3> &p_cameras) {
	const Vector3 begin = p_aabb.position;
	const Vector3 end = p_aabb.position + p_aabb.size;
	double best = 0.0;
	bool found = false;
	for (const Vector3 &camera : p_cameras) {
		const Vector3 nearest(CLAMP(camera.x, begin.x, end.x), CLAMP(camera.y, begin.y, end.y), CLAMP(camera.z, begin.z, end.z));
		const double distance = double((nearest - camera).length());
		if (!found || distance < best) {
			best = distance;
			found = true;
		}
	}
	return best;
}

void report_step(EntityScene &p_scene, const EntitySceneStreaming::Stats &p_stats) {
	const EntityScene::Stats &load = p_stats.load;
	if (!OS::get_singleton()->is_use_benchmark_set()) {
		return;
	}
	if (!load.jobs_dispatched && !load.jobs_completed && !load.jobs_discarded && !load.jobs_resumed && !load.cells_committed && !p_stats.cells_released && !p_stats.entities_unloaded) {
		return;
	}
	const double to_ms = 1.0 / 1000.0;
	String message = vformat("EntityScene streaming: jobs_dispatched=%d jobs_completed=%d jobs_discarded=%d jobs_resumed=%d cells_committed=%d cells_released=%d cells_remaining=%d entities_committed=%d entities_unloaded=%d dispatch=%.2fms",
			load.jobs_dispatched,
			load.jobs_completed,
			load.jobs_discarded,
			load.jobs_resumed,
			load.cells_committed,
			p_stats.cells_released,
			p_stats.cells_remaining,
			load.entities_committed,
			p_stats.entities_unloaded,
			double(load.dispatch_usec) * to_ms);
	message += vformat(" owner_total=%.2fms owner_phases(asset_load=%.2fms revalidate=%.2fms commit_validate=%.2fms install=%.2fms residency=%.2fms job_scan_cleanup=%.2fms)",
			double(load.owner_total_usec) * to_ms,
			double(load.owner_asset_load_usec) * to_ms,
			double(load.owner_revalidate_usec) * to_ms,
			double(load.owner_commit_validate_usec) * to_ms,
			double(load.owner_install_usec) * to_ms,
			double(load.owner_residency_usec) * to_ms,
			double(load.owner_job_scan_cleanup_usec) * to_ms);
	message += vformat(" worker_total=%.2fms worker_phases(parse=%.2fms decode=%.2fms remainder=%.2fms)",
			double(load.worker_total_usec) * to_ms,
			double(load.parse_usec) * to_ms,
			double(load.decode_usec) * to_ms,
			double(load.worker_remainder_usec) * to_ms);
	message += vformat(" install_detail(required_catalog=%.2fms ecs_parent_remove=%.2fms ecs_entity_destroy=%.2fms ecs_entity_create_identity=%.2fms ecs_materialize_parent_set=%.2fms resident_remove=%.2fms resident_insert=%.2fms initial_dirty=%.2fms prepared_schema_lookup=%.2fms component_mutation=%.2fms component_changed=%.2fms change_bookkeeping=%.2fms sections_order=%.2fms catalog_parent=%.2fms ecs_final_parent_set=%.2fms assign_cell=%.2fms remainder=%.2fms)",
			double(load.install_required_catalog_usec) * to_ms,
			double(load.install_ecs_parent_remove_usec) * to_ms,
			double(load.install_ecs_entity_destroy_usec) * to_ms,
			double(load.install_ecs_entity_create_identity_usec) * to_ms,
			double(load.install_ecs_materialize_parent_set_usec) * to_ms,
			double(load.install_resident_remove_usec) * to_ms,
			double(load.install_resident_insert_usec) * to_ms,
			double(load.install_initial_dirty_usec) * to_ms,
			double(load.install_prepared_schema_lookup_usec) * to_ms,
			double(load.install_component_mutation_usec) * to_ms,
			double(load.install_component_changed_usec) * to_ms,
			double(load.install_change_bookkeeping_usec) * to_ms,
			double(load.install_sections_order_usec) * to_ms,
			double(load.install_catalog_parent_usec) * to_ms,
			double(load.install_ecs_final_parent_set_usec) * to_ms,
			double(load.install_assign_cell_usec) * to_ms,
			double(load.install_remainder_usec) * to_ms);
	message += vformat(" operations(materialized=%d destroyed=%d parent_removals=%d materialize_parent_sets=%d component_mutations=%d final_parent_sets=%d) resident_cells=%d resident_entities=%d",
			load.entities_materialized,
			load.entities_destroyed,
			load.parent_removals,
			load.materialize_parent_sets,
			load.component_mutations,
			load.final_parent_sets,
			p_scene.get_resident_cell_count(),
			p_scene.get_resident_count());
	print_line(message);
}

} // namespace

Error EntitySceneStreaming::step(EntityScene &p_scene, const Vector<Vector3> &p_cameras, int p_budget, Stats *r_stats) {
	Stats stats;
	Error result = p_scene.commit_ready(p_budget, &stats.load);
	Vector<EntityScene::CellKey> stale;
	for (const EntityScene::CellKey &cell : p_scene.get_resident_cells()) {
		const double size = p_scene.get_grid_size(cell.grid);
		const double range = p_scene.get_grid_range(cell.grid);
		if (size <= 0.0) {
			continue;
		}
		if (p_cameras.is_empty() || camera_distance(p_scene.cell_aabb(cell), p_cameras) > range + size) {
			stale.push_back(cell);
		}
	}
	Vector<CellCandidate> wanted;
	if (result == OK && !p_cameras.is_empty()) {
		int probes = PROBE_BUDGET;
		HashSet<EntityScene::CellKey, EntityScene::CellKeyHasher> visited;
		for (const String &grid : p_scene.get_grid_names()) {
			const double size = p_scene.get_grid_size(grid);
			const double range = p_scene.get_grid_range(grid);
			if (size <= 0.0 || range <= 0.0) {
				continue;
			}
			for (const Vector3 &camera : p_cameras) {
				const Vector3 reach(range, range, range);
				const EntityScene::CellKey low = p_scene.cell_for_position(grid, camera - reach);
				const EntityScene::CellKey high = p_scene.cell_for_position(grid, camera + reach);
				EntityScene::CellKey cell;
				cell.grid = grid;
				for (int64_t x = low.x; x <= high.x; x++) {
					cell.x = int32_t(x);
					for (int64_t y = low.y; y <= high.y; y++) {
						cell.y = int32_t(y);
						for (int64_t z = low.z; z <= high.z; z++) {
							cell.z = int32_t(z);
							if (visited.has(cell)) {
								continue;
							}
							visited.insert(cell);
							if (p_scene.is_cell_resident(cell)) {
								continue;
							}
							const double distance = camera_distance(p_scene.cell_aabb(cell), p_cameras);
							if (distance > range || !p_scene.cell_exists(cell, &probes)) {
								continue;
							}
							wanted.push_back({ cell, distance });
						}
					}
				}
			}
		}
	}
	if (result == OK && !wanted.is_empty()) {
		wanted.sort_custom<CellCandidateSorter>();
		Vector<EntityScene::CellKey> request;
		request.reserve(wanted.size());
		for (const CellCandidate &candidate : wanted) {
			request.push_back(candidate.cell);
		}
		result = p_scene.request_cells(request, &stats.cells_remaining, &stats.load);
	}
	if (result == OK && !stale.is_empty()) {
		const int resident = p_scene.get_resident_count();
		result = p_scene.release_cells(stale);
		stats.entities_unloaded = resident - p_scene.get_resident_count();
		for (const EntityScene::CellKey &cell : stale) {
			if (!p_scene.is_cell_resident(cell)) {
				stats.cells_released++;
			}
		}
	}
	if (r_stats) {
		*r_stats = stats;
	}
	report_step(p_scene, stats);
	return result;
}
