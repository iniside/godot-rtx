#include "entity_transform_system.h"

#include "entity_world.h"

static bool entity_pose_equal(const EntityPose &p_a, const EntityPose &p_b) {
	return p_a.translation.x == p_b.translation.x && p_a.translation.y == p_b.translation.y && p_a.translation.z == p_b.translation.z && p_a.basis == p_b.basis;
}

EntityTransformSystem::EntityTransformSystem(EntityWorld &p_world) :
		world(p_world) {
	world.ecs.component<State>();
}

EntityPose EntityTransformSystem::compose(const EntityPose &p_parent, const EntityPose &p_local) {
	const Basis &b = p_parent.basis;
	const EntityPosition &t = p_local.translation;
	return {
		{ p_parent.translation.x + double(b[0][0]) * t.x + double(b[0][1]) * t.y + double(b[0][2]) * t.z,
				p_parent.translation.y + double(b[1][0]) * t.x + double(b[1][1]) * t.y + double(b[1][2]) * t.z,
				p_parent.translation.z + double(b[2][0]) * t.x + double(b[2][1]) * t.y + double(b[2][2]) * t.z },
		p_parent.basis * p_local.basis
	};
}

Error EntityTransformSystem::relative_to(const EntityPose &p_world, const EntityPose &p_parent, EntityPose &r_local) {
	ERR_FAIL_COND_V(p_parent.basis.determinant() == 0, ERR_INVALID_PARAMETER);
	Basis inverse = p_parent.basis.inverse();
	EntityPose delta = p_world;
	delta.translation.x -= p_parent.translation.x;
	delta.translation.y -= p_parent.translation.y;
	delta.translation.z -= p_parent.translation.z;
	r_local = compose({ {}, inverse }, delta);
	return OK;
}

void EntityTransformSystem::mark_dirty(uint64_t p_entity) {
	dirty.insert(p_entity);
}

void EntityTransformSystem::_collect_descendants(uint64_t p_entity, HashSet<uint64_t> &r_entities) {
	Vector<uint64_t> pending;
	pending.push_back(p_entity);
	while (!pending.is_empty()) {
		uint64_t entity = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		if (r_entities.has(entity)) {
			continue;
		}
		r_entities.insert(entity);
		ecs_iter_t children = ecs_children(world.ecs.c_ptr(), entity);
		while (ecs_children_next(&children)) {
			for (int i = 0; i < children.count; i++) {
				pending.push_back(children.entities[i]);
			}
		}
	}
}

void EntityTransformSystem::forget(uint64_t p_entity) {
	dirty.erase(p_entity);
	moving.erase(p_entity);
	render_dirty.erase(p_entity);
	reset.erase(p_entity);
}

void EntityTransformSystem::begin_tick() {
	update();
	for (uint64_t id : moving) {
		EntityTransform *transform = world.ecs.entity(id).try_get_mut<EntityTransform>();
		if (transform) {
			transform->previous = transform->current;
			render_dirty.insert(id);
		}
	}
	moving.clear();
}

void EntityTransformSystem::update() {
	HashSet<uint64_t> affected;
	for (uint64_t id : dirty) {
		if (ecs_is_alive(world.ecs.c_ptr(), id)) {
			_collect_descendants(id, affected);
		}
	}
	dirty.clear();
	struct Entry {
		uint64_t entity = 0;
		int depth = 0;
		bool operator<(const Entry &p_other) const { return depth < p_other.depth; }
	};
	Vector<Entry> ordered;
	ordered.reserve(affected.size());
	for (uint64_t id : affected) {
		int depth = 0;
		for (uint64_t parent = ecs_get_parent(world.ecs.c_ptr(), id); parent; parent = ecs_get_parent(world.ecs.c_ptr(), parent)) {
			depth++;
		}
		ordered.push_back({ id, depth });
	}
	ordered.sort();
	for (const Entry &entry : ordered) {
		flecs::entity entity = world.ecs.entity(entry.entity);
		State &state = entity.ensure<State>();
		EntityTransform *transform = entity.try_get_mut<EntityTransform>();
		bool changed = false;
		if (transform) {
			EntityPose pose = transform->local;
			for (uint64_t parent = ecs_get_parent(world.ecs.c_ptr(), entry.entity); parent; parent = ecs_get_parent(world.ecs.c_ptr(), parent)) {
				const EntityTransform *parent_transform = world.ecs.entity(parent).try_get<EntityTransform>();
				if (parent_transform) {
					pose = compose(parent_transform->current, pose);
					break;
				}
			}
			changed = !state.initialized || !entity_pose_equal(pose, transform->current);
			transform->current = pose;
			if (!state.initialized || reset.has(entry.entity)) {
				transform->previous = pose;
				transform->render = pose;
				state.reset_revision = ++reset_serial;
				state.initialized = true;
				moving.erase(entry.entity);
				changed = true;
			} else if (!entity_pose_equal(transform->previous, pose)) {
				state.interpolation_method = TransformInterpolator::find_method(transform->previous.basis, pose.basis);
				moving.insert(entry.entity);
			}
			if (changed) {
				render_dirty.insert(entry.entity);
			}
		} else {
			state.initialized = false;
		}
		EntityVisibility *visibility = entity.try_get_mut<EntityVisibility>();
		if (visibility) {
			bool effective = visibility->visible;
			uint64_t parent = ecs_get_parent(world.ecs.c_ptr(), entry.entity);
			if (visibility->inherit_parent && parent) {
				const EntityVisibility *parent_visibility = world.ecs.entity(parent).try_get<EntityVisibility>();
				if (parent_visibility) {
					effective &= parent_visibility->effective;
				}
			}
			changed |= effective != visibility->effective;
			visibility->effective = effective;
		}
		if (changed) {
			world._mark_changed(world.get_id({ world.generation, entry.entity }));
		}
	}
	reset.clear();
}

void EntityTransformSystem::interpolate(double p_fraction) {
	update();
	for (uint64_t id : moving) {
		render_dirty.insert(id);
	}
	double fraction = CLAMP(p_fraction, 0.0, 1.0);
	for (uint64_t id : render_dirty) {
		EntityTransform *transform = world.ecs.entity(id).try_get_mut<EntityTransform>();
		if (!transform) {
			continue;
		}
		const EntityPose &a = transform->previous;
		const EntityPose &b = transform->current;
		EntityPose pose{
			{ a.translation.x + (b.translation.x - a.translation.x) * fraction,
					a.translation.y + (b.translation.y - a.translation.y) * fraction,
					a.translation.z + (b.translation.z - a.translation.z) * fraction },
			b.basis
		};
		if (fraction == 0.0) {
			pose.basis = a.basis;
		} else if (fraction < 1.0 && a.basis != b.basis) {
			const State &state = world.ecs.entity(id).get<State>();
			TransformInterpolator::interpolate_basis_via_method(a.basis, b.basis, pose.basis, fraction, state.interpolation_method);
		}
		if (!entity_pose_equal(transform->render, pose)) {
			transform->render = pose;
			world._mark_changed(world.get_id({ world.generation, id }));
		}
	}
	render_dirty.clear();
}

void EntityTransformSystem::teleport(uint64_t p_entity) {
	_collect_descendants(p_entity, reset);
	mark_dirty(p_entity);
}

uint64_t EntityTransformSystem::get_reset_revision(uint64_t p_entity) const {
	const State *state = world.ecs.entity(p_entity).try_get<State>();
	return state ? state->reset_revision : 0;
}
