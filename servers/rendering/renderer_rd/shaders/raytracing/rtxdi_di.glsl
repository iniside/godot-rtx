#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_control_flow_attributes : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_NV_cluster_acceleration_structure : enable
#extension GL_ARB_gpu_shader_int64 : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define GLSL 1
#define RTXDI_GLSL 1
#define RTXDI_ENABLE_PRESAMPLING 0
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_RAY_TRACED

// clang-format off
#define MAX_VIEWS 2
#define PI 3.141592653589f
#define OFFSET_NONE 0xFFFFFFFFu
#define FLAG_COMPRESSED 1u
#define FLAG_PROCEDURAL 2u
#define FLAG_DEFORMED 4u
#define FLAG_CLUSTERED 8u
#define FLAG_CASTS_SHADOWS 16u
#define FLAG_SHADOWS_ONLY 32u
#define FLAG_SHADOW_CULL_ENABLED 64u
#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/Utils/RandomSamplerState.hlsli>
#undef saturate
#include "brdf_inc.glsl"
#include "rtxdi_nrd_inc.glsl"
#define ALPHA_HASH_USED
#define ALPHA_HASH_COMPUTE
#define half float
#include "../scene_forward_aa_inc.glsl"
#undef half
#include "raytracing_data_inc.glsl"
#include "rtxdi_light_data_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

layout(set = 0, binding = 1, std140) uniform RtxdiParametersBlock {
	RTXDI_Parameters restir;
	RTXDI_RuntimeParameters runtime;
	RTXDI_LightBufferParameters light_buffer;
	uvec4 extent_history;
}
rtxdi_params;

layout(set = 0, binding = 2) uniform texture2D current_base;
layout(set = 0, binding = 3) uniform texture2D current_shading;
layout(set = 0, binding = 4) uniform texture2D current_emission;
layout(set = 0, binding = 5) uniform texture2D current_motion;
layout(set = 0, binding = 6) uniform utexture2D current_geometry;
layout(set = 0, binding = 7) uniform utexture2D current_classification;
layout(set = 0, binding = 8) uniform texture2D current_depth;
layout(set = 0, binding = 9) uniform texture2D previous_base;
layout(set = 0, binding = 10) uniform texture2D previous_shading;
layout(set = 0, binding = 11) uniform texture2D previous_emission;
layout(set = 0, binding = 12) uniform texture2D previous_motion;
layout(set = 0, binding = 13) uniform utexture2D previous_geometry;
layout(set = 0, binding = 14) uniform utexture2D previous_classification;
layout(set = 0, binding = 15) uniform texture2D previous_depth;

layout(set = 0, binding = 16, std430) readonly buffer CurrentLightBuffer {
	RTLightData current_lights[];
};
layout(set = 0, binding = 17, std430) readonly buffer PreviousLightBuffer {
	RTLightData previous_lights[];
};
layout(set = 0, binding = 18, std140) uniform LightParametersBuffer {
	RTLightBufferParameters rt_light_params;
};
layout(set = 0, binding = 19, std430) readonly buffer CurrentToPreviousLightBuffer {
	uint current_to_previous_light[];
};
layout(set = 0, binding = 20, std430) readonly buffer PreviousToCurrentLightBuffer {
	uint previous_to_current_light[];
};
layout(set = 0, binding = 21, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};
layout(set = 0, binding = 22, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
layout(set = 0, binding = 23, std430) buffer ReservoirBuffer {
	RTXDI_PackedDIReservoir reservoir_data[];
};
layout(set = 0, binding = 24, std430) readonly buffer NeighborOffsetsBuffer {
	vec2 neighbor_offsets[];
};
layout(set = 0, binding = 25) uniform accelerationStructureEXT scene_tlas;

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 26) uniform texture2DArray radiance_octmap;
#else
layout(set = 0, binding = 26) uniform texture2D radiance_octmap;
#endif

layout(set = 0, binding = 27) uniform sampler surface_sampler;
layout(set = 0, binding = 28) uniform sampler environment_sampler;
layout(set = 0, binding = 29) uniform sampler bindless_linear_clamp;
layout(set = 0, binding = 30) uniform sampler bindless_linear_mip_repeat;
layout(set = 0, binding = 31, rgba16f) uniform writeonly image2D diffuse_radiance_distance;
layout(set = 0, binding = 32, rgba16f) uniform writeonly image2D specular_radiance_distance;

#define SAMPLERS_BINDING_FIRST_INDEX 33
#include "../samplers_inc.glsl"

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#define SAMPLER_LINEAR_CLAMP bindless_linear_clamp
#define SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT bindless_linear_mip_repeat
#define RTXDI_LIGHT_RESERVOIR_BUFFER reservoir_data
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER neighbor_offsets

uint rtxdi_visibility_caster_mask;

// clang-format off
#include "rtxdi_light_sampling_inc.glsl"
#undef SAMPLER_LINEAR_CLAMP
#undef SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT
#include "rtxdi_application_bridge_inc.glsl"
#include <Rtxdi/DI/InitialSampling.hlsli>
#include <Rtxdi/DI/TemporalResampling.hlsli>
#include <Rtxdi/DI/SpatialResampling.hlsli>
// clang-format on

RTXDI_DIReservoir rtxdi_sample_initial(uvec2 pixel, RAB_Surface surface, inout RTXDI_RandomSamplerState rng) {
	RTXDI_RandomSamplerState coherent_rng = RTXDI_InitRandomSampler(pixel / RTXDI_TILE_SIZE_IN_PIXELS, rtxdi_params.runtime.frameIndex, 1u);
	RAB_LightSample selected_sample;
	RTXDI_DIInitialSamplingParameters initial = rtxdi_params.restir.initialSamplingParams;
	initial.enableInitialVisibility = 0u;
	RTXDI_InitialSamplingMisData mis = RTXDI_ComputeInitialSamplingMisData(initial);
	RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(rng, coherent_rng, surface, initial, rtxdi_params.light_buffer, selected_sample);
	if (rtxdi_params.restir.initialSamplingParams.numEnvironmentSamples > 0u && rtxdi_params.light_buffer.environmentLightParams.lightPresent != 0u) {
		RTXDI_DIReservoir environment_reservoir = RTXDI_EmptyDIReservoir();
		for (uint i = 0u; i < rtxdi_params.restir.initialSamplingParams.numEnvironmentSamples; i++) {
			vec2 uv = vec2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
			uint light_index = rtxdi_params.light_buffer.environmentLightParams.lightIndex;
			RAB_LightSample light_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(light_index, false), surface, uv);
			float target_pdf = RAB_GetLightSampleTargetPdfForSurface(light_sample, surface);
			float source_pdf = RTXDI_LightBrdfMisWeight(surface, light_sample, 1.0, mis.environmentMapMisWeight, true, mis.brdfMisWeight, initial.brdfCutoff);
			if (RTXDI_StreamSample(environment_reservoir, light_index, uv, RTXDI_GetNextRandom(rng), target_pdf, 1.0 / source_pdf)) {
				selected_sample = light_sample;
			}
		}
		RTXDI_FinalizeResampling(environment_reservoir, 1.0, float(mis.numMisSamples));
		environment_reservoir.M = 1.0;
		if (RTXDI_CombineDIReservoirs(reservoir, environment_reservoir, RTXDI_GetNextRandom(rng), environment_reservoir.targetPdf)) {
			selected_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(environment_reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(environment_reservoir));
		}
		RTXDI_FinalizeResampling(reservoir, 1.0, 1.0);
		reservoir.M = 1.0;
	}
	if (RTXDI_IsValidDIReservoir(reservoir)) {
		selected_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(reservoir));
	}
	if (rtxdi_params.restir.initialSamplingParams.enableInitialVisibility != 0u && RTXDI_IsValidDIReservoir(reservoir) && !RAB_GetConservativeVisibility(surface, selected_sample)) {
		RTXDI_StoreVisibilityInDIReservoir(reservoir, vec3(0.0), true);
	}
	return reservoir;
}

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(pixel, rtxdi_params.extent_history.xy))) {
		return;
	}
	RAB_Surface surface = RAB_GetGBufferSurface(ivec2(pixel), false);
	RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, rtxdi_params.runtime.frameIndex, 0u);
	if (!RAB_IsSurfaceValid(surface)) {
#if defined(MODE_INITIAL)
		RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.initialSamplingOutputBufferIndex);
#elif defined(MODE_TEMPORAL)
		RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.temporalResamplingOutputBufferIndex);
#elif defined(MODE_SPATIAL)
		RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.spatialResamplingOutputBufferIndex);
#else
		imageStore(diffuse_radiance_distance, ivec2(pixel), vec4(0.0));
		imageStore(specular_radiance_distance, ivec2(pixel), vec4(0.0));
#endif
		return;
	}

#if defined(MODE_INITIAL)
	RTXDI_DIReservoir reservoir = rtxdi_sample_initial(pixel, surface, rng);
	RTXDI_StoreDIReservoir(reservoir, rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.initialSamplingOutputBufferIndex);
#elif defined(MODE_TEMPORAL)
	RTXDI_DIReservoir current_reservoir = RTXDI_LoadDIReservoir(rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.initialSamplingOutputBufferIndex);
	RAB_LightSample selected_sample = RAB_EmptyLightSample();
	int2 temporal_pixel;
	RTXDI_DIReservoir reservoir = current_reservoir;
	if (rtxdi_params.extent_history.z != 0u) {
		vec3 temporal_motion = surface.motion;
		temporal_motion.xy += (scene_data_block.prev_data.taa_jitter - scene_data_block.data.taa_jitter) * 0.5 * scene_data_block.data.viewport_size;
		reservoir = RTXDI_DITemporalResampling(pixel, surface, current_reservoir, rng, rtxdi_params.runtime, rtxdi_params.restir.reservoirBufferParams, temporal_motion, rtxdi_params.restir.bufferIndices.temporalResamplingInputBufferIndex, rtxdi_params.restir.temporalResamplingParams, temporal_pixel, selected_sample);
	}
	RTXDI_StoreDIReservoir(reservoir, rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.temporalResamplingOutputBufferIndex);
#elif defined(MODE_SPATIAL)
	RTXDI_DIReservoir center_reservoir = RTXDI_LoadDIReservoir(rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.spatialResamplingInputBufferIndex);
	RAB_LightSample selected_sample = RAB_EmptyLightSample();
	RTXDI_DIReservoir reservoir = RTXDI_DISpatialResampling(pixel, surface, center_reservoir, rng, rtxdi_params.runtime, rtxdi_params.restir.reservoirBufferParams, rtxdi_params.restir.bufferIndices.spatialResamplingInputBufferIndex, rtxdi_params.restir.spatialResamplingParams, selected_sample);
	RTXDI_StoreDIReservoir(reservoir, rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.spatialResamplingOutputBufferIndex);
#else
	RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.shadingInputBufferIndex);
	vec4 diffuse_output = vec4(0.0);
	vec4 specular_output = vec4(0.0);
	if (RTXDI_IsValidDIReservoir(reservoir)) {
		RAB_LightSample light_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(reservoir));
		if (RAB_GetLightSampleTargetPdfForSurface(light_sample, surface) > 0.0 && RAB_GetConservativeVisibility(surface, light_sample)) {
			vec3 diffuse;
			vec3 specular;
			rtxdi_evaluate_brdf(surface, light_sample.direction, diffuse, specular);
			BrdfData brdf_data = prepareBRDFData(surface.normal, light_sample.direction, surface.view_dir, rtxdi_material_properties(surface));
			vec3 diffuse_factor;
			vec3 specular_factor;
			rtxdi_nrd_material_factors(surface.normal, surface.view_dir, brdf_data.diffuseReflectance, brdf_data.specularF0, surface.material.roughness, diffuse_factor, specular_factor);
			vec3 incident = light_sample.radiance * RTXDI_GetDIReservoirInvPdf(reservoir) / rtxdi_light_sample_measure(light_sample);
			diffuse_output = rtxdi_nrd_pack_radiance_distance(incident * diffuse / diffuse_factor, light_sample.distance);
			specular_output = rtxdi_nrd_pack_radiance_distance(incident * specular * light_sample.specular_amount / specular_factor, light_sample.distance);
		}
	}
	imageStore(diffuse_radiance_distance, ivec2(pixel), diffuse_output);
	imageStore(specular_radiance_distance, ivec2(pixel), specular_output);
#endif
}
