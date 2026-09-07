#ifdef ALPHA_HASH_USED

float hash_2d(float2 p) {
	return frac(1.0e4 * sin(17.0 * p.x + 0.1 * p.y) *
			(0.1 + abs(sin(13.0 * p.y + p.x))));
}

float hash_3d(float3 p) {
	return hash_2d(float2(hash_2d(p.xy), p.z));
}

float compute_alpha_hash_threshold(float3 pos, float hash_scale, float3 dx, float3 dy) {
	float delta_max_sqr = max(length(dx), length(dy));
	float pix_scale = 1.0 / (hash_scale * delta_max_sqr);

	float2 pix_scales =
			float2(exp2(floor(log2(pix_scale))), exp2(ceil(log2(pix_scale))));

	float2 a_thresh = float2(hash_3d(floor(pix_scales.x * pos.xyz)),
			hash_3d(floor(pix_scales.y * pos.xyz)));

	float lerp_factor = frac(log2(pix_scale));

	float a_interp = (1.0 - lerp_factor) * a_thresh.x + lerp_factor * a_thresh.y;

	float min_lerp = min(lerp_factor, 1.0 - lerp_factor);

	float3 cases = float3(a_interp * a_interp / (2.0 * min_lerp * (1.0 - min_lerp)),
			(a_interp - 0.5 * min_lerp) / (1.0 - min_lerp),
			1.0 - ((1.0 - a_interp) * (1.0 - a_interp) / (2.0 * min_lerp * (1.0 - min_lerp))));

	float alpha_hash_threshold =
			(a_interp < (1.0 - min_lerp)) ? ((a_interp < min_lerp) ? cases.x : cases.y) : cases.z;

	return float(clamp(alpha_hash_threshold, 0.00001, 1.0));
}

#ifndef ALPHA_HASH_COMPUTE
float compute_alpha_hash_threshold(float3 pos, float hash_scale) {
	return compute_alpha_hash_threshold(pos, hash_scale, ddx(pos), ddy(pos));
}
#endif

#endif

#ifdef ALPHA_ANTIALIASING_EDGE_USED

float calc_mip_level(float2 texture_coord) {
	float2 dx = ddx(texture_coord);
	float2 dy = ddy(texture_coord);
	float delta_max_sqr = max(dot(dx, dx), dot(dy, dy));
	return max(float(0.0), float(0.5) * float(log2(delta_max_sqr)));
}

float compute_alpha_antialiasing_edge(float input_alpha, float2 texture_coord, float alpha_edge) {
	input_alpha *= float(1.0) + calc_mip_level(texture_coord) * float(0.25);
	input_alpha = (input_alpha - alpha_edge) / max(fwidth(input_alpha), float(0.0001)) + float(0.5);
	return clamp(input_alpha, float(0.0), float(1.0));
}

#endif
