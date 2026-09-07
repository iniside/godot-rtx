struct DecalData {
	row_major float4x4 xform;
	float3 inv_extents;
	float albedo_mix;
	float4 albedo_rect;
	float4 normal_rect;
	float4 orm_rect;
	float4 emission_rect;
	float4 modulate;
	float emission_energy;
	uint mask;
	float upper_fade;
	float lower_fade;
	row_major float3x4 normal_xform;
	float3 normal;
	float normal_fade;
};
