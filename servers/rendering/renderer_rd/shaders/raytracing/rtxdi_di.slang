#[compute]

#VERSION_DEFINES

#define MAX_VIEWS 2
#define RTXDI_ENABLE_PRESAMPLING 0
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_RAY_TRACED
#define ALPHA_HASH_USED
#define ALPHA_HASH_COMPUTE
#include "../scene_data_inc.slang"
#include "../scene_forward_aa_inc.slang"
#include "surface_brdf_inc.slang"
#include "geometry_decode_inc.slang"
#include "rt_frame_constants_inc.slang"
#include "light_data_inc.slang"
#include "rtxdi_nrd_inc.slang"
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/Utils/RandomSamplerState.hlsli>

struct SceneDataBlock {
	SceneData data;
	SceneData prev_data;
};
[[vk::binding(0, 0)]] ConstantBuffer<SceneDataBlock> scene_data_block;

struct RtxdiParametersBlock {
	RTXDI_Parameters restir;
	RTXDI_RuntimeParameters runtime;
	RTXDI_LightBufferParameters light_buffer;
	uint4 extent_history;
};
[[vk::binding(1, 0)]] ConstantBuffer<RtxdiParametersBlock> rtxdi_params;

[[vk::binding(2, 0)]] Texture2D<float4> current_base;
[[vk::binding(3, 0)]] Texture2D<float4> current_shading;
[[vk::binding(4, 0)]] Texture2D<float4> current_emission;
[[vk::binding(5, 0)]] Texture2D<float4> current_motion;
[[vk::binding(6, 0)]] Texture2D<uint> current_geometry;
[[vk::binding(7, 0)]] Texture2D<uint2> current_classification;
[[vk::binding(8, 0)]] Texture2D<float> current_depth;
[[vk::binding(9, 0)]] Texture2D<float4> previous_base;
[[vk::binding(10, 0)]] Texture2D<float4> previous_shading;
[[vk::binding(11, 0)]] Texture2D<float4> previous_emission;
[[vk::binding(12, 0)]] Texture2D<float4> previous_motion;
[[vk::binding(13, 0)]] Texture2D<uint> previous_geometry;
[[vk::binding(14, 0)]] Texture2D<uint2> previous_classification;
[[vk::binding(15, 0)]] Texture2D<float> previous_depth;

[[vk::binding(16, 0)]] StructuredBuffer<RTLightData> current_lights;
[[vk::binding(17, 0)]] StructuredBuffer<RTLightData> previous_lights;
[[vk::binding(18, 0)]] ConstantBuffer<RTLightBufferParameters> rt_light_params;
[[vk::binding(19, 0)]] StructuredBuffer<uint> current_to_previous_light;
[[vk::binding(20, 0)]] StructuredBuffer<uint> previous_to_current_light;
[[vk::binding(21, 0)]] StructuredBuffer<GeometryData> geometries;
[[vk::binding(22, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(23, 0)]] RWStructuredBuffer<RTXDI_PackedDIReservoir> reservoir_data;
[[vk::binding(24, 0)]] StructuredBuffer<float2> neighbor_offsets;
[[vk::binding(25, 0)]] RaytracingAccelerationStructure scene_tlas;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
[[vk::binding(26, 0)]] Texture2DArray<float4> radiance_octmap;
#else
[[vk::binding(26, 0)]] Texture2D<float4> radiance_octmap;
#endif
[[vk::binding(27, 0)]] SamplerState surface_sampler;
[[vk::binding(28, 0)]] SamplerState environment_sampler;
[[vk::binding(29, 0)]] SamplerState bindless_linear_clamp;
[[vk::binding(30, 0)]] SamplerState bindless_linear_mip_repeat;
[[vk::binding(31, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> diffuse_radiance_distance;
[[vk::binding(32, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> specular_radiance_distance;
[[vk::binding(33, 0)]] SamplerState SAMPLER_NEAREST_CLAMP;
[[vk::binding(34, 0)]] SamplerState SAMPLER_LINEAR_CLAMP;
[[vk::binding(35, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
[[vk::binding(36, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
[[vk::binding(37, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
[[vk::binding(38, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
[[vk::binding(39, 0)]] SamplerState SAMPLER_NEAREST_REPEAT;
[[vk::binding(40, 0)]] SamplerState SAMPLER_LINEAR_REPEAT;
[[vk::binding(41, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
[[vk::binding(42, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
[[vk::binding(43, 0)]] SamplerState SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
[[vk::binding(44, 0)]] SamplerState SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
[[vk::binding(45, 0)]] ConstantBuffer<RTFrameConstants> rt_frame;

[[vk::binding(0, 1)]] Texture2D<float4> bindless_textures[];

#ifdef RTXDI_DIAGNOSTICS
[[vk::binding(46, 0)]] RWStructuredBuffer<uint> rtxdi_diagnostic_data;
static const uint RTXDI_DIAGNOSTIC_SAMPLE_LIMIT = 1024u;
static const uint RTXDI_DIAGNOSTIC_COUNTER_COUNT = 48u;
static const uint RTXDI_DIAGNOSTIC_SAMPLED_PIXELS = 0u;
static const uint RTXDI_DIAGNOSTIC_VALID_SURFACES = 1u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_OMNI = 2u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_DIRECTIONAL = 3u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_SPOT = 4u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_AREA = 5u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_EMISSIVE_TRIANGLE = 6u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_ENVIRONMENT = 7u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_CANDIDATE_SAMPLE_CALLS_OMNI = 8u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_CANDIDATE_SAMPLE_CALLS_DIRECTIONAL = 9u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_CANDIDATE_SAMPLE_CALLS_SPOT = 10u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_CANDIDATE_SAMPLE_CALLS_AREA = 11u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_CANDIDATE_SAMPLE_CALLS_EMISSIVE_TRIANGLE = 12u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_CANDIDATE_SAMPLE_CALLS_ENVIRONMENT = 13u;
static const uint RTXDI_DIAGNOSTIC_INITIAL_SELECTED_SAMPLE_RECONSTRUCTIONS = 14u;
static const uint RTXDI_DIAGNOSTIC_TARGET_PDF_CALLS = 15u;
static const uint RTXDI_DIAGNOSTIC_POSITIVE_TARGET_PDF_RESULTS = 16u;
static const uint RTXDI_DIAGNOSTIC_TARGET_PDF_BRDF_EVALUATIONS = 17u;
static const uint RTXDI_DIAGNOSTIC_BRDF_PDF_EVALUATIONS = 18u;
static const uint RTXDI_DIAGNOSTIC_BRDF_SAMPLE_CALLS = 19u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_SOURCE_PDF_CALLS = 20u;
static const uint RTXDI_DIAGNOSTIC_ENVIRONMENT_SOURCE_PDF_CALLS = 21u;
static const uint RTXDI_DIAGNOSTIC_CONSERVATIVE_VISIBILITY_CALLS = 22u;
static const uint RTXDI_DIAGNOSTIC_TEMPORAL_VISIBILITY_CALLS = 23u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_RAYS = 24u;
static const uint RTXDI_DIAGNOSTIC_NO_SHADOW_FLAG_BYPASSES = 25u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_TRIANGLE_CANDIDATES = 26u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_GEOMETRY_LOAD_SUCCESSES = 27u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_COVERAGE_CALLS = 28u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_MATERIAL_TESTS = 29u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_COMMITTED_HITS = 30u;
static const uint RTXDI_DIAGNOSTIC_SHADOW_VISIBLE_RESULTS = 31u;
static const uint RTXDI_DIAGNOSTIC_EMITTER_COVERAGE_CALLS = 32u;
static const uint RTXDI_DIAGNOSTIC_EMITTER_MATERIAL_TESTS = 33u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_CALLS = 34u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_TRIANGLE_CANDIDATES = 35u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_GEOMETRY_LOAD_SUCCESSES = 36u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_COVERAGE_CALLS = 37u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_MATERIAL_TESTS = 38u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_GEOMETRY_HITS = 39u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_RAY_GEOMETRY_MISSES = 40u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_LIGHT_SCAN_ITERATIONS = 41u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_LIGHT_SCAN_MATCHES = 42u;
static const uint RTXDI_DIAGNOSTIC_LOCAL_LIGHT_SCAN_NO_MATCH = 43u;
static const uint RTXDI_DIAGNOSTIC_VALID_OUTPUT_RESERVOIRS = 44u;
static const uint RTXDI_DIAGNOSTIC_FINAL_SHADING_BRDF_EVALUATIONS = 45u;
static const uint RTXDI_DIAGNOSTIC_GBUFFER_LOAD_CALLS = 46u;
static const uint RTXDI_DIAGNOSTIC_LIGHT_SAMPLE_CALLS_UNKNOWN_TYPE = 47u;

static bool rtxdi_diagnostic_sampled = false;
static bool rtxdi_diagnostic_initial_candidates = false;
static bool rtxdi_diagnostic_local_ray = false;
static uint rtxdi_diagnostic_base = 0u;

void rtxdi_diagnostic_count(uint counter) {
	if (rtxdi_diagnostic_sampled) {
		rtxdi_diagnostic_data[rtxdi_diagnostic_base + counter] += 1u;
	}
}

void rtxdi_diagnostic_begin(uint2 pixel) {
	uint pixel_count = rtxdi_params.extent_history.x * rtxdi_params.extent_history.y;
	uint stride = (pixel_count + RTXDI_DIAGNOSTIC_SAMPLE_LIMIT - 1u) / RTXDI_DIAGNOSTIC_SAMPLE_LIMIT;
	uint index = pixel.y * rtxdi_params.extent_history.x + pixel.x;
	rtxdi_diagnostic_sampled = index % stride == rtxdi_params.runtime.frameIndex % stride;
#if defined(MODE_INITIAL)
	uint pass = 0u;
#elif defined(MODE_TEMPORAL)
	uint pass = 1u;
#elif defined(MODE_SPATIAL)
	uint pass = 2u;
#else
	uint pass = 3u;
#endif
	rtxdi_diagnostic_base = (pass * RTXDI_DIAGNOSTIC_SAMPLE_LIMIT + index / stride) * RTXDI_DIAGNOSTIC_COUNTER_COUNT;
	rtxdi_diagnostic_count(RTXDI_DIAGNOSTIC_SAMPLED_PIXELS);
}
#define RTXDI_DIAGNOSTIC_COUNT(name) rtxdi_diagnostic_count(RTXDI_DIAGNOSTIC_##name)
#else
#define RTXDI_DIAGNOSTIC_COUNT(name)
#endif

#define RTXDI_LIGHT_RESERVOIR_BUFFER reservoir_data
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER neighbor_offsets
#include "light_sampling_inc.slang"
#include "surface_shading_inc.slang"
#include "rtxdi_application_bridge_inc.slang"
#include <Rtxdi/DI/InitialSampling.hlsli>
#include <Rtxdi/DI/TemporalResampling.hlsli>
#include <Rtxdi/DI/SpatialResampling.hlsli>

RTXDI_DIReservoir rtxdi_sample_initial(uint2 pixel, RAB_Surface surface, inout RTXDI_RandomSamplerState rng) {
	RTXDI_RandomSamplerState coherent_rng = RTXDI_InitRandomSampler(pixel / RTXDI_TILE_SIZE_IN_PIXELS, rtxdi_params.runtime.frameIndex, 1u);
	RAB_LightSample selected_sample;
	RTXDI_DIInitialSamplingParameters initial = rtxdi_params.restir.initialSamplingParams;
	initial.enableInitialVisibility = 0u;
	RTXDI_InitialSamplingMisData mis = RTXDI_ComputeInitialSamplingMisData(initial);
#ifdef RTXDI_DIAGNOSTICS
	rtxdi_diagnostic_initial_candidates = true;
#endif
	RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(rng, coherent_rng, surface, initial, rtxdi_params.light_buffer, selected_sample);
	if (rtxdi_params.restir.initialSamplingParams.numEnvironmentSamples > 0u && rtxdi_params.light_buffer.environmentLightParams.lightPresent != 0u) {
		RTXDI_DIReservoir environment_reservoir = RTXDI_EmptyDIReservoir();
		for (uint i = 0u; i < rtxdi_params.restir.initialSamplingParams.numEnvironmentSamples; i++) {
			float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
			uint light_index = rtxdi_params.light_buffer.environmentLightParams.lightIndex;
			RAB_LightSample light_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(light_index, false), surface, uv);
			float target_pdf = RAB_GetLightSampleTargetPdfForSurface(light_sample, surface);
			float source_pdf = RTXDI_LightBrdfMisWeight(surface, light_sample, 1.0, mis.environmentMapMisWeight, true, mis.brdfMisWeight, initial.brdfCutoff);
			if (RTXDI_StreamSample(environment_reservoir, light_index, uv, RTXDI_GetNextRandom(rng), target_pdf, 1.0 / source_pdf)) {
				selected_sample = light_sample;
			}
		}
#ifdef RTXDI_DIAGNOSTICS
		rtxdi_diagnostic_initial_candidates = false;
#endif
		RTXDI_FinalizeResampling(environment_reservoir, 1.0, float(mis.numMisSamples));
		environment_reservoir.M = 1.0;
		RTXDI_DIReservoir combined_reservoir = RTXDI_EmptyDIReservoir();
		RTXDI_CombineDIReservoirs(combined_reservoir, reservoir, 0.5, reservoir.targetPdf);
		if (RTXDI_CombineDIReservoirs(combined_reservoir, environment_reservoir, RTXDI_GetNextRandom(rng), environment_reservoir.targetPdf)) {
			selected_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(environment_reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(environment_reservoir));
		}
		RTXDI_FinalizeResampling(combined_reservoir, 1.0, 1.0);
		combined_reservoir.M = 1.0;
		reservoir = combined_reservoir;
	}
#ifdef RTXDI_DIAGNOSTICS
	rtxdi_diagnostic_initial_candidates = false;
#endif
	if (RTXDI_IsValidDIReservoir(reservoir)) {
		selected_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(reservoir));
	}
	if (rtxdi_params.restir.initialSamplingParams.enableInitialVisibility != 0u && RTXDI_IsValidDIReservoir(reservoir) && !RAB_GetConservativeVisibility(surface, selected_sample)) {
		RTXDI_StoreVisibilityInDIReservoir(reservoir, float3(0.0), true);
	}
	return reservoir;
}

[numthreads(8, 8, 1)]
void main(uint3 invocation : SV_DispatchThreadID) {
	uint2 pixel = invocation.xy;
	if (any(pixel >= rtxdi_params.extent_history.xy)) {
		return;
	}
#ifdef RTXDI_DIAGNOSTICS
	rtxdi_diagnostic_begin(pixel);
#endif
	RAB_Surface surface = RAB_GetGBufferSurface(int2(pixel), false);
	RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, rtxdi_params.runtime.frameIndex, 0u);
	if (!RAB_IsSurfaceValid(surface)) {
#if defined(MODE_INITIAL)
		RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.initialSamplingOutputBufferIndex);
#elif defined(MODE_TEMPORAL)
		RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.temporalResamplingOutputBufferIndex);
#elif defined(MODE_SPATIAL)
		RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.spatialResamplingOutputBufferIndex);
#else
		diffuse_radiance_distance[pixel] = float4(0.0);
		specular_radiance_distance[pixel] = float4(0.0);
#endif
		return;
	}

	RTXDI_DIAGNOSTIC_COUNT(VALID_SURFACES);

#if defined(MODE_INITIAL)
	RTXDI_DIReservoir reservoir = rtxdi_sample_initial(pixel, surface, rng);
	RTXDI_StoreDIReservoir(reservoir, rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.initialSamplingOutputBufferIndex);
#elif defined(MODE_TEMPORAL)
	RTXDI_DIReservoir current_reservoir = RTXDI_LoadDIReservoir(rtxdi_params.restir.reservoirBufferParams, pixel, rtxdi_params.restir.bufferIndices.initialSamplingOutputBufferIndex);
	RAB_LightSample selected_sample = RAB_EmptyLightSample();
	int2 temporal_pixel;
	RTXDI_DIReservoir reservoir = current_reservoir;
	if (rtxdi_params.extent_history.z != 0u) {
		float3 temporal_motion = surface.motion;
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
	float4 diffuse_output = float4(0.0);
	float4 specular_output = float4(0.0);
	if (RTXDI_IsValidDIReservoir(reservoir)) {
		RAB_LightSample light_sample = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false), surface, RTXDI_GetDIReservoirSampleUV(reservoir));
		if (RAB_GetLightSampleTargetPdfForSurface(light_sample, surface) > 0.0 && RAB_GetConservativeVisibility(surface, light_sample)) {
			float3 diffuse;
			float3 specular;
			RTXDI_DIAGNOSTIC_COUNT(FINAL_SHADING_BRDF_EVALUATIONS);
			surface_evaluate_brdf(surface.surface, light_sample.direction, diffuse, specular);
			BrdfData brdf_data = prepareBRDFData(surface.surface.normal, light_sample.direction, surface.surface.view_direction, surface_material_properties(surface.surface.material));
			float3 diffuse_factor;
			float3 specular_factor;
			rtxdi_nrd_material_factors(surface.surface.normal, surface.surface.view_direction, brdf_data.diffuseReflectance, brdf_data.specularF0, surface.surface.material.roughness, diffuse_factor, specular_factor);
			float3 incident = light_sample.radiance * RTXDI_GetDIReservoirInvPdf(reservoir) / surface_light_measure(light_sample);
			diffuse_output = rtxdi_nrd_pack_radiance_distance(incident * diffuse / diffuse_factor, light_sample.distance);
			specular_output = rtxdi_nrd_pack_radiance_distance(incident * specular * light_sample.specular_amount / specular_factor, light_sample.distance);
		}
	}
	diffuse_radiance_distance[pixel] = diffuse_output;
	specular_radiance_distance[pixel] = specular_output;
#endif
#ifdef RTXDI_DIAGNOSTICS
	if (RTXDI_IsValidDIReservoir(reservoir)) {
		RTXDI_DIAGNOSTIC_COUNT(VALID_OUTPUT_RESERVOIRS);
	}
#endif
}
