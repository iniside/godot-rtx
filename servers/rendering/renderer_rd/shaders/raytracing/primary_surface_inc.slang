#ifndef PRIMARY_SURFACE_INC_SLANG
#define PRIMARY_SURFACE_INC_SLANG

#include "rt_hit_context_inc.slang"

struct PrimarySurfaceData {
	SurfaceBufferData surface;
	float4 motion;
	float depth;
};

float3 primary_surface_previous_position(RTHitPayload hit, StructuredBuffer<int> motion_indices, StructuredBuffer<InstanceMotionData> instance_motion) {
	int motion_index = motion_indices[hit.geometry_index];
	if (motion_index < 0) { return hit.surface.world_position; }
	GeometryData geometry = geometry_previous_positions(geometries[hit.geometry_index]);
	uint3 indices;
	uint primitive;
	if (!geometry_select_triangle(geometry, hit.cluster_id, hit.local_primitive_index, primitive) || !geometry_triangle_indices(geometry, primitive, indices)) { return hit.surface.world_position; }
	float3 bary = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
	float3 position = geometry_fetch_position(geometry, indices.x) * bary.x + geometry_fetch_position(geometry, indices.y) * bary.y + geometry_fetch_position(geometry, indices.z) * bary.z;
	InstanceMotionData motion = instance_motion[motion_index];
	return float3(dot(float4(position, 1.0), float4(motion.prev_object_to_rt[0], motion.prev_object_to_rt[1], motion.prev_object_to_rt[2], motion.prev_object_to_rt[3])),
		dot(float4(position, 1.0), float4(motion.prev_object_to_rt[4], motion.prev_object_to_rt[5], motion.prev_object_to_rt[6], motion.prev_object_to_rt[7])),
		dot(float4(position, 1.0), float4(motion.prev_object_to_rt[8], motion.prev_object_to_rt[9], motion.prev_object_to_rt[10], motion.prev_object_to_rt[11])));
}
PrimarySurfaceData reconstruct_primary_surface(RTHitPayload hit, RayDesc ray, uint2 extent, StructuredBuffer<int> motion_indices, StructuredBuffer<InstanceMotionData> instance_motion) {
	float4x4 camera_to_rt = rt_camera_to_rt(rt_frame, false);
	float4x4 previous_to_rt = rt_camera_to_rt(rt_frame, true);
	float3 current_view, previous_view;
	SurfaceBufferData packed = (SurfaceBufferData)0;
	if (hit.hit != 0u) {
		packed = encode_surface_buffers(hit.surface);
		packed.emission.rgb *= scene_data_block.data.emissive_exposure_normalization;
		current_view = mul(geometry_inverse(camera_to_rt), float4(hit.surface.world_position, 1.0)).xyz;
		previous_view = mul(geometry_inverse(previous_to_rt), float4(primary_surface_previous_position(hit, motion_indices, instance_motion), 1.0)).xyz;
	} else {
		current_view = mul(transpose((float3x3)camera_to_rt), ray.Direction);
		previous_view = mul(transpose((float3x3)previous_to_rt), ray.Direction);
	}
	float4 current_clip = mul(float4(current_view, hit.hit != 0u ? 1.0 : 0.0), scene_data_block.data.projection_matrix);
	float4 previous_clip = mul(float4(previous_view, hit.hit != 0u ? 1.0 : 0.0), scene_data_block.prev_data.projection_matrix);
	float2 motion = 0.0;
	if (abs(current_clip.w) > 1e-6 && abs(previous_clip.w) > 1e-6) {
		motion = ((previous_clip.xy / previous_clip.w - scene_data_block.prev_data.taa_jitter) - (current_clip.xy / current_clip.w - scene_data_block.data.taa_jitter)) * 0.5 * float2(extent);
	}
	PrimarySurfaceData result;
	result.surface = packed;
	result.motion = float4(motion, hit.hit != 0u ? current_view.z - previous_view.z : 0.0, packed.ambient_occlusion);
	result.depth = hit.hit != 0u ? current_clip.z / current_clip.w : 0.0;
	return result;
}

#endif
