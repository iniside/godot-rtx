#pragma once

#include "entity_components.h"

#include "core/math/transform_interpolator.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

class EntityWorld;

class EntityTransformSystem {
	friend class EntityWorld;
	struct State {
		bool initialized = false;
		uint64_t reset_revision = 0;
		TransformInterpolator::Method interpolation_method = TransformInterpolator::INTERP_LERP;
	};
	EntityWorld &world;
	HashSet<uint64_t> dirty;
	HashSet<uint64_t> moving;
	HashSet<uint64_t> render_dirty;
	HashSet<uint64_t> reset;
	uint64_t reset_serial = 0;

	void _collect_descendants(uint64_t p_entity, HashSet<uint64_t> &r_entities);
	bool _update_entity(uint64_t p_entity, State &r_state, bool p_initial);

public:
	explicit EntityTransformSystem(EntityWorld &p_world);
	void mark_dirty(uint64_t p_entity);
	void forget(uint64_t p_entity);
	void begin_tick();
	void update();
	Error finalize_created(const LocalVector<uint64_t> &p_ordered);
	void interpolate(double p_fraction);
	void teleport(uint64_t p_entity);
	uint64_t get_reset_revision(uint64_t p_entity) const;
	static EntityPose compose(const EntityPose &p_parent, const EntityPose &p_local);
	static Error relative_to(const EntityPose &p_world, const EntityPose &p_parent, EntityPose &r_local);
};
