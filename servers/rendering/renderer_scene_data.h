#pragma once

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/math/color.h"
#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "servers/rendering/rendering_server_enums.h"

Color render_light_color_from_temperature(float p_temperature);

struct ToolRenderData {
	RID handle;
	RID base;
	RID scenario;
	RID skeleton;
	RID material_override;
	Transform3D transform;
	Vector<Ref<Resource>> assets;
	Ref<Resource> base_asset;
	Ref<Resource> material_asset;
	uint32_t layers = 1;
	RSE::ShadowCastingSetting cast_shadows = RSE::SHADOW_CASTING_SETTING_ON;
	float extra_margin = 0.0;
	float sorting_offset = 0.0;
	bool use_aabb_center = true;
	bool visible = true;
	bool baked_light = true;
	bool dynamic_gi = false;
	bool ignore_occlusion_culling = false;
	bool ignore_all_culling = false;

	static ToolRenderData create(RID p_base = RID(), RID p_scenario = RID(), const Ref<Resource> &p_asset = Ref<Resource>());
	bool is_valid() const { return handle.is_valid(); }
	bool is_null() const { return handle.is_null(); }
	void publish() const;
	void clear();
};

struct RenderSceneInstanceData {
	RSE::InstanceType base_type = RSE::INSTANCE_NONE;
	RID base;
	RID skeleton;
	RID material_override;
	RID material_overlay;
	RID mesh_instance;
	RID render_handle;
	RID scenario_rid;
	Transform3D transform;
	double origin[3] = {};
	Vector<RID> materials;
	AABB aabb;
	AABB transformed_aabb;
	float transparency = 0.0f;
	float lod_bias = 1.0;
	float sorting_offset = 0.0;
	uint32_t layer_mask = 1;
	RSE::ShadowCastingSetting cast_shadows = RSE::SHADOW_CASTING_SETTING_ON;
	bool mirror = false;
	bool visible = true;
	bool baked_light = false;
	bool dynamic_gi = false;
	bool use_aabb_center = true;
};
