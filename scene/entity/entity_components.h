#pragma once

#include "entity_id.h"

#include "core/math/basis.h"
#include "core/templates/vector.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/voxel_gi.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/compositor.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

#ifdef ENTITY_SCHEMA_SCAN
#define ENTITY_COMPONENT(m_data) __attribute__((annotate("entity_component:" m_data)))
#define ENTITY_VALUE(m_data) __attribute__((annotate("entity_value:" m_data)))
#define ENTITY_FIELD(m_data) __attribute__((annotate("entity_field:" m_data)))
#else
#define ENTITY_COMPONENT(m_data)
#define ENTITY_VALUE(m_data)
#define ENTITY_FIELD(m_data)
#endif

struct ENTITY_VALUE("id=1000000000000001") EntityPosition {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	double x = 0.0;
	ENTITY_FIELD("id=0000000000000002;unit=m")
	double y = 0.0;
	ENTITY_FIELD("id=0000000000000003;unit=m")
	double z = 0.0;
};

struct ENTITY_VALUE("id=1000000000000002") EntityPose {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	EntityPosition translation;
	ENTITY_FIELD("id=0000000000000002")
	Basis basis;
};

struct ENTITY_COMPONENT("id=2000000000000001") EntityName {
	ENTITY_FIELD("id=0000000000000001")
	String name;
	ENTITY_FIELD("id=0000000000000002;reference=entity")
	EntityRef document_group;
};

struct ENTITY_COMPONENT("id=2000000000000002") EntityTransform {
	ENTITY_FIELD("id=0000000000000001")
	EntityPose local;
	ENTITY_FIELD("id=0000000000000002;serialize=false;edit=false")
	EntityPose current;
	ENTITY_FIELD("id=0000000000000003;serialize=false;edit=false")
	EntityPose previous;
	ENTITY_FIELD("id=0000000000000004;serialize=false;edit=false")
	EntityPose render;
};

struct ENTITY_COMPONENT("id=2000000000000003") EntityMesh {
	ENTITY_FIELD("id=0000000000000001;reference=asset")
	Ref<Mesh> mesh;
	ENTITY_FIELD("id=0000000000000002;reference=asset")
	Ref<Material> material_override;
	ENTITY_FIELD("id=0000000000000003;reference=asset")
	Vector<Ref<Material>> surface_materials;
	ENTITY_FIELD("id=0000000000000004")
	bool visible = true;
	ENTITY_FIELD("id=0000000000000005;min=0;max=4294967295")
	uint32_t layers = 1;
};

struct ENTITY_COMPONENT("id=2000000000000004") EntityVisibility {
	ENTITY_FIELD("id=0000000000000001")
	bool visible = true;
	ENTITY_FIELD("id=0000000000000002")
	bool inherit_parent = false;
	ENTITY_FIELD("id=0000000000000003;serialize=false;edit=false")
	bool effective = true;
};

struct ENTITY_VALUE("id=1000000000000003") EntityShaderUniform {
	ENTITY_FIELD("id=0000000000000001")
	String name;
	ENTITY_FIELD("id=0000000000000002")
	Variant value;
};

struct ENTITY_COMPONENT("id=2000000000000005") EntityGeometry {
	ENTITY_FIELD("id=0000000000000001;min=0;max=3")
	uint32_t cast_shadows = 1;
	ENTITY_FIELD("id=0000000000000002")
	bool use_baked_light = false;
	ENTITY_FIELD("id=0000000000000003")
	bool use_dynamic_gi = false;
	ENTITY_FIELD("id=0000000000000004;reference=asset")
	Ref<Material> material_overlay;
	ENTITY_FIELD("id=0000000000000005;min=0;max=1")
	double transparency = 0.0;
	ENTITY_FIELD("id=0000000000000006;unit=m")
	double sorting_offset = 0.0;
	ENTITY_FIELD("id=0000000000000007")
	bool use_aabb_center = true;
	ENTITY_FIELD("id=0000000000000008;min=0;max=1000000")
	double lod_bias = 1.0;
	ENTITY_FIELD("id=0000000000000009")
	AABB custom_aabb;
	ENTITY_FIELD("id=000000000000000a;unit=m;min=0;max=1000000")
	double extra_cull_margin = 0.0;
	ENTITY_FIELD("id=000000000000000b")
	bool ignore_occlusion_culling = false;
	ENTITY_FIELD("id=000000000000000c")
	bool ignore_all_culling = false;
	ENTITY_FIELD("id=000000000000000d;unit=m;min=0;max=1000000")
	double visibility_range_begin = 0.0;
	ENTITY_FIELD("id=000000000000000e;unit=m;min=0;max=1000000")
	double visibility_range_end = 0.0;
	ENTITY_FIELD("id=000000000000000f;unit=m;min=0;max=1000000")
	double visibility_range_begin_margin = 0.0;
	ENTITY_FIELD("id=0000000000000010;unit=m;min=0;max=1000000")
	double visibility_range_end_margin = 0.0;
	ENTITY_FIELD("id=0000000000000011;min=0;max=2")
	uint32_t visibility_range_fade_mode = 0;
	ENTITY_FIELD("id=0000000000000012;reference=entity")
	EntityRef visibility_parent;
	ENTITY_FIELD("id=0000000000000013")
	Vector<EntityShaderUniform> shader_uniforms;
	ENTITY_FIELD("id=0000000000000014")
	Vector<double> blend_shape_weights;
	ENTITY_FIELD("id=0000000000000015;reference=entity")
	EntityRef skeleton;
	ENTITY_FIELD("id=0000000000000016;reference=entity")
	EntityRef lightmap;
	ENTITY_FIELD("id=0000000000000017")
	Rect2 lightmap_uv_scale;
	ENTITY_FIELD("id=0000000000000018")
	uint32_t lightmap_slice = 0;
	ENTITY_FIELD("id=0000000000000019")
	bool rt_procedural = false;
	ENTITY_FIELD("id=000000000000001a")
	AABB procedural_aabb;
	ENTITY_FIELD("id=000000000000001b")
	Vector<double> procedural_bounds;
	ENTITY_FIELD("id=000000000000001c")
	bool expose_procedural_bounds = false;
};

struct ENTITY_COMPONENT("id=2000000000000006") EntityCamera {
	ENTITY_FIELD("id=0000000000000001")
	bool current = true;
	ENTITY_FIELD("id=0000000000000002;min=0;max=2")
	uint32_t projection = 0;
	ENTITY_FIELD("id=0000000000000003;unit=deg;min=1;max=179")
	double fov = 75.0;
	ENTITY_FIELD("id=0000000000000004;unit=m;min=0.001;max=1000000")
	double size = 1.0;
	ENTITY_FIELD("id=0000000000000005;unit=m;min=0.001;max=1000000")
	double near_distance = 0.05;
	ENTITY_FIELD("id=0000000000000006;unit=m;min=0.001;max=1000000")
	double far_distance = 4000.0;
	ENTITY_FIELD("id=0000000000000007")
	Vector2 frustum_offset;
	ENTITY_FIELD("id=0000000000000008")
	bool keep_width = false;
	ENTITY_FIELD("id=0000000000000009")
	uint32_t layers = 0xfffff;
	ENTITY_FIELD("id=000000000000000a;unit=m")
	Vector2 offset;
	ENTITY_FIELD("id=000000000000000b;reference=asset")
	Ref<Environment> environment;
	ENTITY_FIELD("id=000000000000000c;reference=asset")
	Ref<CameraAttributes> attributes;
	ENTITY_FIELD("id=000000000000000d;reference=asset")
	Ref<Compositor> compositor;
};

struct ENTITY_COMPONENT("id=2000000000000007") EntityLight {
	ENTITY_FIELD("id=0000000000000001;min=0;max=3")
	uint32_t type = 0;
	ENTITY_FIELD("id=0000000000000002")
	Color color = Color(1, 1, 1, 1);
	ENTITY_FIELD("id=0000000000000003")
	double energy = 1.0;
	ENTITY_FIELD("id=0000000000000004")
	double indirect_energy = 1.0;
	ENTITY_FIELD("id=0000000000000005")
	double volumetric_fog_energy = 1.0;
	ENTITY_FIELD("id=0000000000000006")
	double specular = 1.0;
	ENTITY_FIELD("id=0000000000000007;unit=m;min=0;max=1000000")
	double range = 5.0;
	ENTITY_FIELD("id=0000000000000008;unit=m;min=0;max=1000000")
	double size = 0.0;
	ENTITY_FIELD("id=0000000000000009")
	double attenuation = 1.0;
	ENTITY_FIELD("id=000000000000000a;unit=deg;min=0;max=180")
	double spot_angle = 45.0;
	ENTITY_FIELD("id=000000000000000b")
	double spot_attenuation = 1.0;
	ENTITY_FIELD("id=000000000000000c")
	bool shadow = false;
	ENTITY_FIELD("id=000000000000000d")
	bool negative = false;
	ENTITY_FIELD("id=000000000000000e")
	bool shadow_reverse_cull = false;
	ENTITY_FIELD("id=000000000000000f")
	uint32_t cull_mask = 0xffffffff;
	ENTITY_FIELD("id=0000000000000010")
	uint32_t shadow_caster_mask = 0xffffffff;
	ENTITY_FIELD("id=0000000000000011;min=0;max=2")
	uint32_t bake_mode = 2;
	ENTITY_FIELD("id=0000000000000012;reference=asset")
	Ref<Texture2D> projector;
	ENTITY_FIELD("id=0000000000000013")
	bool distance_fade_enabled = false;
	ENTITY_FIELD("id=0000000000000014;unit=m")
	double distance_fade_begin = 40.0;
	ENTITY_FIELD("id=0000000000000015;unit=m")
	double distance_fade_shadow = 50.0;
	ENTITY_FIELD("id=0000000000000016;unit=m")
	double distance_fade_length = 10.0;
	ENTITY_FIELD("id=0000000000000017;unit=m")
	double shadow_max_distance = 100.0;
	ENTITY_FIELD("id=0000000000000018")
	Vector3 shadow_split_offsets = Vector3(0.1, 0.2, 0.5);
	ENTITY_FIELD("id=0000000000000019")
	double shadow_fade_start = 0.8;
	ENTITY_FIELD("id=000000000000001a")
	double shadow_normal_bias = 2.0;
	ENTITY_FIELD("id=000000000000001b")
	double shadow_bias = 0.1;
	ENTITY_FIELD("id=000000000000001c;unit=m")
	double shadow_pancake_size = 20.0;
	ENTITY_FIELD("id=000000000000001d;min=0;max=1")
	double shadow_opacity = 1.0;
	ENTITY_FIELD("id=000000000000001e")
	double shadow_blur = 1.0;
	ENTITY_FIELD("id=000000000000001f")
	double transmittance_bias = 0.05;
	ENTITY_FIELD("id=0000000000000020")
	double intensity = 100000.0;
	ENTITY_FIELD("id=0000000000000021;min=0;max=2")
	uint32_t directional_shadow_mode = 2;
	ENTITY_FIELD("id=0000000000000022")
	bool directional_blend_splits = false;
	ENTITY_FIELD("id=0000000000000023;min=0;max=2")
	uint32_t directional_sky_mode = 0;
	ENTITY_FIELD("id=0000000000000024;min=0;max=1")
	uint32_t omni_shadow_mode = 1;
	ENTITY_FIELD("id=0000000000000025;unit=m")
	Vector2 area_size = Vector2(1, 1);
	ENTITY_FIELD("id=0000000000000026;reference=asset")
	Ref<Texture2D> area_texture;
	ENTITY_FIELD("id=0000000000000027")
	bool area_normalize_energy = true;
	ENTITY_FIELD("id=0000000000000028;unit=K;min=1000;max=15000")
	double temperature = 6500.0;
};

struct ENTITY_COMPONENT("id=2000000000000008") EntityEnvironment {
	ENTITY_FIELD("id=0000000000000001;reference=asset")
	Ref<Environment> environment;
	ENTITY_FIELD("id=0000000000000002;reference=asset")
	Ref<CameraAttributes> attributes;
	ENTITY_FIELD("id=0000000000000003;reference=asset")
	Ref<Compositor> compositor;
};

struct ENTITY_COMPONENT("id=2000000000000009") EntityMultiMesh {
	ENTITY_FIELD("id=0000000000000001;reference=asset")
	Ref<MultiMesh> multimesh;
	ENTITY_FIELD("id=0000000000000002;reference=asset")
	Ref<Material> material_override;
	ENTITY_FIELD("id=0000000000000003;reference=asset")
	Vector<Ref<Material>> surface_materials;
	ENTITY_FIELD("id=0000000000000004")
	bool visible = true;
	ENTITY_FIELD("id=0000000000000005")
	uint32_t layers = 1;
};

struct ENTITY_COMPONENT("id=200000000000000a") EntityDecal {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	Vector3 size = Vector3(2, 2, 2);
	ENTITY_FIELD("id=0000000000000002;reference=asset")
	Ref<Texture2D> albedo;
	ENTITY_FIELD("id=0000000000000003;reference=asset")
	Ref<Texture2D> normal;
	ENTITY_FIELD("id=0000000000000004;reference=asset")
	Ref<Texture2D> orm;
	ENTITY_FIELD("id=0000000000000005;reference=asset")
	Ref<Texture2D> emission;
	ENTITY_FIELD("id=0000000000000006")
	double emission_energy = 1.0;
	ENTITY_FIELD("id=0000000000000007;min=0;max=1")
	double albedo_mix = 1.0;
	ENTITY_FIELD("id=0000000000000008")
	Color modulate = Color(1, 1, 1, 1);
	ENTITY_FIELD("id=0000000000000009")
	uint32_t cull_mask = 0xfffff;
	ENTITY_FIELD("id=000000000000000a;min=0;max=1")
	double normal_fade = 0.0;
	ENTITY_FIELD("id=000000000000000b")
	double upper_fade = 0.3;
	ENTITY_FIELD("id=000000000000000c")
	double lower_fade = 0.3;
	ENTITY_FIELD("id=000000000000000d")
	bool distance_fade_enabled = false;
	ENTITY_FIELD("id=000000000000000e;unit=m")
	double distance_fade_begin = 40.0;
	ENTITY_FIELD("id=000000000000000f;unit=m")
	double distance_fade_length = 10.0;
};

struct ENTITY_COMPONENT("id=200000000000000b") EntityFogVolume {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	Vector3 size = Vector3(2, 2, 2);
	ENTITY_FIELD("id=0000000000000002;min=0;max=4")
	uint32_t shape = 3;
	ENTITY_FIELD("id=0000000000000003;reference=asset")
	Ref<Material> material;
};

struct ENTITY_COMPONENT("id=200000000000000c") EntityReflectionProbe {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	Vector3 size = Vector3(20, 20, 20);
	ENTITY_FIELD("id=0000000000000002;unit=m")
	Vector3 origin_offset;
	ENTITY_FIELD("id=0000000000000003")
	double intensity = 1.0;
	ENTITY_FIELD("id=0000000000000004;unit=m")
	double blend_distance = 1.0;
	ENTITY_FIELD("id=0000000000000005;unit=m")
	double max_distance = 0.0;
	ENTITY_FIELD("id=0000000000000006")
	bool box_projection = false;
	ENTITY_FIELD("id=0000000000000007")
	bool enable_shadows = false;
	ENTITY_FIELD("id=0000000000000008")
	bool interior = false;
	ENTITY_FIELD("id=0000000000000009;min=0;max=2")
	uint32_t ambient_mode = 1;
	ENTITY_FIELD("id=000000000000000a")
	Color ambient_color = Color(0, 0, 0);
	ENTITY_FIELD("id=000000000000000b")
	double ambient_energy = 1.0;
	ENTITY_FIELD("id=000000000000000c")
	double mesh_lod_threshold = 1.0;
	ENTITY_FIELD("id=000000000000000d")
	uint32_t cull_mask = 0xfffff;
	ENTITY_FIELD("id=000000000000000e")
	uint32_t reflection_mask = 0xfffff;
	ENTITY_FIELD("id=000000000000000f;min=0;max=1")
	uint32_t update_mode = 0;
};

struct ENTITY_COMPONENT("id=200000000000000d") EntityParticles {
	ENTITY_FIELD("id=0000000000000001")
	bool emitting = true;
	ENTITY_FIELD("id=0000000000000002;min=1;max=1000000")
	uint32_t amount = 8;
	ENTITY_FIELD("id=0000000000000003;min=0;max=1")
	double amount_ratio = 1.0;
	ENTITY_FIELD("id=0000000000000004;unit=s;min=0.001;max=1000000")
	double lifetime = 1.0;
	ENTITY_FIELD("id=0000000000000005")
	bool one_shot = false;
	ENTITY_FIELD("id=0000000000000006;unit=s")
	double preprocess = 0.0;
	ENTITY_FIELD("id=0000000000000007;min=0;max=1")
	double explosiveness = 0.0;
	ENTITY_FIELD("id=0000000000000008;min=0;max=1")
	double randomness = 0.0;
	ENTITY_FIELD("id=0000000000000009")
	double speed_scale = 1.0;
	ENTITY_FIELD("id=000000000000000a")
	AABB visibility_aabb = AABB(Vector3(-4, -4, -4), Vector3(8, 8, 8));
	ENTITY_FIELD("id=000000000000000b")
	bool local_coords = false;
	ENTITY_FIELD("id=000000000000000c;min=0;max=1000")
	uint32_t fixed_fps = 30;
	ENTITY_FIELD("id=000000000000000d")
	bool fractional_delta = true;
	ENTITY_FIELD("id=000000000000000e")
	bool interpolate = true;
	ENTITY_FIELD("id=000000000000000f;reference=entity")
	EntityRef sub_emitter;
	ENTITY_FIELD("id=0000000000000010;unit=m")
	double collision_base_size = 0.01;
	ENTITY_FIELD("id=0000000000000011")
	uint32_t seed = 0;
	ENTITY_FIELD("id=0000000000000012")
	bool use_fixed_seed = false;
	ENTITY_FIELD("id=0000000000000013")
	bool trail_enabled = false;
	ENTITY_FIELD("id=0000000000000014;unit=s")
	double trail_lifetime = 0.3;
	ENTITY_FIELD("id=0000000000000015;min=0;max=4")
	uint32_t transform_align = 0;
	ENTITY_FIELD("id=0000000000000016")
	uint32_t transform_align_channel_filter = 0;
	ENTITY_FIELD("id=0000000000000017")
	uint32_t transform_align_axis = 1;
	ENTITY_FIELD("id=0000000000000019;reference=asset")
	Ref<Material> process_material;
	ENTITY_FIELD("id=000000000000001a;min=0;max=3")
	uint32_t draw_order = 0;
	ENTITY_FIELD("id=000000000000001b;reference=asset")
	Vector<Ref<Mesh>> draw_passes;
	ENTITY_FIELD("id=000000000000001c;reference=asset")
	Ref<Skin> skin;
	ENTITY_FIELD("id=000000000000001d;min=0;max=1")
	double interp_to_end = 0.0;
	ENTITY_FIELD("id=000000000000001e;reference=asset")
	Ref<Material> material_override;
	ENTITY_FIELD("id=000000000000001f")
	uint32_t layers = 1;
	ENTITY_FIELD("id=0000000000000020;serialize=false;edit=false")
	uint32_t restart_revision = 0;
	ENTITY_FIELD("id=0000000000000021;serialize=false;edit=false")
	Vector3 emitter_velocity;
};

struct ENTITY_COMPONENT("id=200000000000000e") EntityVoxelGI {
	ENTITY_FIELD("id=0000000000000001;reference=asset")
	Ref<VoxelGIData> data;
};

struct ENTITY_COMPONENT("id=200000000000000f") EntityLightmap {
	ENTITY_FIELD("id=0000000000000001;reference=asset")
	Ref<LightmapGIData> data;
};

struct ENTITY_COMPONENT("id=2000000000000010") EntitySkinningPose {
	ENTITY_FIELD("id=0000000000000001;serialize=false;edit=false")
	Vector<EntityPose> bones;
};

struct ENTITY_COMPONENT("id=2000000000000011") EntityParticlesCollision {
	ENTITY_FIELD("id=0000000000000001;min=0;max=6")
	uint32_t type = 3;
	ENTITY_FIELD("id=0000000000000002;unit=m;min=0.001;max=1000000")
	double radius = 1.0;
	ENTITY_FIELD("id=0000000000000003;unit=m")
	Vector3 size = Vector3(2, 2, 2);
	ENTITY_FIELD("id=0000000000000004")
	uint32_t cull_mask = 0xffffffff;
	ENTITY_FIELD("id=0000000000000005")
	double strength = 1.0;
	ENTITY_FIELD("id=0000000000000006;min=0;max=1")
	double directionality = 0.0;
	ENTITY_FIELD("id=0000000000000007")
	double attenuation = 1.0;
	ENTITY_FIELD("id=0000000000000008;reference=asset")
	Ref<Texture3D> field;
	ENTITY_FIELD("id=0000000000000009;min=0;max=5")
	uint32_t heightfield_resolution = 2;
	ENTITY_FIELD("id=000000000000000a")
	uint32_t heightfield_mask = 0xfffff;
	ENTITY_FIELD("id=000000000000000b;min=0;max=1")
	uint32_t heightfield_update_mode = 0;
	ENTITY_FIELD("id=000000000000000c")
	bool heightfield_follow_camera = false;
	ENTITY_FIELD("id=000000000000000d;serialize=false;edit=false")
	uint32_t update_revision = 0;
};
