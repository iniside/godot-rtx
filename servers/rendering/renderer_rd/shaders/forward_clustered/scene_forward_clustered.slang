#[vertex]

#define GODOT_VERTEX_STAGE

#VERSION_DEFINES

static uint vertex_index;
static uint instance_index_builtin;
static uint view_index;
static float4 vertex_position;
static float vertex_point_size = 1.0;
#include "scene_forward_clustered_inc.slang"

#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0

#ifdef USE_MULTIVIEW
#define OUTPUT_IS_MULTIVIEW true
#else
#define OUTPUT_IS_MULTIVIEW false
#endif

static float4 vertex_angle_attrib;

#if defined(NORMAL_USED) || defined(TANGENT_USED)

static float4 axis_tangent_attrib;
#endif

#if defined(COLOR_USED)
static float4 color_attrib;
#endif

#ifdef UV_USED
static float2 uv_attrib;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP) || defined(MODE_RENDER_MATERIAL)
static float2 uv2_attrib;
#endif

#if defined(CUSTOM0_USED)
static float4 custom0_attrib;
#endif

#if defined(CUSTOM1_USED)
static float4 custom1_attrib;
#endif

#if defined(CUSTOM2_USED)
static float4 custom2_attrib;
#endif

#if defined(CUSTOM3_USED)
static float4 custom3_attrib;
#endif

#if defined(BONES_USED) || defined(USE_PARTICLE_TRAILS)
static uint4 bone_attrib;
#endif

#if defined(WEIGHTS_USED) || defined(USE_PARTICLE_TRAILS)
static float4 weight_attrib;
#endif

#ifdef MOTION_VECTORS
static float4 previous_vertex_attrib;

#if defined(NORMAL_USED) || defined(TANGENT_USED)
static float4 previous_normal_attrib;
#endif

#endif

void axis_angle_to_tbn(float3 axis, float angle, out float3 tangent, out float3 binormal, out float3 normal) {
	float c = cos(angle);
	float s = sin(angle);
	float3 omc_axis = (1.0 - c) * axis;
	float3 s_axis = s * axis;
	tangent = omc_axis.xxx * axis + float3(c, -s_axis.z, s_axis.y);
	binormal = omc_axis.yyy * axis + float3(s_axis.z, c, -s_axis.x);
	normal = omc_axis.zzz * axis + float3(-s_axis.y, s_axis.x, c);
}

static float3 vertex_interp;

#ifdef NORMAL_USED
static float3 normal_interp;
#endif

#if defined(COLOR_USED)
static float4 color_interp;
#endif

#ifdef UV_USED
static float2 uv_interp;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
static float2 uv2_interp;
#endif

#ifdef TANGENT_USED
static float3 tangent_interp;
static float3 binormal_interp;
#endif

#ifdef MOTION_VECTORS
static float4 screen_position;
static float4 prev_screen_position;
#ifdef MODE_RTXDI_SURFACE
static float2 linear_depth_interp;
#endif
#endif

#ifdef MATERIAL_UNIFORMS_USED

struct MaterialUniforms {
#MATERIAL_UNIFORMS
};
[[vk::binding(0, MATERIAL_UNIFORM_SET)]] ConstantBuffer<MaterialUniforms> material;

#endif

static float global_time;
static float global_prev_time;

#ifdef MODE_DUAL_PARABOLOID

static float dp_clip;

#endif

static uint instance_index_interp;

#ifdef USE_MULTIVIEW
#define ViewIndex view_index

float3 multiview_uv(float2 uv) {
	return float3(uv, ViewIndex);
}
int3 multiview_uv(int2 uv) {
	return int3(uv, int(ViewIndex));
}
int4 godot_multiview_load_coord(int3 uv, int lod) { return int4(uv, lod); }
static float4 combined_projected;
#else
#define ViewIndex 0
float2 multiview_uv(float2 uv) {
	return uv;
}
int2 multiview_uv(int2 uv) {
	return uv;
}
int3 godot_multiview_load_coord(int2 uv, int lod) { return int3(uv, lod); }
#endif

#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
static float2 point_coord_interp;
#endif

#GLOBALS

#ifdef USE_DOUBLE_PRECISION

float3 quick_two_sum(float3 a, float3 b, out float3 out_p) {
	precise float3 s = a + b;
	out_p = b - (s - a);
	return s;
}

float3 two_sum(float3 a, float3 b, out float3 out_p) {
	precise float3 s = a + b;
	precise float3 v = s - a;
	out_p = (a - (s - v)) + (b - v);
	return s;
}

float3 double_add_vec3(float3 base_a, float3 prec_a, float3 base_b, float3 prec_b, out float3 out_precision) {
	precise float3 s, t, se, te;
	s = two_sum(base_a, base_b, se);
	t = two_sum(prec_a, prec_b, te);
	se += t;
	precise float3 combined_error;
	s = quick_two_sum(s, se, combined_error);
	se = combined_error;
	se += te;
	s = quick_two_sum(s, se, out_precision);
	return s;
}
#endif

uint multimesh_stride() {
	uint stride = sc_multimesh_format_2d() ? 2 : 3;
	stride += sc_multimesh_has_color() ? 1 : 0;
	stride += sc_multimesh_has_custom_data() ? 1 : 0;
	return stride;
}

void vertex_shader(float3 vertex_input,
#ifdef NORMAL_USED
		in float3 normal_input,
#endif
#ifdef TANGENT_USED
		in float3 tangent_input,
		in float3 binormal_input,
#endif
		in uint instance_index, in uint multimesh_offset, in SceneData scene_data, in float3x4 in_model_matrix,
#ifdef USE_DOUBLE_PRECISION
		in float3 model_precision,
#endif
		out float4 screen_pos, out float linear_depth) {
	screen_pos = float4(0.0);
	linear_depth = 0.0;
	float4 instance_custom = float4(0.0);
#if defined(COLOR_USED)
	color_interp = color_attrib;
#endif

	float4x4 inv_view_matrix = transpose(float4x4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			float4(0.0, 0.0, 0.0, 1.0)));

	float4x4 model_matrix = transpose(float4x4(in_model_matrix[0],
			in_model_matrix[1],
			in_model_matrix[2],
			float4(0.0, 0.0, 0.0, 1.0)));

#ifdef USE_DOUBLE_PRECISION
	float3 view_precision = scene_data.inv_view_precision.xyz;
#endif

	float3x3 model_normal_matrix;
	if (bool(instances[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(godot_inverse((float3x3)model_matrix));
	} else {
		model_normal_matrix = (float3x3)model_matrix;
	}

	float4x4 matrix = float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
	float4x4 read_model_matrix = model_matrix;

	if (sc_multimesh()) {

#ifdef USE_PARTICLE_TRAILS
		uint trail_size = (instances[instance_index].flags >> INSTANCE_FLAGS_PARTICLE_TRAIL_SHIFT) & INSTANCE_FLAGS_PARTICLE_TRAIL_MASK;
		uint stride = 3 + 1 + 1;

		uint offset = trail_size * stride * INSTANCE_INDEX;

#ifdef COLOR_USED
		float4 pcolor;
#endif
		{
			uint boffset = offset + bone_attrib.x * stride;
			matrix = float4x4(transforms[boffset + 0], transforms[boffset + 1], transforms[boffset + 2], float4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.x;
#ifdef COLOR_USED
			pcolor = transforms[boffset + 3] * weight_attrib.x;
#endif
		}
		if (weight_attrib.y > 0.001) {
			uint boffset = offset + bone_attrib.y * stride;
			matrix += float4x4(transforms[boffset + 0], transforms[boffset + 1], transforms[boffset + 2], float4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.y;
#ifdef COLOR_USED
			pcolor += transforms[boffset + 3] * weight_attrib.y;
#endif
		}
		if (weight_attrib.z > 0.001) {
			uint boffset = offset + bone_attrib.z * stride;
			matrix += float4x4(transforms[boffset + 0], transforms[boffset + 1], transforms[boffset + 2], float4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.z;
#ifdef COLOR_USED
			pcolor += transforms[boffset + 3] * weight_attrib.z;
#endif
		}
		if (weight_attrib.w > 0.001) {
			uint boffset = offset + bone_attrib.w * stride;
			matrix += float4x4(transforms[boffset + 0], transforms[boffset + 1], transforms[boffset + 2], float4(0.0, 0.0, 0.0, 1.0)) * weight_attrib.w;
#ifdef COLOR_USED
			pcolor += transforms[boffset + 3] * weight_attrib.w;
#endif
		}

		instance_custom = transforms[offset + 4];

#ifdef COLOR_USED
		color_interp *= pcolor;
#endif

#else
		uint stride = multimesh_stride();
		uint offset = stride * (INSTANCE_INDEX + multimesh_offset);

		if (sc_multimesh_format_2d()) {
			matrix = float4x4(transforms[offset + 0], transforms[offset + 1], float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0));
			offset += 2;
		} else {
			matrix = float4x4(transforms[offset + 0], transforms[offset + 1], transforms[offset + 2], float4(0.0, 0.0, 0.0, 1.0));
			offset += 3;
		}

		if (sc_multimesh_has_color()) {
#ifdef COLOR_USED
			color_interp *= transforms[offset];
#endif
			offset += 1;
		}

		if (sc_multimesh_has_custom_data()) {
			instance_custom = transforms[offset];
		}

#endif

		matrix = transpose(matrix);
#if !defined(USE_DOUBLE_PRECISION) || defined(SKIP_TRANSFORM_USED) || defined(VERTEX_WORLD_COORDS_USED) || defined(MODEL_MATRIX_USED)

		read_model_matrix = mul(matrix, model_matrix);
#if !defined(USE_DOUBLE_PRECISION) || defined(SKIP_TRANSFORM_USED) || defined(VERTEX_WORLD_COORDS_USED)
		model_matrix = read_model_matrix;
#endif
#endif
		model_normal_matrix = mul((float3x3)matrix, model_normal_matrix);
	}

	float3 vertex = vertex_input;
#ifdef NORMAL_USED
	float3 normal_highp = normal_input;
#endif

#ifdef TANGENT_USED
	float3 tangent = tangent_input;
	float3 binormal = binormal_input;
#endif

#ifdef UV_USED
	uv_interp = uv_attrib;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	uv2_interp = uv2_attrib;
#endif

	float4 uv_scale = instances[instance_index].uv_scale;

	if (any(uv_scale != float4(0.0))) {
#ifdef UV_USED
		uv_interp = (uv_interp - 0.5) * uv_scale.xy;
#endif
#if defined(UV2_USED) || defined(USE_LIGHTMAP)
		uv2_interp = (uv2_interp - 0.5) * uv_scale.zw;
#endif
	}

#ifdef OVERRIDE_POSITION
	float4 position = float4(1.0);
#endif

#ifdef USE_MULTIVIEW
	float4x4 combined_projection = scene_data.projection_matrix;
	float4x4 projection_matrix = scene_data.projection_matrix_view[ViewIndex];
	float4x4 inv_projection_matrix = scene_data.inv_projection_matrix_view[ViewIndex];
	float3 eye_offset = scene_data.eye_offset[ViewIndex].xyz;
#else
	float4x4 projection_matrix = scene_data.projection_matrix;
	float4x4 inv_projection_matrix = scene_data.inv_projection_matrix;
	float3 eye_offset = float3(0.0, 0.0, 0.0);
#endif

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)

	vertex = (mul(float4(vertex, 1.0), model_matrix)).xyz;

#ifdef NORMAL_USED
	normal_highp = mul(normal_highp, model_normal_matrix);
#endif

#ifdef TANGENT_USED

	tangent = mul(tangent, model_normal_matrix);
	binormal = mul(binormal, model_normal_matrix);

#endif
#endif

#ifdef Z_CLIP_SCALE_USED
	float z_clip_scale = 1.0;
#endif

	float roughness_highp = 1.0;

	float4x4 read_view_matrix = transpose(float4x4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			float4(0.0, 0.0, 0.0, 1.0)));

#ifdef USE_DOUBLE_PRECISION
	float4x4 modelview = mul(model_matrix, read_view_matrix);

	float3 model_origin = model_matrix[3].xyz;
	if (sc_multimesh()) {
		modelview = mul(matrix, modelview);

		float3 instance_origin = mul(matrix[3].xyz, (float3x3)model_matrix);
		float3 instance_precision;
		model_origin = double_add_vec3(model_origin, model_precision, instance_origin, float3(0.0), instance_precision);
		model_precision = instance_precision;
	}

	float3 temp_precision;
	modelview[3].xyz = double_add_vec3(model_origin, model_precision, inv_view_matrix[3].xyz, view_precision, temp_precision);
	modelview[3].xyz = mul(modelview[3].xyz, (float3x3)read_view_matrix);
#else
	float4x4 modelview = mul(model_matrix, read_view_matrix);
#endif
	float3x3 modelview_normal = mul(model_normal_matrix, (float3x3)read_view_matrix);
	float2 read_viewport_size = scene_data.viewport_size;

#ifdef POINT_SIZE_USED
	float point_size = 1.0;
#endif

	{
#CODE : VERTEX
	}

	float roughness = roughness_highp;
#ifdef NORMAL_USED
	float3 normal = normal_highp;
#endif

#if !defined(SKIP_TRANSFORM_USED) && !defined(VERTEX_WORLD_COORDS_USED)

	vertex = (mul(float4(vertex, 1.0), modelview)).xyz;

#ifdef NORMAL_USED
	normal = mul(normal, modelview_normal);
#endif

#ifdef TANGENT_USED

	binormal = mul(binormal, modelview_normal);
	tangent = mul(tangent, modelview_normal);
#endif
#endif

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)

	vertex = (mul(float4(vertex, 1.0), read_view_matrix)).xyz;
#ifdef NORMAL_USED
	normal = (mul(float4(normal, 0.0), read_view_matrix)).xyz;
#endif

#ifdef TANGENT_USED
	binormal = (mul(float4(binormal, 0.0), read_view_matrix)).xyz;
	tangent = (mul(float4(tangent, 0.0), read_view_matrix)).xyz;
#endif
#endif

	vertex_interp = vertex;

#ifdef NORMAL_USED
	normal_interp = normalize(normal);
#endif

#ifdef TANGENT_USED
	tangent_interp = normalize(tangent);
	binormal_interp = normalize(binormal);
#endif

#ifdef MODE_RENDER_DEPTH

#ifdef MODE_DUAL_PARABOLOID

	vertex_interp.z *= scene_data.dual_paraboloid_side;

	dp_clip = vertex_interp.z;

	float3 vtx = vertex_interp;
	float distance = length(vtx);
	vtx = normalize(vtx);
	vtx.xy /= 1.0 - vtx.z;
	vtx.z = (distance / scene_data.z_far);
	vtx.z = vtx.z * 2.0 - 1.0;
	vertex_interp = vtx;

#endif

#endif

#ifdef OVERRIDE_POSITION
	vertex_position = position;
#else
	vertex_position = mul(float4(vertex_interp, 1.0), projection_matrix);
#endif

#ifdef USE_MULTIVIEW
	combined_projected = mul(float4(vertex_interp, 1.0), combined_projection);
#endif

#ifdef MOTION_VECTORS
	screen_pos = vertex_position;
	linear_depth = -vertex_interp.z;
#endif

#ifdef MODE_RENDER_DEPTH
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_PANCAKE_SHADOWS)) {
		if (vertex_position.z >= 0.9999) {
			vertex_position.z = 0.9999;
		}
	}
#endif
#ifdef MODE_RENDER_MATERIAL
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_UV2_MATERIAL)) {
		float2 uv_dest_attrib;
		if (any(uv_scale != float4(0.0))) {
			uv_dest_attrib = (uv2_attrib.xy - 0.5) * uv_scale.zw;
		} else {
			uv_dest_attrib = uv2_attrib.xy;
		}

		float2 uv_offset = float2(f16tof32(draw_call.uv_offset), f16tof32(draw_call.uv_offset >> 16));
		vertex_position.xy = (uv_dest_attrib + uv_offset) * 2.0 - 1.0;
		vertex_position.z = 0.00001;
		vertex_position.w = 1.0;
	}
#endif

#ifdef Z_CLIP_SCALE_USED
	if (!bool(scene_data.flags & SCENE_DATA_FLAGS_IN_SHADOW_PASS)) {
		vertex_position.z = lerp(vertex_position.w, vertex_position.z, z_clip_scale);
	}
#endif

#ifdef POINT_SIZE_USED
	if (sc_emulate_point_size) {
		float2 point_coords[6] = {
				float2(0, 1),
				float2(0, 0),
				float2(1, 1),
				float2(0, 0),
				float2(1, 0),
				float2(1, 1)};

		float2 point_coord = point_coords[vertex_index % 6];
		vertex_position.xy += (point_coord * 2.0 - 1.0) * (point_size / scene_data.viewport_size) * vertex_position.w;

#ifdef POINT_COORD_USED
		point_coord_interp = point_coord;
#endif
	} else {
		vertex_point_size = point_size;
	}
#endif
}

void _unpack_vertex_attributes(float4 p_vertex_in, float3 p_compressed_aabb_position, float3 p_compressed_aabb_size,
#if defined(NORMAL_USED) || defined(TANGENT_USED)
		float4 p_normal_in,
#ifdef NORMAL_USED
		out float3 r_normal,
#endif
		out float3 r_tangent,
		out float3 r_binormal,
#endif
		out float3 r_vertex) {

	r_vertex = p_vertex_in.xyz * p_compressed_aabb_size + p_compressed_aabb_position;
#ifdef NORMAL_USED
	r_normal = oct_to_vec3(p_normal_in.xy * 2.0 - 1.0);
#endif

#if defined(NORMAL_USED) || defined(TANGENT_USED)

	float binormal_sign;

	if (p_normal_in.z > 0.0 || p_normal_in.w < 1.0) {

		float2 signed_tangent_attrib = p_normal_in.zw * 2.0 - 1.0;
		r_tangent = oct_to_vec3(float2(signed_tangent_attrib.x, abs(signed_tangent_attrib.y) * 2.0 - 1.0));
		binormal_sign = sign(signed_tangent_attrib.y);
		r_binormal = normalize(cross(r_normal, r_tangent) * binormal_sign);
	} else {

		float angle = p_vertex_in.w;
		binormal_sign = angle > 0.5 ? 1.0 : -1.0;
		angle = abs(angle * 2.0 - 1.0) * M_PI;
		float3 axis = r_normal;
		axis_angle_to_tbn(axis, angle, r_tangent, r_binormal, r_normal);
		r_binormal *= binormal_sign;
	}
#endif
}

struct VertexInput {
	uint vertex_index : SV_VulkanVertexID;
	uint instance_index : SV_VulkanInstanceID;
#ifdef USE_MULTIVIEW
	uint view_index : SV_ViewID;
#endif
	[[vk::location(0)]] float4 vertex_angle_attrib;
#if (defined(NORMAL_USED) || defined(TANGENT_USED))
	[[vk::location(1)]] float4 axis_tangent_attrib;
#endif
#if (defined(COLOR_USED))
	[[vk::location(3)]] float4 color_attrib;
#endif
#if (defined(UV_USED))
	[[vk::location(4)]] float2 uv_attrib;
#endif
#if (defined(UV2_USED) || defined(USE_LIGHTMAP) || defined(MODE_RENDER_MATERIAL))
	[[vk::location(5)]] float2 uv2_attrib;
#endif
#if (defined(CUSTOM0_USED))
	[[vk::location(6)]] float4 custom0_attrib;
#endif
#if (defined(CUSTOM1_USED))
	[[vk::location(7)]] float4 custom1_attrib;
#endif
#if (defined(CUSTOM2_USED))
	[[vk::location(8)]] float4 custom2_attrib;
#endif
#if (defined(CUSTOM3_USED))
	[[vk::location(9)]] float4 custom3_attrib;
#endif
#if (defined(BONES_USED) || defined(USE_PARTICLE_TRAILS))
	[[vk::location(10)]] uint4 bone_attrib;
#endif
#if (defined(WEIGHTS_USED) || defined(USE_PARTICLE_TRAILS))
	[[vk::location(11)]] float4 weight_attrib;
#endif
#if (defined(MOTION_VECTORS))
	[[vk::location(12)]] float4 previous_vertex_attrib;
#endif
#if (defined(MOTION_VECTORS)) && (defined(NORMAL_USED) || defined(TANGENT_USED))
	[[vk::location(13)]] float4 previous_normal_attrib;
#endif
};
struct VertexOutput {
	float4 position : SV_Position;
#ifdef POINT_SIZE_USED
	float point_size : SV_PointSize;
#endif
	[[vk::location(0)]] float3 vertex_interp;
#if (defined(NORMAL_USED))
	[[vk::location(1)]] float3 normal_interp;
#endif
#if (defined(COLOR_USED))
	[[vk::location(2)]] float4 color_interp;
#endif
#if (defined(UV_USED))
	[[vk::location(3)]] float2 uv_interp;
#endif
#if (defined(UV2_USED) || defined(USE_LIGHTMAP))
	[[vk::location(4)]] float2 uv2_interp;
#endif
#if (defined(TANGENT_USED))
	[[vk::location(5)]] float3 tangent_interp;
#endif
#if (defined(TANGENT_USED))
	[[vk::location(6)]] float3 binormal_interp;
#endif
#if (defined(MOTION_VECTORS))
	[[vk::location(7)]] float4 screen_position;
#endif
#if (defined(MOTION_VECTORS))
	[[vk::location(8)]] float4 prev_screen_position;
#endif
#if (defined(MOTION_VECTORS)) && (defined(MODE_RTXDI_SURFACE))
	[[vk::location(15)]] float2 linear_depth_interp;
#endif
#if (defined(MODE_DUAL_PARABOLOID))
	[[vk::location(9)]] float dp_clip;
#endif
	nointerpolation [[vk::location(10)]] uint instance_index_interp;
#if (defined(USE_MULTIVIEW))
	[[vk::location(11)]] float4 combined_projected;
#endif
#if (defined(POINT_SIZE_USED) && defined(POINT_COORD_USED))
	[[vk::location(14)]] float2 point_coord_interp;
#endif
#CODE : VARYINGS
};
[shader("vertex")]
VertexOutput main(VertexInput stage_input) {
	vertex_index = stage_input.vertex_index;
	instance_index_builtin = stage_input.instance_index;
#ifdef USE_MULTIVIEW
	view_index = stage_input.view_index;
#endif
	vertex_angle_attrib = stage_input.vertex_angle_attrib;
#if (defined(NORMAL_USED) || defined(TANGENT_USED))
	axis_tangent_attrib = stage_input.axis_tangent_attrib;
#endif
#if (defined(COLOR_USED))
	color_attrib = stage_input.color_attrib;
#endif
#if (defined(UV_USED))
	uv_attrib = stage_input.uv_attrib;
#endif
#if (defined(UV2_USED) || defined(USE_LIGHTMAP) || defined(MODE_RENDER_MATERIAL))
	uv2_attrib = stage_input.uv2_attrib;
#endif
#if (defined(CUSTOM0_USED))
	custom0_attrib = stage_input.custom0_attrib;
#endif
#if (defined(CUSTOM1_USED))
	custom1_attrib = stage_input.custom1_attrib;
#endif
#if (defined(CUSTOM2_USED))
	custom2_attrib = stage_input.custom2_attrib;
#endif
#if (defined(CUSTOM3_USED))
	custom3_attrib = stage_input.custom3_attrib;
#endif
#if (defined(BONES_USED) || defined(USE_PARTICLE_TRAILS))
	bone_attrib = stage_input.bone_attrib;
#endif
#if (defined(WEIGHTS_USED) || defined(USE_PARTICLE_TRAILS))
	weight_attrib = stage_input.weight_attrib;
#endif
#if (defined(MOTION_VECTORS))
	previous_vertex_attrib = stage_input.previous_vertex_attrib;
#endif
#if (defined(MOTION_VECTORS)) && (defined(NORMAL_USED) || defined(TANGENT_USED))
	previous_normal_attrib = stage_input.previous_normal_attrib;
#endif

	uint instance_index = draw_call.instance_index;
	if (!sc_multimesh()) {
		instance_index += INSTANCE_INDEX;
	}

	instance_index_interp = instance_index;

#ifdef MOTION_VECTORS

	float prev_linear_depth;
	float3 prev_vertex;
#ifdef NORMAL_USED
	float3 prev_normal;
#endif
#if defined(NORMAL_USED) || defined(TANGENT_USED)
	float3 prev_tangent;
	float3 prev_binormal;
#endif

	_unpack_vertex_attributes(
			previous_vertex_attrib,
			instances[instance_index].compressed_aabb_position_pad.xyz,
			instances[instance_index].compressed_aabb_size_pad.xyz,

#if defined(NORMAL_USED) || defined(TANGENT_USED)
			previous_normal_attrib,
#ifdef NORMAL_USED
			prev_normal,
#endif
			prev_tangent,
			prev_binormal,
#endif
			prev_vertex);

	global_time = scene_data_block.prev_data.time;
	vertex_shader(prev_vertex,
#ifdef NORMAL_USED
			prev_normal,
#endif
#ifdef TANGENT_USED
			prev_tangent,
			prev_binormal,
#endif
			instance_index, draw_call.multimesh_motion_vectors_previous_offset, scene_data_block.prev_data, instances[instance_index].prev_transform,
#ifdef USE_DOUBLE_PRECISION
			instances[instance_index].prev_model_precision.xyz,
#endif
			prev_screen_position, prev_linear_depth);
#ifdef MODE_RTXDI_SURFACE
	linear_depth_interp.y = prev_linear_depth;
#endif
#else

	float4 screen_position;
#endif
	float linear_depth;

	float3 vertex;
#ifdef NORMAL_USED
	float3 normal;
#endif
#if defined(NORMAL_USED) || defined(TANGENT_USED)
	float3 tangent;
	float3 binormal;
#endif

	_unpack_vertex_attributes(
			vertex_angle_attrib,
			instances[instance_index].compressed_aabb_position_pad.xyz,
			instances[instance_index].compressed_aabb_size_pad.xyz,
#if defined(NORMAL_USED) || defined(TANGENT_USED)
			axis_tangent_attrib,
#ifdef NORMAL_USED
			normal,
#endif
			tangent,
			binormal,
#endif
			vertex);

	global_time = scene_data_block.data.time;
	global_prev_time = scene_data_block.prev_data.time;
	vertex_shader(vertex,
#ifdef NORMAL_USED
			normal,
#endif
#ifdef TANGENT_USED
			tangent,
			binormal,
#endif
			instance_index, draw_call.multimesh_motion_vectors_current_offset, scene_data_block.data, instances[instance_index].transform,
#ifdef USE_DOUBLE_PRECISION
			instances[instance_index].model_precision.xyz,
#endif

			screen_position, linear_depth);
#ifdef MODE_RTXDI_SURFACE
	linear_depth_interp.x = linear_depth;
#endif
	VertexOutput stage_output;
	stage_output.position = vertex_position;
#ifdef POINT_SIZE_USED
	stage_output.point_size = vertex_point_size;
#endif
	stage_output.vertex_interp = vertex_interp;
#if (defined(NORMAL_USED))
	stage_output.normal_interp = normal_interp;
#endif
#if (defined(COLOR_USED))
	stage_output.color_interp = color_interp;
#endif
#if (defined(UV_USED))
	stage_output.uv_interp = uv_interp;
#endif
#if (defined(UV2_USED) || defined(USE_LIGHTMAP))
	stage_output.uv2_interp = uv2_interp;
#endif
#if (defined(TANGENT_USED))
	stage_output.tangent_interp = tangent_interp;
#endif
#if (defined(TANGENT_USED))
	stage_output.binormal_interp = binormal_interp;
#endif
#if (defined(MOTION_VECTORS))
	stage_output.screen_position = screen_position;
#endif
#if (defined(MOTION_VECTORS))
	stage_output.prev_screen_position = prev_screen_position;
#endif
#if (defined(MOTION_VECTORS)) && (defined(MODE_RTXDI_SURFACE))
	stage_output.linear_depth_interp = linear_depth_interp;
#endif
#if (defined(MODE_DUAL_PARABOLOID))
	stage_output.dp_clip = dp_clip;
#endif
	stage_output.instance_index_interp = instance_index_interp;
#if (defined(USE_MULTIVIEW))
	stage_output.combined_projected = combined_projected;
#endif
#if (defined(POINT_SIZE_USED) && defined(POINT_COORD_USED))
	stage_output.point_coord_interp = point_coord_interp;
#endif
#CODE : VARYINGS_VERTEX
	return stage_output;
}

#[fragment]

#VERSION_DEFINES

#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0

#ifdef USE_MULTIVIEW
#define OUTPUT_IS_MULTIVIEW true
#else
#define OUTPUT_IS_MULTIVIEW false
#endif

static uint view_index;
static float4 fragment_coord;
static float fragment_depth;
static bool front_facing_builtin;
static float2 raster_point_coord;
#include "scene_forward_clustered_inc.slang"

static float3 vertex_interp;

#ifdef NORMAL_USED
static float3 normal_interp;
#endif

#if defined(COLOR_USED)
static float4 color_interp;
#endif

#ifdef UV_USED
static float2 uv_interp;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
static float2 uv2_interp;
#endif

#ifdef TANGENT_USED
static float3 tangent_interp;
static float3 binormal_interp;
#endif

#ifdef MOTION_VECTORS
static float4 screen_position;
static float4 prev_screen_position;
#ifdef MODE_RTXDI_SURFACE
static float2 linear_depth_interp;
#endif
#endif

#ifdef MODE_DUAL_PARABOLOID

static float dp_clip;

#endif

static uint instance_index_interp;

#ifdef USE_LIGHTMAP

#endif

#ifdef USE_MULTIVIEW
#define ViewIndex view_index
float3 multiview_uv(float2 uv) {
	return float3(uv, ViewIndex);
}
int3 multiview_uv(int2 uv) {
	return int3(uv, int(ViewIndex));
}
int4 godot_multiview_load_coord(int3 uv, int lod) { return int4(uv, lod); }
static float4 combined_projected;
#else
#define ViewIndex 0
float2 multiview_uv(float2 uv) {
	return uv;
}
int2 multiview_uv(int2 uv) {
	return uv;
}
int3 godot_multiview_load_coord(int2 uv, int lod) { return int3(uv, lod); }
#endif

#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
static float2 point_coord_interp;
#endif

#ifdef USE_MULTIVIEW
#define projection_matrix scene_data.projection_matrix_view[ViewIndex]
#define inv_projection_matrix scene_data.inv_projection_matrix_view[ViewIndex]
#else
#define projection_matrix scene_data.projection_matrix
#define inv_projection_matrix scene_data.inv_projection_matrix
#endif

#define global_time scene_data_block.data.time
#define global_prev_time scene_data_block.prev_data.time

#if defined(ENABLE_SSS) && defined(ENABLE_TRANSMITTANCE)

#define LIGHT_TRANSMITTANCE_USED
#endif

#ifdef MATERIAL_UNIFORMS_USED

struct MaterialUniforms {
#MATERIAL_UNIFORMS
};
[[vk::binding(0, MATERIAL_UNIFORM_SET)]] ConstantBuffer<MaterialUniforms> material;

#endif

#GLOBALS

#ifdef MODE_RTXDI_SURFACE
static float4 rtxdi_base_output;
static float4 rtxdi_shading_output;
static float4 rtxdi_emission_output;
static float4 rtxdi_motion_output;
static uint rtxdi_geometry_output;
static uint2 rtxdi_classification_output;
#elif defined(MODE_RENDER_DEPTH)

#ifdef MODE_RENDER_MATERIAL

static float4 albedo_output_buffer;
static float4 normal_output_buffer;
static float4 orm_output_buffer;
static float4 emission_output_buffer;
static float depth_output_buffer;

#endif

#ifdef MODE_RENDER_NORMAL_ROUGHNESS
static float4 normal_roughness_output_buffer;

#ifdef MODE_RENDER_VOXEL_GI
static uint2 voxel_gi_buffer;
#endif

#endif
#endif

#if defined(MOTION_VECTORS) && !defined(MODE_RTXDI_SURFACE)
static float2 motion_vector;
#endif

#include "../scene_forward_aa_inc.slang"

#ifndef MODE_RENDER_DEPTH

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;

	item_from = item_min >> 5;
	item_to = (item_max == 0) ? 0 : ((item_max - 1) >> 5) + 1;
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return (mask_width == 32 ? 0xFFFFFFFFu : ((1u << mask_width) - 1u)) << local_min;
}

#endif

#if defined(MODE_RENDER_NORMAL_ROUGHNESS) || defined(MODE_RENDER_MATERIAL)

float3 encode24(float3 v) {

	float3 vNormalUns = abs(v);

	float maxNAbs = max(vNormalUns.z, max(vNormalUns.x, vNormalUns.y));

	float2 vTexCoord = vNormalUns.z < maxNAbs ? (vNormalUns.y < maxNAbs ? vNormalUns.yz : vNormalUns.xz) : vNormalUns.xy;
	vTexCoord /= maxNAbs;
	vTexCoord = vTexCoord.x < vTexCoord.y ? vTexCoord.yx : vTexCoord.xy;

	vTexCoord.y /= vTexCoord.x;
	float fFittingScale = best_fit_normal_texture.Sample(SAMPLER_NEAREST_CLAMP, vTexCoord).r;

	float3 result = v / maxNAbs;

	result *= fFittingScale;
	return result;
}
#endif

void fragment_shader(in SceneData scene_data) {
	uint instance_index = instance_index_interp;

#ifdef PREMUL_ALPHA_USED
	float premul_alpha = 1.0;
#endif

	float3 vertex = vertex_interp;
#ifdef MODE_RTXDI_SURFACE
	float3 rtxdi_geometric_normal = normalize(cross(ddx(vertex_interp), ddy(vertex_interp)));
	float3 rtxdi_incident = projection_matrix[3][3] == 0.0 ? vertex_interp : float3(0.0, 0.0, -1.0);
	rtxdi_geometric_normal = faceforward(rtxdi_geometric_normal, rtxdi_incident, rtxdi_geometric_normal);
#endif
#ifdef USE_MULTIVIEW
	float3 eye_offset = scene_data.eye_offset[ViewIndex].xyz;
	float3 view_highp = -normalize(vertex_interp - eye_offset);

	float2 combined_uv = (combined_projected.xy / combined_projected.w) * 0.5 + 0.5;
#else
	float3 eye_offset = float3(0.0, 0.0, 0.0);
	float3 view_highp = -normalize(vertex_interp);
#endif
	float3 albedo_highp = float3(1.0);
	float3 backlight = float3(0.0);
	float4 transmittance_color = float4(0.0, 0.0, 0.0, 1.0);
	float transmittance_depth = 0.0;
	float transmittance_boost = 0.0;
	float metallic_highp = 0.0;
	float specular = 0.5;
	float3 emission = float3(0.0);
	float roughness_highp = 1.0;
	float rim = 0.0;
	float rim_tint = 0.0;
	float clearcoat = 0.0;
	float clearcoat_roughness = 0.0;
	float anisotropy = 0.0;
	float2 anisotropy_flow = float2(1.0, 0.0);
	float4 fog = float4(0.0, 0.0, 0.0, 1.0);
#if defined(CUSTOM_RADIANCE_USED)
	float4 custom_radiance = float4(0.0);
#endif
#if defined(CUSTOM_IRRADIANCE_USED)
	float4 custom_irradiance = float4(0.0);
#endif

	float ao = 1.0;
	float ao_light_affect = 0.0;

	float alpha_highp = float(instances[instance_index].flags >> INSTANCE_FLAGS_FADE_SHIFT) / float(255.0);

#ifdef TANGENT_USED
	float3 binormal = binormal_interp;
	float3 tangent = tangent_interp;
#else
	float3 binormal = float3(0.0);
	float3 tangent = float3(0.0);
#endif

#ifdef NORMAL_USED
	float3 normal_highp = normal_interp;
#if defined(DO_SIDE_CHECK)
	if (!front_facing_builtin) {
		normal_highp = -normal_highp;
	}
#endif
#endif

#ifdef UV_USED
	float2 uv = uv_interp;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	float2 uv2 = uv2_interp;
#endif

#if defined(COLOR_USED)
	float4 color = color_interp;
#endif

#if defined(NORMAL_MAP_USED)

	float3 normal_map = float3(0.5);
#endif

#if defined(BENT_NORMAL_MAP_USED)
	float3 bent_normal_vector = float3(0.5);
	float3 bent_normal_map = float3(0.5);
#endif

	float normal_map_depth = 1.0;

	float2 screen_uv = fragment_coord.xy * scene_data.screen_pixel_size;

	float sss_strength = 0.0;

#ifdef ALPHA_SCISSOR_USED
	float alpha_scissor_threshold = 1.0;
#endif

#ifdef ALPHA_HASH_USED
	float alpha_hash_scale = 1.0;
#endif

#ifdef ALPHA_ANTIALIASING_EDGE_USED
	float alpha_antialiasing_edge = 0.0;
	float2 alpha_texture_coordinate = float2(0.0, 0.0);
#endif

	float4x4 inv_view_matrix = transpose(float4x4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			float4(0.0, 0.0, 0.0, 1.0)));
	float4x4 read_model_matrix = transpose(float4x4(instances[instance_index].transform[0],
			instances[instance_index].transform[1],
			instances[instance_index].transform[2],
			float4(0.0, 0.0, 0.0, 1.0)));

#ifdef LIGHT_VERTEX_USED
	float3 light_vertex = vertex;
#endif

	float3x3 model_normal_matrix;
	if (bool(instances[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(godot_inverse((float3x3)read_model_matrix));
	} else {
		model_normal_matrix = (float3x3)read_model_matrix;
	}

	float4x4 read_view_matrix = transpose(float4x4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			float4(0.0, 0.0, 0.0, 1.0)));
	float2 read_viewport_size = scene_data.viewport_size;

#ifdef POINT_COORD_USED
#ifdef POINT_SIZE_USED
	float2 point_coord;
	if (sc_emulate_point_size) {
		point_coord = point_coord_interp;
	} else {
		point_coord = raster_point_coord;
	}
#else
	float2 point_coord = float2(0.5);
#endif
#endif

	{
#CODE : FRAGMENT
	}

	float roughness = roughness_highp;
	float metallic = metallic_highp;
	float3 albedo = albedo_highp;
	float alpha = alpha_highp;
#ifdef NORMAL_USED
	float3 normal = normal_highp;
#endif

#ifdef LIGHT_TRANSMITTANCE_USED
	transmittance_color.a *= sss_strength;
#endif

#ifdef LIGHT_VERTEX_USED
	vertex = light_vertex;
#ifdef USE_MULTIVIEW
	float3 view = -normalize(vertex - eye_offset);
#else
	float3 view = -normalize(vertex);
#endif
#else
	float3 view = view_highp;
#endif

#ifdef NORMAL_USED
	float3 geo_normal = normalize(normal);
#endif

#ifndef USE_SHADOW_TO_OPACITY

#ifdef ALPHA_SCISSOR_USED
#ifdef MODE_RENDER_MATERIAL
	if (alpha < alpha_scissor_threshold) {
		alpha = 0.0;
	} else {
		alpha = 1.0;
	}
#else
	if (alpha < alpha_scissor_threshold) {
		discard;
	}
#endif
#endif

#ifdef ALPHA_HASH_USED
	float3 object_pos = (mul(mul(float4(vertex, 1.0), inv_view_matrix), godot_inverse(read_model_matrix))).xyz;
#ifdef MODE_RENDER_MATERIAL
	if (alpha < compute_alpha_hash_threshold(object_pos, alpha_hash_scale)) {
		alpha = 0.0;
	} else {
		alpha = 1.0;
	}
#else
	if (alpha < compute_alpha_hash_threshold(object_pos, alpha_hash_scale)) {
		discard;
	}
#endif
#endif

#if (defined(ALPHA_SCISSOR_USED) || defined(ALPHA_HASH_USED)) && !defined(ALPHA_ANTIALIASING_EDGE_USED) && !defined(MODE_RENDER_MATERIAL)
	alpha = 1.0;
#endif

#ifdef ALPHA_ANTIALIASING_EDGE_USED

#ifdef ALPHA_SCISSOR_USED
	alpha_antialiasing_edge = clamp(alpha_scissor_threshold + alpha_antialiasing_edge, 0.0, 1.0);
#endif
	alpha = compute_alpha_antialiasing_edge(alpha, alpha_texture_coordinate, alpha_antialiasing_edge);
#endif

#ifdef MODE_RENDER_DEPTH
#if defined(USE_OPAQUE_PREPASS) || defined(ALPHA_ANTIALIASING_EDGE_USED)
	if (alpha < scene_data.opaque_prepass_threshold) {
		discard;
	}
#endif
#endif

#endif

#if defined(NORMAL_MAP_USED)
	normal_map.xy = normal_map.xy * 2.0 - 1.0;
	normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy)));

	normal = normalize(lerp(normal, tangent * normal_map.x + binormal * normal_map.y + normal * normal_map.z, normal_map_depth));
#elif defined(NORMAL_USED)
	normal = geo_normal;
#endif

#ifdef BENT_NORMAL_MAP_USED
	bent_normal_map.xy = bent_normal_map.xy * 2.0 - 1.0;
	bent_normal_map.z = sqrt(max(0.0, 1.0 - dot(bent_normal_map.xy, bent_normal_map.xy)));

	bent_normal_vector = normalize(tangent * bent_normal_map.x + binormal * bent_normal_map.y + normal * bent_normal_map.z);
#endif

#ifdef LIGHT_ANISOTROPY_USED

	if (anisotropy > 0.01) {
		float3x3 rot = float3x3(normalize(tangent), normalize(binormal), normal);

		tangent = normalize(mul(float3(anisotropy_flow.x, anisotropy_flow.y, 0.0), rot));
		binormal = normalize(mul(float3(-anisotropy_flow.y, anisotropy_flow.x, 0.0), rot));
	}

#endif

#ifdef ENABLE_CLIP_ALPHA
#ifdef MODE_RENDER_MATERIAL
	if (albedo.a < 0.99) {

		albedo.a = 0.0;
		alpha = 0.0;
	} else {
		albedo.a = 1.0;
		alpha = 1.0;
	}
#else
	if (albedo.a < 0.99) {

		discard;
	}
#endif
#endif

#ifndef MODE_RENDER_DEPTH

#endif

#ifndef MODE_RENDER_DEPTH

#ifdef USE_MULTIVIEW
	uint2 cluster_pos = uint2(combined_uv.xy / scene_data.screen_pixel_size) >> implementation_data.cluster_shift;
#else
	uint2 cluster_pos = uint2(fragment_coord.xy) >> implementation_data.cluster_shift;
#endif
	uint cluster_offset = (implementation_data.cluster_width * cluster_pos.y + cluster_pos.x) * (implementation_data.max_cluster_element_count_div_32 + 32);

	uint cluster_z = uint(clamp((-vertex.z / scene_data.z_far) * 32.0, 0.0, 31.0));

	float3 vertex_ddx = ddx(vertex);
	float3 vertex_ddy = ddy(vertex);

	{

		uint cluster_decal_offset = cluster_offset + implementation_data.cluster_type_size * 3;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_decal_offset + implementation_data.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		item_from = WaveReadLaneFirst(WaveActiveMin(item_from));
		item_to = WaveReadLaneFirst(WaveActiveMax(item_to));

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer[cluster_decal_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			uint merged_mask = WaveReadLaneFirst(WaveActiveBitOr(mask));
			while (merged_mask != 0) {
				uint bit = uint(firstbithigh(merged_mask));
				merged_mask &= ~(1u << bit);

				if (((1u << bit) & mask) == 0) {
					continue;
				}

				uint decal_index = 32 * i + bit;

				if (!bool(decals[decal_index].mask & instances[instance_index].layer_mask)) {
					continue;
				}

				float3 uv_local = (mul(float4(vertex, 1.0), decals[decal_index].xform)).xyz;
				if (any(uv_local < float3(0.0, -1.0, 0.0)) || any(uv_local > float3(1.0))) {
					continue;
				}

				float fade = pow(1.0 - (uv_local.y > 0.0 ? uv_local.y : -uv_local.y), uv_local.y > 0.0 ? decals[decal_index].upper_fade : decals[decal_index].lower_fade);

				if (decals[decal_index].normal_fade > 0.0) {
					fade *= smoothstep(decals[decal_index].normal_fade, 1.0, dot(geo_normal, decals[decal_index].normal) * 0.5 + 0.5);
				}

				float2 ddx = (mul(float4(vertex_ddx, 0.0), decals[decal_index].xform)).xz;
				float2 ddy = (mul(float4(vertex_ddy, 0.0), decals[decal_index].xform)).xz;

				if (any(decals[decal_index].albedo_rect != float4(0.0))) {

					float4 decal_albedo;
					if (sc_decal_use_mipmaps()) {
						decal_albedo = decal_atlas_srgb.SampleGrad(decal_sampler, uv_local.xz * decals[decal_index].albedo_rect.zw + decals[decal_index].albedo_rect.xy, ddx * decals[decal_index].albedo_rect.zw, ddy * decals[decal_index].albedo_rect.zw);
					} else {
						decal_albedo = decal_atlas_srgb.SampleLevel(decal_sampler, uv_local.xz * decals[decal_index].albedo_rect.zw + decals[decal_index].albedo_rect.xy, 0.0);
					}
					decal_albedo *= decals[decal_index].modulate;
					decal_albedo.a *= fade;
					albedo = lerp(albedo, decal_albedo.rgb, decal_albedo.a * decals[decal_index].albedo_mix);

					if (any(decals[decal_index].normal_rect != float4(0.0))) {
						float3 decal_normal;
						if (sc_decal_use_mipmaps()) {
							decal_normal = decal_atlas.SampleGrad(decal_sampler, uv_local.xz * decals[decal_index].normal_rect.zw + decals[decal_index].normal_rect.xy, ddx * decals[decal_index].normal_rect.zw, ddy * decals[decal_index].normal_rect.zw).xyz;
						} else {
							decal_normal = decal_atlas.SampleLevel(decal_sampler, uv_local.xz * decals[decal_index].normal_rect.zw + decals[decal_index].normal_rect.xy, 0.0).xyz;
						}
						decal_normal.xy = decal_normal.xy * float2(2.0, -2.0) - float2(1.0, -1.0);
						decal_normal.z = sqrt(max(0.0, 1.0 - dot(decal_normal.xy, decal_normal.xy)));

						decal_normal = (mul(decal_normal.xzy, decals[decal_index].normal_xform)).xyz;

						normal = normalize(lerp(normal, decal_normal, decal_albedo.a));
					}

					if (any(decals[decal_index].orm_rect != float4(0.0))) {
						float3 decal_orm;
						if (sc_decal_use_mipmaps()) {
							decal_orm = decal_atlas.SampleGrad(decal_sampler, uv_local.xz * decals[decal_index].orm_rect.zw + decals[decal_index].orm_rect.xy, ddx * decals[decal_index].orm_rect.zw, ddy * decals[decal_index].orm_rect.zw).xyz;
						} else {
							decal_orm = decal_atlas.SampleLevel(decal_sampler, uv_local.xz * decals[decal_index].orm_rect.zw + decals[decal_index].orm_rect.xy, 0.0).xyz;
						}
						ao = lerp(ao, decal_orm.r, decal_albedo.a);
						roughness = lerp(roughness, decal_orm.g, decal_albedo.a);
						metallic = lerp(metallic, decal_orm.b, decal_albedo.a);
					}
				}

				if (any(decals[decal_index].emission_rect != float4(0.0))) {

					if (sc_decal_use_mipmaps()) {
						emission += decal_atlas_srgb.SampleGrad(decal_sampler, uv_local.xz * decals[decal_index].emission_rect.zw + decals[decal_index].emission_rect.xy, ddx * decals[decal_index].emission_rect.zw, ddy * decals[decal_index].emission_rect.zw).xyz * decals[decal_index].modulate.rgb * decals[decal_index].emission_energy * fade;
					} else {
						emission += decal_atlas_srgb.SampleLevel(decal_sampler, uv_local.xz * decals[decal_index].emission_rect.zw + decals[decal_index].emission_rect.xy, 0.0).xyz * decals[decal_index].modulate.rgb * decals[decal_index].emission_energy * fade;
					}
				}
			}
		}
	}

#endif

#ifdef NORMAL_USED
	if (bool(scene_data.flags & SCENE_DATA_FLAGS_USE_ROUGHNESS_LIMITER)) {

		float roughness2 = roughness * roughness;
		float3 dndu = ddx(normal), dndv = ddy(normal);
		float variance = scene_data.roughness_limiter_amount * (dot(dndu, dndu) + dot(dndv, dndv));
		float kernelRoughness2 = min(2.0 * variance, scene_data.roughness_limiter_limit);
		float filteredRoughness2 = min(1.0, roughness2 + kernelRoughness2);
		roughness = sqrt(filteredRoughness2);

		if (roughness < 0.00000001) {
			roughness = 0.0;
		}
	}
#endif

	float3 ambient_light = float3(0.0, 0.0, 0.0);
#ifndef MODE_UNSHADED

	emission *= scene_data.emissive_exposure_normalization;
#endif

#ifdef USE_SHADOW_TO_OPACITY
#ifndef MODE_RENDER_DEPTH
	alpha = min(alpha, clamp(length(ambient_light), 0.0, 1.0));

#if defined(ALPHA_SCISSOR_USED)
#ifdef MODE_RENDER_MATERIAL
	if (alpha < alpha_scissor_threshold) {
		alpha = 0.0;
	} else {
		alpha = 1.0;
	}
#else
	if (alpha < alpha_scissor_threshold) {
		discard;
	}
#endif
#endif

#endif
#endif

#ifdef MODE_RTXDI_SURFACE
	float2 position_clip = (screen_position.xy / screen_position.w) - scene_data.taa_jitter;
	float2 prev_position_clip = (prev_screen_position.xy / prev_screen_position.w) - scene_data_block.prev_data.taa_jitter;
	SurfaceData surface;
	surface.world_position = mul(float4(vertex, 1.0), inv_view_matrix).xyz;
	surface.normal = normalize(mul(normal, (float3x3)inv_view_matrix));
	surface.geometry_normal = normalize(mul(rtxdi_geometric_normal, (float3x3)inv_view_matrix));
	surface.view_direction = normalize(mul(view, (float3x3)inv_view_matrix));
	surface.layer_mask = instances[instance_index].layer_mask;
	surface.material.base_color = albedo;
	surface.material.opacity = alpha;
	surface.material.roughness = roughness;
	surface.material.metallic = metallic;
	surface.material.specular = specular;
	surface.material.emission = emission;
	surface.material.ambient_occlusion = ao;
	surface.material.flags = instances[instance_index].rtxdi_material_flags;
	SurfaceBufferData packed_surface = encode_surface_buffers(surface);
	rtxdi_base_output = packed_surface.base;
	rtxdi_shading_output = packed_surface.shading;
	rtxdi_emission_output = packed_surface.emission;
	rtxdi_motion_output = float4((prev_position_clip - position_clip) * 0.5 * scene_data.viewport_size, linear_depth_interp.y - linear_depth_interp.x, packed_surface.ambient_occlusion);
	rtxdi_geometry_output = packed_surface.geometry;
	rtxdi_classification_output = packed_surface.classification;
#elif defined(MODE_RENDER_DEPTH)

#ifdef MODE_RENDER_SDF

	{
		float3 local_pos = (mul(float4(vertex, 1.0), implementation_data.sdf_to_bounds)).xyz;
		int3 grid_pos = implementation_data.sdf_offset + int3(local_pos * float3(implementation_data.sdf_size));

		uint albedo16 = 0x1;
		albedo16 |= clamp(uint(albedo.r * 31.0), 0, 31) << 11;
		albedo16 |= clamp(uint(albedo.g * 31.0), 0, 31) << 6;
		albedo16 |= clamp(uint(albedo.b * 31.0), 0, 31) << 1;

		albedo_volume_grid[grid_pos] = albedo16;

		uint facing_bits = 0;
		const float3 aniso_dir[6] = {
				float3(1, 0, 0),
				float3(0, 1, 0),
				float3(0, 0, 1),
				float3(-1, 0, 0),
				float3(0, -1, 0),
				float3(0, 0, -1)};

		float3 cam_normal = mul(normalize(normal_interp), (float3x3)inv_view_matrix);

		float closest_dist = -1e20;

		for (uint i = 0; i < 6; i++) {
			float d = dot(cam_normal, aniso_dir[i]);
			if (d > closest_dist) {
				closest_dist = d;
				facing_bits = (1 << i);
			}
		}

#ifdef NO_IMAGE_ATOMICS
		geom_facing_grid[grid_pos] |= facing_bits;
#else
		InterlockedOr(geom_facing_grid[grid_pos], facing_bits);
#endif

		if (length(emission) > 0.001) {
			float lumas[6];
			float3 light_total = float3(0);

			for (int i = 0; i < 6; i++) {
				float strength = max(0.0, dot(cam_normal, aniso_dir[i]));
				float3 light = emission * strength;
				light_total += light;
				lumas[i] = max(light.r, max(light.g, light.b));
			}

			float luma_total = max(light_total.r, max(light_total.g, light_total.b));

			uint light_aniso = 0;

			for (int i = 0; i < 6; i++) {
				light_aniso |= min(31, uint((lumas[i] / luma_total) * 31.0)) << (i * 5);
			}

			const float pow2to9 = 512.0f;
			const float B = 15.0f;
			const float N = 9.0f;
			const float LN2 = 0.6931471805599453094172321215;

			float cRed = clamp(light_total.r, 0.0, 65408.0);
			float cGreen = clamp(light_total.g, 0.0, 65408.0);
			float cBlue = clamp(light_total.b, 0.0, 65408.0);

			float cMax = max(cRed, max(cGreen, cBlue));

			float expp = max(-B - 1.0f, floor(log(cMax) / LN2)) + 1.0f + B;

			float sMax = floor((cMax / pow(2.0f, expp - B - N)) + 0.5f);

			float exps = expp + 1.0f;

			if (0.0 <= sMax && sMax < pow2to9) {
				exps = expp;
			}

			float sRed = floor((cRed / pow(2.0f, exps - B - N)) + 0.5f);
			float sGreen = floor((cGreen / pow(2.0f, exps - B - N)) + 0.5f);
			float sBlue = floor((cBlue / pow(2.0f, exps - B - N)) + 0.5f);

			uint light_rgbe = ((uint(sRed) & 0x1FFu) >> 1) | ((uint(sGreen) & 0x1FFu) << 8) | (((uint(sBlue) & 0x1FFu) >> 1) << 17) | ((uint(exps) & 0x1Fu) << 25);

			emission_grid[grid_pos] = light_rgbe;
			emission_aniso_grid[grid_pos] = light_aniso;
		}
	}

#endif

#ifdef MODE_RENDER_MATERIAL

	albedo_output_buffer.rgb = albedo;
	albedo_output_buffer.a = alpha;

	normal_output_buffer.rgb = encode24(normal) * 0.5 + 0.5;
	normal_output_buffer.a = 0.0;
	depth_output_buffer.r = -vertex.z;

	orm_output_buffer.r = ao;
	orm_output_buffer.g = roughness;
	orm_output_buffer.b = metallic;
	orm_output_buffer.a = sss_strength;

	emission_output_buffer.rgb = emission;
	emission_output_buffer.a = 0.0;
#endif

#ifdef MODE_RENDER_NORMAL_ROUGHNESS
	normal_roughness_output_buffer = float4(encode24(normal) * 0.5 + 0.5, roughness);

	normal_roughness_output_buffer.w = normal_roughness_output_buffer.w * (127.0 / 255.0);
	if (bool(instances[instance_index].flags & INSTANCE_FLAGS_DYNAMIC)) {
		normal_roughness_output_buffer.w = 1.0 - normal_roughness_output_buffer.w;
	}
	normal_roughness_output_buffer.w = normal_roughness_output_buffer.w;

#ifdef MODE_RENDER_VOXEL_GI
	if (bool(instances[instance_index].flags & INSTANCE_FLAGS_USE_VOXEL_GI)) {
		uint index1 = instances[instance_index].gi_offset & 0xFFFF;
		uint index2 = instances[instance_index].gi_offset >> 16;
		voxel_gi_buffer.x = index1 & 0xFFu;
		voxel_gi_buffer.y = index2 & 0xFFu;
	} else {
		voxel_gi_buffer.x = 0xFF;
		voxel_gi_buffer.y = 0xFF;
	}
#endif

#endif

#endif
#if defined(MOTION_VECTORS) && !defined(MODE_RTXDI_SURFACE)
	float2 position_clip = (screen_position.xy / screen_position.w) - scene_data.taa_jitter;
	float2 prev_position_clip = (prev_screen_position.xy / prev_screen_position.w) - scene_data_block.prev_data.taa_jitter;

	float2 position_uv = position_clip * float2(0.5, 0.5);
	float2 prev_position_uv = prev_position_clip * float2(0.5, 0.5);

	motion_vector = prev_position_uv - position_uv;
#endif
}

struct FragmentInput {
	float4 position : SV_Position;
	bool front_facing : SV_IsFrontFace;
#ifdef USE_MULTIVIEW
	uint view_index : SV_ViewID;
#endif
#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
	float2 point_coord : SV_PointCoord;
#endif
	[[vk::location(0)]] float3 vertex_interp;
#if (defined(NORMAL_USED))
	[[vk::location(1)]] float3 normal_interp;
#endif
#if (defined(COLOR_USED))
	[[vk::location(2)]] float4 color_interp;
#endif
#if (defined(UV_USED))
	[[vk::location(3)]] float2 uv_interp;
#endif
#if (defined(UV2_USED) || defined(USE_LIGHTMAP))
	[[vk::location(4)]] float2 uv2_interp;
#endif
#if (defined(TANGENT_USED))
	[[vk::location(5)]] float3 tangent_interp;
#endif
#if (defined(TANGENT_USED))
	[[vk::location(6)]] float3 binormal_interp;
#endif
#if (defined(MOTION_VECTORS))
	[[vk::location(7)]] float4 screen_position;
#endif
#if (defined(MOTION_VECTORS))
	[[vk::location(8)]] float4 prev_screen_position;
#endif
#if (defined(MOTION_VECTORS)) && (defined(MODE_RTXDI_SURFACE))
	[[vk::location(15)]] float2 linear_depth_interp;
#endif
#if (defined(MODE_DUAL_PARABOLOID))
	[[vk::location(9)]] float dp_clip;
#endif
	nointerpolation [[vk::location(10)]] uint instance_index_interp;
#if (defined(USE_MULTIVIEW))
	[[vk::location(11)]] float4 combined_projected;
#endif
#if (defined(POINT_SIZE_USED) && defined(POINT_COORD_USED))
	[[vk::location(14)]] float2 point_coord_interp;
#endif
#CODE : VARYINGS
};
struct FragmentOutput {
#ifdef DEPTH_USED
	float depth : SV_Depth;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	[[vk::location(0)]] float4 rtxdi_base_output : SV_Target0;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	[[vk::location(1)]] float4 rtxdi_shading_output : SV_Target1;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	[[vk::location(2)]] float4 rtxdi_emission_output : SV_Target2;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	[[vk::location(3)]] float4 rtxdi_motion_output : SV_Target3;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	[[vk::location(4)]] uint rtxdi_geometry_output : SV_Target4;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	[[vk::location(5)]] uint2 rtxdi_classification_output : SV_Target5;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	[[vk::location(0)]] float4 albedo_output_buffer : SV_Target0;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	[[vk::location(1)]] float4 normal_output_buffer : SV_Target1;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	[[vk::location(2)]] float4 orm_output_buffer : SV_Target2;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	[[vk::location(3)]] float4 emission_output_buffer : SV_Target3;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	[[vk::location(4)]] float depth_output_buffer : SV_Target4;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_NORMAL_ROUGHNESS))
	[[vk::location(0)]] float4 normal_roughness_output_buffer : SV_Target0;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_NORMAL_ROUGHNESS)) && (defined(MODE_RENDER_VOXEL_GI))
	[[vk::location(1)]] uint2 voxel_gi_buffer : SV_Target1;
#endif
#if (defined(MOTION_VECTORS) && !defined(MODE_RTXDI_SURFACE))
	[[vk::location(2)]] float2 motion_vector : SV_Target2;
#endif
};
[shader("fragment")]
FragmentOutput main(FragmentInput stage_input) {
	fragment_coord = stage_input.position;
	fragment_depth = fragment_coord.z;
	front_facing_builtin = stage_input.front_facing;
#ifdef USE_MULTIVIEW
	view_index = stage_input.view_index;
#endif
#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
	raster_point_coord = stage_input.point_coord;
#endif
#CODE : VARYINGS_FRAGMENT
	vertex_interp = stage_input.vertex_interp;
#if (defined(NORMAL_USED))
	normal_interp = stage_input.normal_interp;
#endif
#if (defined(COLOR_USED))
	color_interp = stage_input.color_interp;
#endif
#if (defined(UV_USED))
	uv_interp = stage_input.uv_interp;
#endif
#if (defined(UV2_USED) || defined(USE_LIGHTMAP))
	uv2_interp = stage_input.uv2_interp;
#endif
#if (defined(TANGENT_USED))
	tangent_interp = stage_input.tangent_interp;
#endif
#if (defined(TANGENT_USED))
	binormal_interp = stage_input.binormal_interp;
#endif
#if (defined(MOTION_VECTORS))
	screen_position = stage_input.screen_position;
#endif
#if (defined(MOTION_VECTORS))
	prev_screen_position = stage_input.prev_screen_position;
#endif
#if (defined(MOTION_VECTORS)) && (defined(MODE_RTXDI_SURFACE))
	linear_depth_interp = stage_input.linear_depth_interp;
#endif
#if (defined(MODE_DUAL_PARABOLOID))
	dp_clip = stage_input.dp_clip;
#endif
	instance_index_interp = stage_input.instance_index_interp;
#if (defined(USE_MULTIVIEW))
	combined_projected = stage_input.combined_projected;
#endif
#if (defined(POINT_SIZE_USED) && defined(POINT_COORD_USED))
	point_coord_interp = stage_input.point_coord_interp;
#endif

#ifdef UBERSHADER
	bool front_facing = front_facing_builtin;
	if (uc_cull_mode() == POLYGON_CULL_BACK && !front_facing) {
		discard;
	} else if (uc_cull_mode() == POLYGON_CULL_FRONT && front_facing) {
		discard;
	}
#endif
#ifdef MODE_DUAL_PARABOLOID

	if (dp_clip > 0.0) {
		discard;
	}
#endif

	fragment_shader(scene_data_block.data);
	FragmentOutput stage_output;
#ifdef DEPTH_USED
	stage_output.depth = fragment_depth;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	stage_output.rtxdi_base_output = rtxdi_base_output;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	stage_output.rtxdi_shading_output = rtxdi_shading_output;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	stage_output.rtxdi_emission_output = rtxdi_emission_output;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	stage_output.rtxdi_motion_output = rtxdi_motion_output;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	stage_output.rtxdi_geometry_output = rtxdi_geometry_output;
#endif
#if (defined(MODE_RTXDI_SURFACE))
	stage_output.rtxdi_classification_output = rtxdi_classification_output;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	stage_output.albedo_output_buffer = albedo_output_buffer;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	stage_output.normal_output_buffer = normal_output_buffer;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	stage_output.orm_output_buffer = orm_output_buffer;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	stage_output.emission_output_buffer = emission_output_buffer;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_MATERIAL))
	stage_output.depth_output_buffer = depth_output_buffer;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_NORMAL_ROUGHNESS))
	stage_output.normal_roughness_output_buffer = normal_roughness_output_buffer;
#endif
#if (!(defined(MODE_RTXDI_SURFACE)) && (defined(MODE_RENDER_DEPTH))) && (defined(MODE_RENDER_NORMAL_ROUGHNESS)) && (defined(MODE_RENDER_VOXEL_GI))
	stage_output.voxel_gi_buffer = voxel_gi_buffer;
#endif
#if (defined(MOTION_VECTORS) && !defined(MODE_RTXDI_SURFACE))
	stage_output.motion_vector = motion_vector;
#endif
	return stage_output;
}
