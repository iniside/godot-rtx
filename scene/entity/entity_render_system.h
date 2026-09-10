#pragma once

#include "entity_components.h"

#include "core/templates/hash_map.h"

struct EntityRenderPoseUpdate {
	EntityId id;
	EntityHandle handle;
	uint64_t reset_revision = 0;
	bool visible = true;
	EntityPose pose;
};

struct EntityRenderUpdate : EntityRenderPoseUpdate {
	enum Component : uint32_t {
		MESH = 1 << 0,
		GEOMETRY = 1 << 1,
		CAMERA = 1 << 2,
		LIGHT = 1 << 3,
		ENVIRONMENT = 1 << 4,
		MULTIMESH = 1 << 5,
		DECAL = 1 << 6,
		FOG_VOLUME = 1 << 7,
		REFLECTION_PROBE = 1 << 8,
		PARTICLES = 1 << 9,
		VOXEL_GI = 1 << 10,
		LIGHTMAP = 1 << 11,
		SKINNING_POSE = 1 << 12,
		PARTICLES_COLLISION = 1 << 13,
		COMPONENT_COUNT = 14,
		POSE = 1 << COMPONENT_COUNT,
		COMPONENTS = POSE - 1,
		ALL = (POSE << 1) - 1,
	};
	uint32_t components = 0;
	uint32_t changed_components = 0;
	bool procedural = false;
	EntityMesh mesh;
	EntityGeometry geometry;
	EntityCamera camera;
	EntityLight light;
	EntityEnvironment environment;
	EntityMultiMesh multimesh;
	EntityDecal decal;
	EntityFogVolume fog_volume;
	EntityReflectionProbe reflection_probe;
	EntityParticles particles;
	EntityVoxelGI voxel_gi;
	EntityLightmap lightmap;
	EntitySkinningPose skinning_pose;
	EntityParticlesCollision particles_collision;
};

struct EntityRenderPacket {
	RID scenario;
	RID camera;
	uint64_t world_generation = 0;
	uint64_t sequence = 0;
	bool release = false;
	Vector<EntityRenderUpdate> updates;
	Vector<EntityRenderPoseUpdate> poses;
};

class EntityWorld;

class EntityRenderSystem {
	EntityWorld &world;
	HashMap<EntityId, uint32_t, EntityIdHasher> dirty;
	uint64_t sequence = 0;

public:
	explicit EntityRenderSystem(EntityWorld &p_world) : world(p_world) {}
	static uint32_t component_mask(uint64_t p_component);
	void mark_dirty(EntityId p_id, uint32_t p_mask = EntityRenderUpdate::ALL) {
		if (p_mask) {
			dirty[p_id] |= p_mask;
		}
	}
	void publish();
	void release();
};
