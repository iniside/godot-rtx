#include "entity_render_system.h"

#include "entity_world.h"

#include "servers/rendering/rendering_server.h"

uint32_t EntityRenderSystem::component_mask(uint64_t p_component) {
	if (!p_component || p_component == EntityComponentTraits<EntityTransform>::id || p_component == EntityComponentTraits<EntityVisibility>::id) {
		return EntityRenderUpdate::POSE;
	}
	if (p_component == EntityComponentTraits<EntityMesh>::id) {
		return EntityRenderUpdate::MESH;
	}
	if (p_component == EntityComponentTraits<EntityGeometry>::id) {
		return EntityRenderUpdate::GEOMETRY;
	}
	if (p_component == EntityComponentTraits<EntityCamera>::id) {
		return EntityRenderUpdate::CAMERA;
	}
	if (p_component == EntityComponentTraits<EntityLight>::id) {
		return EntityRenderUpdate::LIGHT;
	}
	if (p_component == EntityComponentTraits<EntityEnvironment>::id) {
		return EntityRenderUpdate::ENVIRONMENT;
	}
	if (p_component == EntityComponentTraits<EntityMultiMesh>::id) {
		return EntityRenderUpdate::MULTIMESH;
	}
	if (p_component == EntityComponentTraits<EntityDecal>::id) {
		return EntityRenderUpdate::DECAL;
	}
	if (p_component == EntityComponentTraits<EntityFogVolume>::id) {
		return EntityRenderUpdate::FOG_VOLUME;
	}
	if (p_component == EntityComponentTraits<EntityReflectionProbe>::id) {
		return EntityRenderUpdate::REFLECTION_PROBE;
	}
	if (p_component == EntityComponentTraits<EntityParticles>::id) {
		return EntityRenderUpdate::PARTICLES;
	}
	if (p_component == EntityComponentTraits<EntityVoxelGI>::id) {
		return EntityRenderUpdate::VOXEL_GI;
	}
	if (p_component == EntityComponentTraits<EntityLightmap>::id) {
		return EntityRenderUpdate::LIGHTMAP;
	}
	if (p_component == EntityComponentTraits<EntitySkinningPose>::id) {
		return EntityRenderUpdate::SKINNING_POSE;
	}
	if (p_component == EntityComponentTraits<EntityParticlesCollision>::id) {
		return EntityRenderUpdate::PARTICLES_COLLISION;
	}
	return 0;
}

void EntityRenderSystem::prepare_initial(const LocalVector<EntityInitialRenderGroup> &p_groups, EntityInitialRenderProfile *r_profile) {
	if (p_groups.is_empty() || world.get_scenario().is_null()) {
		return;
	}
	uint32_t entity_count = 0;
	for (const EntityInitialRenderGroup &group : p_groups) {
		entity_count += group.entities.size();
	}
	const int offset = initial_updates.size();
	initial_updates.resize(offset + entity_count);
	EntityRenderUpdate *updates = initial_updates.ptrw() + offset;
	const bool erase_dirty = !dirty.is_empty();
	uint32_t written = 0;
	for (const EntityInitialRenderGroup &group : p_groups) {
		if (group.entities.is_empty()) {
			continue;
		}
		const ecs_table_t *table = static_cast<const ecs_table_t *>(group.table);
		bool contiguous = table && ecs_table_has_id(world.ecs.c_ptr(), table, group.storage_tag);
		const int32_t table_count = contiguous ? ecs_table_count(table) : 0;
		const ecs_entity_t *table_entities = contiguous ? ecs_table_entities(table) : nullptr;
		const EntityWorld::Identity *identities = contiguous ? static_cast<const EntityWorld::Identity *>(ecs_table_get_id(world.ecs.c_ptr(), table, world.ecs.id<EntityWorld::Identity>(), 0)) : nullptr;
		const void *transform_states = contiguous ? world._get_transform_states(table) : nullptr;
		const EntityTransform *transforms = contiguous ? static_cast<const EntityTransform *>(ecs_table_get_id(world.ecs.c_ptr(), table, world.ecs.id<EntityTransform>(), 0)) : nullptr;
		const EntityVisibility *visibilities = contiguous ? static_cast<const EntityVisibility *>(ecs_table_get_id(world.ecs.c_ptr(), table, world.ecs.id<EntityVisibility>(), 0)) : nullptr;
		contiguous = contiguous && table_count <= int32_t(group.entities.size()) && table_entities && identities && transform_states;
		auto column = [&](auto *p_type, EntityRenderUpdate::Component p_flag) {
			using T = std::remove_pointer_t<decltype(p_type)>;
			return contiguous && (group.entities[0].components & p_flag) ? static_cast<const T *>(ecs_table_get_id(world.ecs.c_ptr(), table, world.ecs.id<T>(), 0)) : nullptr;
		};
		const EntityMesh *meshes = column(static_cast<EntityMesh *>(nullptr), EntityRenderUpdate::MESH);
		const EntityGeometry *geometries = column(static_cast<EntityGeometry *>(nullptr), EntityRenderUpdate::GEOMETRY);
		const EntityCamera *cameras = column(static_cast<EntityCamera *>(nullptr), EntityRenderUpdate::CAMERA);
		const EntityLight *lights = column(static_cast<EntityLight *>(nullptr), EntityRenderUpdate::LIGHT);
		const EntityEnvironment *environments = column(static_cast<EntityEnvironment *>(nullptr), EntityRenderUpdate::ENVIRONMENT);
		const EntityMultiMesh *multimeshes = column(static_cast<EntityMultiMesh *>(nullptr), EntityRenderUpdate::MULTIMESH);
		const EntityDecal *decals = column(static_cast<EntityDecal *>(nullptr), EntityRenderUpdate::DECAL);
		const EntityFogVolume *fog_volumes = column(static_cast<EntityFogVolume *>(nullptr), EntityRenderUpdate::FOG_VOLUME);
		const EntityReflectionProbe *reflection_probes = column(static_cast<EntityReflectionProbe *>(nullptr), EntityRenderUpdate::REFLECTION_PROBE);
		const EntityParticles *particles = column(static_cast<EntityParticles *>(nullptr), EntityRenderUpdate::PARTICLES);
		const EntityVoxelGI *voxel_gis = column(static_cast<EntityVoxelGI *>(nullptr), EntityRenderUpdate::VOXEL_GI);
		const EntityLightmap *lightmaps = column(static_cast<EntityLightmap *>(nullptr), EntityRenderUpdate::LIGHTMAP);
		const EntitySkinningPose *skinning_poses = column(static_cast<EntitySkinningPose *>(nullptr), EntityRenderUpdate::SKINNING_POSE);
		const EntityParticlesCollision *particles_collisions = column(static_cast<EntityParticlesCollision *>(nullptr), EntityRenderUpdate::PARTICLES_COLLISION);
		contiguous = contiguous && (!(group.entities[0].components & EntityRenderUpdate::MESH) || meshes) && (!(group.entities[0].components & EntityRenderUpdate::GEOMETRY) || geometries) && (!(group.entities[0].components & EntityRenderUpdate::CAMERA) || cameras) && (!(group.entities[0].components & EntityRenderUpdate::LIGHT) || lights) && (!(group.entities[0].components & EntityRenderUpdate::ENVIRONMENT) || environments) && (!(group.entities[0].components & EntityRenderUpdate::MULTIMESH) || multimeshes) && (!(group.entities[0].components & EntityRenderUpdate::DECAL) || decals) && (!(group.entities[0].components & EntityRenderUpdate::FOG_VOLUME) || fog_volumes) && (!(group.entities[0].components & EntityRenderUpdate::REFLECTION_PROBE) || reflection_probes) && (!(group.entities[0].components & EntityRenderUpdate::PARTICLES) || particles) && (!(group.entities[0].components & EntityRenderUpdate::VOXEL_GI) || voxel_gis) && (!(group.entities[0].components & EntityRenderUpdate::LIGHTMAP) || lightmaps) && (!(group.entities[0].components & EntityRenderUpdate::SKINNING_POSE) || skinning_poses) && (!(group.entities[0].components & EntityRenderUpdate::PARTICLES_COLLISION) || particles_collisions);

		auto finish = [&](EntityRenderUpdate &r_update) {
			r_update.changed_components = EntityRenderUpdate::COMPONENTS;
			if (r_update.components & EntityRenderUpdate::GEOMETRY) {
				r_update.procedural = r_update.geometry.rt_procedural;
			}
			if (erase_dirty) {
				dirty.erase(r_update.id);
			}
		};
		if (contiguous) {
			if (r_profile) {
				r_profile->table_rows += table_count;
			}
			for (int32_t row = 0; row < table_count; row++) {
				EntityRenderUpdate &update = updates[written++];
				update.id = identities[row].id;
				update.handle = { world.generation, table_entities[row] };
				update.reset_revision = world._get_transform_reset_revision(transform_states, row);
				if (transforms) {
					update.pose = transforms[row].render;
				}
				if (visibilities) {
					update.visible = visibilities[row].effective;
				}
				update.components = group.entities[0].components;
				if (meshes) {
					update.mesh = meshes[row];
				}
				if (geometries) {
					update.geometry = geometries[row];
				}
				if (cameras) {
					update.camera = cameras[row];
				}
				if (lights) {
					update.light = lights[row];
				}
				if (environments) {
					update.environment = environments[row];
				}
				if (multimeshes) {
					update.multimesh = multimeshes[row];
				}
				if (decals) {
					update.decal = decals[row];
				}
				if (fog_volumes) {
					update.fog_volume = fog_volumes[row];
				}
				if (reflection_probes) {
					update.reflection_probe = reflection_probes[row];
				}
				if (particles) {
					update.particles = particles[row];
				}
				if (voxel_gis) {
					update.voxel_gi = voxel_gis[row];
				}
				if (lightmaps) {
					update.lightmap = lightmaps[row];
				}
				if (skinning_poses) {
					update.skinning_pose = skinning_poses[row];
				}
				if (particles_collisions) {
					update.particles_collision = particles_collisions[row];
				}
				finish(update);
			}
		}
		if (contiguous && table_count == int32_t(group.entities.size())) {
			continue;
		}
		for (const EntityInitialRender &initial : group.entities) {
			if (contiguous && ecs_get_table(world.ecs.c_ptr(), initial.handle.entity) == table) {
				continue;
			}
			EntityRenderUpdate &update = updates[written++];
			if (r_profile) {
				r_profile->fallback_rows++;
			}
			update.id = initial.id;
			update.handle = initial.handle;
			update.reset_revision = world.get_transforms().get_reset_revision(initial.handle.entity);
			if (const EntityTransform *transform = static_cast<const EntityTransform *>(ecs_get_id(world.ecs.c_ptr(), initial.handle.entity, world.ecs.id<EntityTransform>()))) {
				update.pose = transform->render;
			}
			if (const EntityVisibility *visibility = static_cast<const EntityVisibility *>(ecs_get_id(world.ecs.c_ptr(), initial.handle.entity, world.ecs.id<EntityVisibility>()))) {
				update.visible = visibility->effective;
			}
			update.components = initial.components;
			auto copy_component = [&](auto &r_value, EntityRenderUpdate::Component p_flag) {
				if (initial.components & p_flag) {
					using T = std::decay_t<decltype(r_value)>;
					if (const T *component = static_cast<const T *>(ecs_get_id(world.ecs.c_ptr(), initial.handle.entity, world.ecs.id<T>()))) {
						r_value = *component;
					}
				}
			};
			copy_component(update.mesh, EntityRenderUpdate::MESH);
			copy_component(update.geometry, EntityRenderUpdate::GEOMETRY);
			copy_component(update.camera, EntityRenderUpdate::CAMERA);
			copy_component(update.light, EntityRenderUpdate::LIGHT);
			copy_component(update.environment, EntityRenderUpdate::ENVIRONMENT);
			copy_component(update.multimesh, EntityRenderUpdate::MULTIMESH);
			copy_component(update.decal, EntityRenderUpdate::DECAL);
			copy_component(update.fog_volume, EntityRenderUpdate::FOG_VOLUME);
			copy_component(update.reflection_probe, EntityRenderUpdate::REFLECTION_PROBE);
			copy_component(update.particles, EntityRenderUpdate::PARTICLES);
			copy_component(update.voxel_gi, EntityRenderUpdate::VOXEL_GI);
			copy_component(update.lightmap, EntityRenderUpdate::LIGHTMAP);
			copy_component(update.skinning_pose, EntityRenderUpdate::SKINNING_POSE);
			copy_component(update.particles_collision, EntityRenderUpdate::PARTICLES_COLLISION);
			finish(update);
		}
	}
	DEV_ASSERT(written == entity_count);
}

void EntityRenderSystem::publish() {
	if ((dirty.is_empty() && initial_updates.is_empty()) || world.get_scenario().is_null()) {
		return;
	}
	EntityRenderPacket packet;
	packet.scenario = world.get_scenario();
	packet.camera = world.get_camera();
	packet.world_generation = world.get_generation();
	packet.sequence = ++sequence;
	packet.updates = std::move(initial_updates);
	packet.poses.reserve(dirty.size());
	for (const KeyValue<EntityId, uint32_t> &entry : dirty) {
		EntityId id = entry.key;
		EntityRenderPoseUpdate pose;
		pose.id = id;
		EntityResolution resolution = world.resolve({ id });
		if (resolution.state == EntityReferenceState::RESIDENT) {
			pose.handle = resolution.handle;
			pose.reset_revision = world.get_transforms().get_reset_revision(pose.handle.entity);
			if (const EntityTransform *transform = world.get<EntityTransform>(pose.handle)) {
				pose.pose = transform->render;
			}
			if (const EntityVisibility *visibility = world.get<EntityVisibility>(pose.handle)) {
				pose.visible = visibility->effective;
			}
			if (!(entry.value & EntityRenderUpdate::COMPONENTS)) {
				packet.poses.push_back(pose);
				continue;
			}
		}
		EntityRenderUpdate update;
		static_cast<EntityRenderPoseUpdate &>(update) = pose;
		update.changed_components = entry.value & EntityRenderUpdate::COMPONENTS;
		if (update.changed_components & EntityRenderUpdate::GEOMETRY) {
			update.changed_components |= EntityRenderUpdate::MESH;
		}
		uint32_t payload = update.changed_components;
		if (payload & (EntityRenderUpdate::MESH | EntityRenderUpdate::MULTIMESH | EntityRenderUpdate::PARTICLES)) {
			payload |= EntityRenderUpdate::GEOMETRY;
		}
		if (update.handle.is_valid()) {
			if (const EntityGeometry *geometry = world.get<EntityGeometry>(update.handle)) {
				update.procedural = geometry->rt_procedural;
			}
			auto copy_component = [&](auto &r_value, EntityRenderUpdate::Component p_flag) {
				using T = std::decay_t<decltype(r_value)>;
				if (const T *component = world.get<T>(update.handle)) {
					if (payload & p_flag) {
						r_value = *component;
					}
					update.components |= p_flag;
				}
			};
			copy_component(update.mesh, EntityRenderUpdate::MESH);
			copy_component(update.geometry, EntityRenderUpdate::GEOMETRY);
			copy_component(update.camera, EntityRenderUpdate::CAMERA);
			copy_component(update.light, EntityRenderUpdate::LIGHT);
			copy_component(update.environment, EntityRenderUpdate::ENVIRONMENT);
			copy_component(update.multimesh, EntityRenderUpdate::MULTIMESH);
			copy_component(update.decal, EntityRenderUpdate::DECAL);
			copy_component(update.fog_volume, EntityRenderUpdate::FOG_VOLUME);
			copy_component(update.reflection_probe, EntityRenderUpdate::REFLECTION_PROBE);
			copy_component(update.particles, EntityRenderUpdate::PARTICLES);
			copy_component(update.voxel_gi, EntityRenderUpdate::VOXEL_GI);
			copy_component(update.lightmap, EntityRenderUpdate::LIGHTMAP);
			copy_component(update.skinning_pose, EntityRenderUpdate::SKINNING_POSE);
			copy_component(update.particles_collision, EntityRenderUpdate::PARTICLES_COLLISION);
		}
		packet.updates.push_back(update);
	}
	dirty.clear();
	RenderingServer::get_singleton()->scene_publish_entities(packet);
	initial_updates = std::move(packet.updates);
	initial_updates.clear();
}

void EntityRenderSystem::release() {
	initial_updates.clear();
	if (world.get_scenario().is_null()) {
		return;
	}
	EntityRenderPacket packet;
	packet.scenario = world.get_scenario();
	packet.camera = world.get_camera();
	packet.world_generation = world.get_generation();
	packet.sequence = ++sequence;
	packet.release = true;
	RenderingServer::get_singleton()->scene_publish_entities(packet);
}
