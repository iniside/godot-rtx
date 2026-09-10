#define SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT (1 << 0)
#define SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP (1 << 1)
#define SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP (1 << 2)
#define SCENE_DATA_FLAGS_USE_ROUGHNESS_LIMITER (1 << 3)
#define SCENE_DATA_FLAGS_USE_FOG (1 << 4)
#define SCENE_DATA_FLAGS_USE_UV2_MATERIAL (1 << 5)
#define SCENE_DATA_FLAGS_USE_PANCAKE_SHADOWS (1 << 6)
#define SCENE_DATA_FLAGS_IN_SHADOW_PASS (1 << 7)
#define SCENE_DATA_FLAGS_USE_DEPTH_FOG (1 << 8)

struct SceneData {
	row_major float4x4 projection_matrix;
	row_major float4x4 inv_projection_matrix;
	row_major float3x4 inv_view_matrix;
	row_major float3x4 view_matrix;

	float4 inv_view_precision;

	row_major float4x4 projection_matrix_view[MAX_VIEWS];
	row_major float4x4 inv_projection_matrix_view[MAX_VIEWS];
	float4 eye_offset[MAX_VIEWS];

	row_major float4x4 main_cam_inv_view_matrix;

	float2 viewport_size;
	float2 screen_pixel_size;

	float4 directional_penumbra_shadow_kernel[32];
	float4 directional_soft_shadow_kernel[32];
	float4 penumbra_shadow_kernel[32];
	float4 soft_shadow_kernel[32];

	float2 shadow_atlas_pixel_size;
	float2 directional_shadow_pixel_size;

	float radiance_pixel_size;
	float radiance_border_size;
	float2 reflection_atlas_border_size;

	uint directional_light_count;
	float dual_paraboloid_side;
	float z_far;
	float z_near;

	float roughness_limiter_amount;
	float roughness_limiter_limit;
	float opaque_prepass_threshold;
	uint flags;

	row_major float3x3 radiance_inverse_xform;

	float4 ambient_light_color_energy;

	float ambient_color_sky_mix;
	float fog_density;
	float fog_height;
	float fog_height_density;

	float fog_depth_curve;
	float fog_depth_begin;
	float fog_depth_end;
	float fog_sun_scatter;

	float3 fog_light_color;
	float fog_aerial_perspective;

	float time;
	float taa_frame_count;
	float2 taa_jitter;

	float emissive_exposure_normalization;
	float IBL_exposure_normalization;
	uint camera_visible_layers;
	float pass_alpha_multiplier;
	float fog_sky_affect;
};
