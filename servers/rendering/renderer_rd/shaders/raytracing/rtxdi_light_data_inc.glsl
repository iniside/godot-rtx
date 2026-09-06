#ifndef RTXDI_LIGHT_DATA_INC_GLSL
#define RTXDI_LIGHT_DATA_INC_GLSL

#define RT_LIGHT_TYPE_OMNI 0
#define RT_LIGHT_TYPE_DIRECTIONAL 1
#define RT_LIGHT_TYPE_SPOT 2
#define RT_LIGHT_TYPE_AREA 3
#define RT_LIGHT_TYPE_EMISSIVE_TRIANGLE 4
#define RT_LIGHT_TYPE_ENVIRONMENT 5

#define RT_LIGHT_FLAG_CASTS_SHADOW 1u
#define RT_LIGHT_FLAG_TEXTURED 2u

#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

struct RTLightData {
	vec3 position;
	uint type;
	vec3 direction;
	uint flags;
	vec3 emission;
	float radius;
	vec3 axis_u;
	float inv_area;
	vec3 axis_v;
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
	uvec2 _pad0;
	vec4 uv_rect;
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
	vec3 direction;
	float distance;
	vec3 radiance;
	float solid_angle_pdf;
	vec3 position;
	uint valid;
};

#endif
