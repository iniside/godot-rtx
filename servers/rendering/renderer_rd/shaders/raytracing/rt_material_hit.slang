#[raygen]

#VERSION_DEFINES
#include "rt_hit_context_inc.slang"

[shader("raygeneration")]
void main() {
	uint3 dispatch = DispatchRaysIndex();
	uint3 dimensions = DispatchRaysDimensions();
	uint index = dispatch.x + dimensions.x * (dispatch.y + dimensions.y * dispatch.z);
	if (index >= rt_material_frame.counts.z) {
		return;
	}
	RTHitRay input = rt_hit_rays[index];
	RayDesc ray;
	ray.Origin = input.origin;
	ray.Direction = normalize(input.direction);
	ray.TMin = input.min_t;
	ray.TMax = input.max_t;
	RTHitPayload payload = (RTHitPayload)0;
	payload.cone_width = input.cone_width;
	payload.cone_spread = input.cone_spread;
	payload.ray_layer_mask = input.layer_mask;
	payload.ray_flags = input.flags;
	TraceRay(scene_tlas, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, 0, 0, 0, ray, payload);
	rt_hit_results[index] = payload;
}

#[any_hit]

#VERSION_DEFINES
#define RT_MATERIAL_HIT_STAGE
#include "rt_hit_context_inc.slang"

struct MaterialUniforms {
#MATERIAL_UNIFORMS
};
static MaterialUniforms material;
#GLOBALS
#CODE : RT_MATERIAL_CALLBACKS

[shader("anyhit")]
void main(inout RTHitPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
	GeometryHitInput hit;
	MaterialData material_data;
	GeometryTriangle triangle;
	if (!load_geometry_hit(InstanceID(), PrimitiveIndex(), GetClusterID(), attributes.barycentrics, ObjectToWorld3x4(), hit, material_data) || !geometry_decode_triangle(hit, triangle)) {
		IgnoreHit();
		return;
	}
	bool shadow_ray = (payload.ray_flags & 1u) != 0u;
	bool native_front = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
	bool facing = shadow_ray ? ((hit.geometry.flags & FLAG_SHADOW_CULL_ENABLED) == 0u || !native_front) : (RT_CULL_MODE == 0 || !native_front);
	if (!facing || (hit.geometry.instance_layer_mask & payload.ray_layer_mask) == 0u) {
		IgnoreHit();
		return;
	}
	float3 object_position, object_dx, object_dy;
	float2 uv, uv_dx, uv_dy;
	float radius = max(payload.cone_width + RayTCurrent() * payload.cone_spread, 1e-6);
	rt_ray_footprint(hit, triangle, WorldRayDirection(), radius, object_position, object_dx, object_dy, uv, uv_dx, uv_dy);
	if (!geometry_material_covered(hit, material_data, object_position, object_dx, object_dy, uv, uv_dx, uv_dy, shadow_ray, payload.ray_layer_mask)) {
		IgnoreHit();
	}
}

#[closest_hit]

#VERSION_DEFINES
#define RT_MATERIAL_HIT_STAGE
#include "rt_hit_context_inc.slang"

struct MaterialUniforms {
#MATERIAL_UNIFORMS
};
static MaterialUniforms material;
#GLOBALS
#CODE : RT_MATERIAL_CALLBACKS

[shader("closesthit")]
void main(inout RTHitPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
	GeometryHitInput hit;
	MaterialData material_data;
	GeometryTriangle triangle;
	if (!load_geometry_hit(InstanceID(), PrimitiveIndex(), GetClusterID(), attributes.barycentrics, ObjectToWorld3x4(), hit, material_data) || !geometry_decode_triangle(hit, triangle)) {
		payload.hit = 0u;
		return;
	}
	bool native_front = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
	payload.surface = rt_evaluate_material(hit, triangle, WorldRayDirection(), RayTCurrent(), native_front, payload.cone_width, payload.cone_spread);
	payload.hit = 1u;
	payload.hit_distance = RayTCurrent();
	payload.cone_width = max(payload.cone_width + payload.hit_distance * payload.cone_spread, 0.0);
	payload.geometry_index = hit.geometry_index;
	geometry_resolve_primitive(hit.geometry, hit.cluster_id, hit.primitive_id, payload.primitive_index);
	payload.barycentrics = attributes.barycentrics;
	payload.front_face = front_facing_builtin ? 1u : 0u;
}

#[miss]

#VERSION_DEFINES
#include "rt_hit_context_inc.slang"

[shader("miss")]
void main(inout RTHitPayload payload) {
	payload.hit = 0u;
}
