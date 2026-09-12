#include "entity_scene_streaming.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

namespace {

struct GridBounds {
	double size = 0.0;
	double range = 0.0;
};

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

} // namespace

Error EntitySceneStreaming::step(EntityScene &p_scene, const Vector<Vector3> &p_cameras, int p_budget, Stats *r_stats) {
	Stats stats;
	if (r_stats) {
		*r_stats = stats;
	}
	if (p_cameras.is_empty()) {
		return OK;
	}
	const Vector<EntityScene::CellKey> keys = p_scene.get_cells();
	if (keys.is_empty()) {
		return OK;
	}
	HashMap<String, GridBounds> bounds;
	for (const String &grid : p_scene.get_grid_names()) {
		bounds.insert(grid, { p_scene.get_grid_size(grid), p_scene.get_grid_range(grid) });
	}
	Vector<CellCandidate> wanted;
	Vector<EntityScene::CellKey> stale;
	for (const EntityScene::CellKey &cell : keys) {
		const GridBounds *grid = bounds.getptr(cell.grid);
		if (!grid || grid->size <= 0.0) {
			continue;
		}
		const double distance = camera_distance(p_scene.cell_aabb(cell), p_cameras);
		if (p_scene.is_cell_resident(cell)) {
			if (distance > grid->range + grid->size) {
				stale.push_back(cell);
			}
		} else if (distance <= grid->range) {
			wanted.push_back({ cell, distance });
		}
	}
	Error result = OK;
	if (!wanted.is_empty()) {
		wanted.sort_custom<CellCandidateSorter>();
		Vector<EntityScene::CellKey> request;
		request.reserve(wanted.size());
		for (const CellCandidate &candidate : wanted) {
			request.push_back(candidate.cell);
		}
		const int resident = p_scene.get_resident_count();
		result = p_scene.request_cells(request, p_budget, &stats.cells_remaining);
		stats.entities_loaded = p_scene.get_resident_count() - resident;
		for (const EntityScene::CellKey &cell : request) {
			if (p_scene.is_cell_resident(cell)) {
				stats.cells_requested++;
			}
		}
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
	if (OS::get_singleton()->is_use_benchmark_set() && (stats.cells_requested || stats.cells_released || stats.entities_loaded || stats.entities_unloaded)) {
		print_line(vformat("EntityScene streaming: cells_requested=%d cells_released=%d cells_remaining=%d entities_loaded=%d entities_unloaded=%d resident_cells=%d resident_entities=%d",
				stats.cells_requested,
				stats.cells_released,
				stats.cells_remaining,
				stats.entities_loaded,
				stats.entities_unloaded,
				p_scene.get_resident_cells().size(),
				p_scene.get_resident_count()));
	}
	return result;
}
