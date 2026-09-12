#include "entity_scene_streaming.h"

#include "core/math/math_funcs.h"
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

int32_t cell_index(double p_value, double p_size) {
	const double index = Math::floor(p_value / p_size);
	return int32_t(CLAMP(index, double(INT32_MIN), double(INT32_MAX)));
}

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
	if (!load.jobs_dispatched && !load.jobs_completed && !load.jobs_discarded && !load.cells_committed && !p_stats.cells_released && !p_stats.entities_unloaded) {
		return;
	}
	const double to_ms = 1.0 / 1000.0;
	print_line(vformat("EntityScene streaming: jobs_dispatched=%d jobs_completed=%d jobs_discarded=%d cells_committed=%d cells_released=%d cells_remaining=%d entities_committed=%d entities_unloaded=%d dispatch=%.2fms commit=%.2fms worker=%.2fms resident_cells=%d resident_entities=%d",
			load.jobs_dispatched,
			load.jobs_completed,
			load.jobs_discarded,
			load.cells_committed,
			p_stats.cells_released,
			p_stats.cells_remaining,
			load.entities_committed,
			p_stats.entities_unloaded,
			double(load.dispatch_usec) * to_ms,
			double(load.commit_usec) * to_ms,
			double(load.worker_usec) * to_ms,
			p_scene.get_resident_cell_count(),
			p_scene.get_resident_count()));
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
				EntityScene::CellKey cell;
				cell.grid = grid;
				const int32_t min_x = cell_index(double(camera.x) - range, size);
				const int32_t max_x = cell_index(double(camera.x) + range, size);
				const int32_t min_y = cell_index(double(camera.y) - range, size);
				const int32_t max_y = cell_index(double(camera.y) + range, size);
				const int32_t min_z = cell_index(double(camera.z) - range, size);
				const int32_t max_z = cell_index(double(camera.z) + range, size);
				for (int64_t x = min_x; x <= max_x; x++) {
					cell.x = int32_t(x);
					for (int64_t y = min_y; y <= max_y; y++) {
						cell.y = int32_t(y);
						for (int64_t z = min_z; z <= max_z; z++) {
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
