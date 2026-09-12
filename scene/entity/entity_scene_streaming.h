#pragma once

#include "scene/resources/entity_scene.h"

class EntitySceneStreaming {
public:
	static constexpr int DEFAULT_BUDGET = 256;

	struct Stats {
		int cells_requested = 0;
		int cells_released = 0;
		int cells_remaining = 0;
		int entities_loaded = 0;
		int entities_unloaded = 0;
	};

	static Error step(EntityScene &p_scene, const Vector<Vector3> &p_cameras, int p_budget = DEFAULT_BUDGET, Stats *r_stats = nullptr);
};
