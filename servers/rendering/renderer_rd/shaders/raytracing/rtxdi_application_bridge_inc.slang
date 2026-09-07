#ifndef RTXDI_APPLICATION_BRIDGE_INC_SLANG
#define RTXDI_APPLICATION_BRIDGE_INC_SLANG

typedef SurfaceMaterial RAB_Material;
typedef RTLightData RAB_LightInfo;
typedef SurfaceLightSample RAB_LightSample;

struct RAB_Surface {
	SurfaceData surface;
	float linear_depth;
	float3 motion;
	bool valid;
};

RAB_Surface RAB_EmptySurface() {
	return (RAB_Surface)0;
}

RAB_LightInfo RAB_EmptyLightInfo() {
	return (RAB_LightInfo)0;
}

RAB_LightSample RAB_EmptyLightSample() {
	RAB_LightSample sample = (RAB_LightSample)0;
	sample.specular_amount = 1.0;
	return sample;
}

float4x4 rtxdi_decode_inv_view(SceneData scene_data) {
	float4x4 inv_view = float4x4(scene_data.inv_view_matrix[0], scene_data.inv_view_matrix[1], scene_data.inv_view_matrix[2], float4(0.0, 0.0, 0.0, 1.0));
#ifdef USE_DOUBLE_PRECISION
	inv_view[0][3] = -(inv_view[0][3] + scene_data.inv_view_precision.x);
	inv_view[1][3] = -(inv_view[1][3] + scene_data.inv_view_precision.y);
	inv_view[2][3] = -(inv_view[2][3] + scene_data.inv_view_precision.z);
#endif
	return inv_view;
}

GeometryFootprint rtxdi_camera_footprint() {
	GeometryFootprint footprint;
	footprint.world_to_clip = mul(transpose(scene_data_block.data.projection_matrix), geometry_inverse(rtxdi_decode_inv_view(scene_data_block.data)));
	footprint.pixel_size = scene_data_block.data.screen_pixel_size;
	return footprint;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixel, bool previous_frame) {
	if (any(pixel < int2(0)) || any(pixel >= int2(rtxdi_params.extent_history.xy)) || (previous_frame && rtxdi_params.extent_history.z == 0u)) {
		return RAB_EmptySurface();
	}
	int3 address = int3(pixel, 0);
	SurfaceBufferData packed;
	packed.base = previous_frame ? previous_base.Load(address) : current_base.Load(address);
	packed.shading = previous_frame ? previous_shading.Load(address) : current_shading.Load(address);
	packed.emission = previous_frame ? previous_emission.Load(address) : current_emission.Load(address);
	float4 motion = previous_frame ? previous_motion.Load(address) : current_motion.Load(address);
	packed.geometry = previous_frame ? previous_geometry.Load(address) : current_geometry.Load(address);
	packed.classification = previous_frame ? previous_classification.Load(address) : current_classification.Load(address);
	packed.ambient_occlusion = motion.w;
	float depth = previous_frame ? previous_depth.Load(address) : current_depth.Load(address);
	if ((packed.classification.x & 1u) == 0u || (packed.classification.x & 2u) != 0u || depth <= 0.0) {
		return RAB_EmptySurface();
	}
	SceneData scene_data = previous_frame ? scene_data_block.prev_data : scene_data_block.data;
	float2 ndc = (float2(pixel) + 0.5) * scene_data.screen_pixel_size * 2.0 - 1.0;
	float4 view = mul(float4(ndc, depth, 1.0), scene_data.inv_projection_matrix);
	view /= view.w;
	float4x4 inv_view = rtxdi_decode_inv_view(scene_data);
	float3 world_position = mul(inv_view, float4(view.xyz, 1.0)).xyz;
	float3 camera_direction = float3(inv_view[0][2], inv_view[1][2], inv_view[2][2]);
	float3 camera_position = float3(inv_view[0][3], inv_view[1][3], inv_view[2][3]);
	float3 view_direction = normalize(rtxdi_params.extent_history.w != 0u ? camera_direction : camera_position - world_position);
	RAB_Surface surface;
	surface.surface = decode_surface_buffers(packed, world_position, view_direction);
	surface.linear_depth = -view.z;
	surface.motion = motion.xyz;
	surface.valid = true;
	return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface) {
	return surface.valid;
}

float RAB_GetSurfaceLinearDepth(RAB_Surface surface) {
	return surface.linear_depth;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) {
	return surface.surface.world_position;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface) {
	return surface.surface.normal;
}

RAB_Material RAB_GetMaterial(RAB_Surface surface) {
	return surface.surface.material;
}

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b) {
	return a.flags == b.flags && distance(a.base_color, b.base_color) <= 0.1 && abs(a.roughness - b.roughness) <= 0.1 && abs(a.metallic - b.metallic) <= 0.1;
}

int2 RAB_ClampSamplePositionIntoView(int2 pixel_position, bool previous_frame) {
	return clamp(pixel_position, int2(0), int2(rtxdi_params.extent_history.xy) - int2(1));
}

RAB_LightInfo RAB_LoadLightInfo(uint light_index, bool previous_frame) {
	uint count = previous_frame ? rt_light_params.previous_count : rt_light_params.total_count;
	if (light_index >= count) {
		return RAB_EmptyLightInfo();
	}
	RAB_LightInfo light = previous_frame ? previous_lights[light_index] : current_lights[light_index];
	if (previous_frame && light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE) {
		uint current_index = previous_to_current_light[light_index];
		if (current_index >= rt_light_params.total_count) {
			return RAB_EmptyLightInfo();
		}
		light.geometry_index = current_lights[current_index].geometry_index;
		light._pad0.x = 1u;
	}
	return light;
}

int RAB_TranslateLightIndex(uint light_index, bool current_to_previous) {
	uint source_count = current_to_previous ? rt_light_params.total_count : rt_light_params.previous_count;
	uint target_count = current_to_previous ? rt_light_params.previous_count : rt_light_params.total_count;
	if (light_index >= source_count) {
		return -1;
	}
	uint mapped = current_to_previous ? current_to_previous_light[light_index] : previous_to_current_light[light_index];
	return mapped >= target_count ? -1 : int(mapped);
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 random) {
	SceneData scene = scene_data_block.data;
	float3x3 world_to_radiance = mul(transpose(scene.radiance_inverse_xform), transpose((float3x3)scene.inv_view_matrix));
	float2 border = float2(scene.radiance_border_size, 1.0 - scene.radiance_border_size * 2.0);
	return sample_surface_light(light, surface.surface, random, rtxdi_camera_footprint(), world_to_radiance, border);
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample light_sample, out float3 direction, out float distance) {
	direction = light_sample.direction;
	distance = light_sample.distance;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample light_sample) {
	return light_sample.solid_angle_pdf;
}

bool RAB_IsAnalyticLightSample(RAB_LightSample light_sample) {
	return light_sample.light_type != RT_LIGHT_TYPE_EMISSIVE_TRIANGLE && light_sample.light_type != RT_LIGHT_TYPE_ENVIRONMENT;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample light_sample, RAB_Surface surface) {
	return surface_light_target_pdf(light_sample, surface.surface);
}

float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 direction) {
	return surface_brdf_pdf(surface.surface, direction);
}

bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction) {
	float3 random;
	random.x = RTXDI_GetNextRandom(rng);
	random.y = RTXDI_GetNextRandom(rng);
	random.z = RTXDI_GetNextRandom(rng);
	return surface_sample_brdf(surface.surface, random, direction);
}

float RAB_EvaluateLocalLightSourcePdf(uint light_index) {
	return light_uniform_light_pdf(light_index, rt_light_params.local_first, rt_light_params.local_count);
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction) {
	float phi = atan2(direction.y, direction.x);
	return float2(clamp((1.0 - direction.z) * 0.5, 0.0, 1.0), frac(phi / (2.0 * PI)));
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction) {
	return 1.0;
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample light_sample) {
	return trace_light_visibility(surface.surface, light_sample, rtxdi_camera_footprint());
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface current_surface, RAB_Surface previous_surface, RAB_LightSample light_sample) {
	return trace_light_visibility(previous_surface.surface, light_sample, rtxdi_camera_footprint());
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float min_t, float max_t, out uint light_index, out float2 random) {
	light_index = RTXDI_InvalidLightIndex;
	random = float2(0.0);
	uint geometry_index;
	uint primitive_index;
	float2 barycentrics;
	if (!trace_local_geometry(origin, direction, min_t, max_t, rtxdi_camera_footprint(), geometry_index, primitive_index, barycentrics)) {
		return false;
	}
	for (uint i = 0u; i < rt_light_params.local_count; i++) {
		uint candidate_index = rt_light_params.local_first + i;
		RTLightData light = current_lights[candidate_index];
		if (light.type == RT_LIGHT_TYPE_EMISSIVE_TRIANGLE && light.geometry_index == geometry_index && light.primitive_index == primitive_index) {
			light_index = candidate_index;
			float root = max(barycentrics.x + barycentrics.y, 1e-6);
			random = float2(root * root, barycentrics.y / root);
			break;
		}
	}
	return true;
}

#endif
