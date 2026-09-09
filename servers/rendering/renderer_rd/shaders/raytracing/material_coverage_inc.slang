#ifndef MATERIAL_COVERAGE_INC_SLANG
#define MATERIAL_COVERAGE_INC_SLANG

float sample_material_coverage(MaterialData material, float2 uv, float2 uv_dx, float2 uv_dy) {
	uint texture_index = NonUniformResourceIndex(material.albedo_texture_idx);
	switch (material.coverage_sampler) {
		case 0u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_NEAREST_CLAMP, uv, uv_dx, uv_dy).a;
		case 1u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_LINEAR_CLAMP, uv, uv_dx, uv_dy).a;
		case 2u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP, uv, uv_dx, uv_dy).a;
		case 3u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP, uv, uv_dx, uv_dy).a;
		case 4u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP, uv, uv_dx, uv_dy).a;
		case 5u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP, uv, uv_dx, uv_dy).a;
		case 6u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_NEAREST_REPEAT, uv, uv_dx, uv_dy).a;
		case 7u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_LINEAR_REPEAT, uv, uv_dx, uv_dy).a;
		case 8u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT, uv, uv_dx, uv_dy).a;
		case 9u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT, uv, uv_dx, uv_dy).a;
		case 10u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT, uv, uv_dx, uv_dy).a;
		case 11u:
			return bindless_textures[texture_index].SampleGrad(SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT, uv, uv_dx, uv_dy).a;
	}
	return 0.0;
}

bool geometry_material_covered(GeometryHitInput hit, MaterialData material, float3 object_position, float3 position_dx, float3 position_dy, float2 uv, float2 uv_dx, float2 uv_dy, bool shadow_ray, uint caster_mask) {
	if (shadow_ray && ((hit.geometry.flags & FLAG_CASTS_SHADOWS) == 0u || (hit.geometry.instance_layer_mask & caster_mask) == 0u)) {
		return false;
	}
	if (!shadow_ray && (hit.geometry.flags & FLAG_SHADOWS_ONLY) != 0u) {
		return false;
	}
	if ((material.coverage_flags & 3u) == 0u) {
		return true;
	}
	uv = uv * material.uv1_scale + material.uv1_offset;
	float alpha = sample_material_coverage(material, uv, uv_dx * material.uv1_scale, uv_dy * material.uv1_scale) * material.albedo_color.a;
	if ((material.coverage_flags & 4u) != 0u) {
		float3 bary = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
		uint primitive;
		uint3 indices;
		if (!geometry_select_triangle(hit.geometry, hit.cluster_id, hit.primitive_id, primitive) || !geometry_triangle_indices(hit.geometry, primitive, indices)) {
			return false;
		}
		alpha *= geometry_fetch_color(hit.geometry, indices, bary).a;
	}
	if ((material.coverage_flags & 1u) != 0u && alpha < material.alpha_scissor_threshold) {
		return false;
	}
	if ((material.coverage_flags & 2u) != 0u && alpha < compute_alpha_hash_threshold(object_position, max(material.alpha_hash_scale, 1e-6), position_dx, position_dy)) {
		return false;
	}
	return true;
}

bool load_geometry_hit(uint geometry_index, uint primitive_id, int cluster_id, float2 barycentrics, float3x4 object_to_world, out GeometryHitInput hit, out MaterialData material) {
	hit = (GeometryHitInput)0;
	material = (MaterialData)0;
	uint geometry_count;
	uint material_count;
	uint stride;
	geometries.GetDimensions(geometry_count, stride);
	materials.GetDimensions(material_count, stride);
	if (geometry_index >= geometry_count || geometry_index >= material_count) {
		return false;
	}
	hit.geometry = geometries[geometry_index];
	hit.geometry_index = geometry_index;
	hit.primitive_id = primitive_id;
	hit.cluster_id = cluster_id;
	hit.barycentrics = barycentrics;
	hit.object_to_world = object_to_world;
	material = materials[geometry_index];
	return true;
}

#endif
