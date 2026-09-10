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

void EntityRenderSystem::publish() {
	if (dirty.is_empty() || world.get_scenario().is_null()) {
		return;
	}
	EntityRenderPacket packet;
	packet.scenario = world.get_scenario();
	packet.camera = world.get_camera();
	packet.world_generation = world.get_generation();
	packet.sequence = ++sequence;
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
}

void EntityRenderSystem::release() {
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
