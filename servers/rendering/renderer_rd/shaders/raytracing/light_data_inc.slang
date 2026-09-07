#ifndef LIGHT_DATA_INC_SLANG
#define LIGHT_DATA_INC_SLANG

static const uint RT_LIGHT_TYPE_OMNI = 0;
static const uint RT_LIGHT_TYPE_DIRECTIONAL = 1;
static const uint RT_LIGHT_TYPE_SPOT = 2;
static const uint RT_LIGHT_TYPE_AREA = 3;
static const uint RT_LIGHT_TYPE_EMISSIVE_TRIANGLE = 4;
static const uint RT_LIGHT_TYPE_ENVIRONMENT = 5;

static const uint RT_LIGHT_FLAG_CASTS_SHADOW = 1u;
static const uint RT_LIGHT_FLAG_TEXTURED = 2u;


struct RTLightData {
	float3 position;
	uint type;
	float3 direction;
	uint flags;
	float3 emission;
	float radius;
	float3 axis_u;
	float inv_area;
	float3 axis_v;
	float attenuation;
	float range;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float specular_amount;
	uint texture_index;
	uint geometry_index;
	uint primitive_index;
	uint receiver_mask;
	uint caster_mask;
	uint topology_generation;
	uint2 _pad0;
	float4 uv_rect;
	float transform[12];
};

struct RTLightBufferParameters {
	uint local_first;
	uint local_count;
	uint infinite_first;
	uint infinite_count;
	uint environment_index;
	uint environment_present;
	uint total_count;
	uint previous_count;
};

struct RTLightProposal {
	float3 direction;
	float distance;
	float3 radiance;
	float solid_angle_pdf;
	float3 position;
	uint valid;
};

#endif
