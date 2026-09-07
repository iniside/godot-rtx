#ifndef OCT_INC_SLANG
#define OCT_INC_SLANG

float3 oct_to_vec3(float2 e) {
	float3 v = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

float3 oct_to_vec3_with_border(float2 uv, float border_size) {

	uv = (uv - 0.5) * (2.0 / border_size);

	float2 mask = step(float2(1.0), abs(uv));
	uv = 2.0 * clamp(uv, -1.0, 1.0) - uv;
	uv = lerp(uv, -uv, mask.yx);
	return oct_to_vec3(uv);
}

float2 oct_wrap(float2 v) {
	float2 signVal;
	signVal.x = v.x >= 0.0 ? 1.0 : -1.0;
	signVal.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * signVal;
}

float2 vec3_to_oct(float3 n) {

	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	n.xy = (n.z >= 0.0) ? n.xy : oct_wrap(n.xy);
	n.xy = n.xy * 0.5 + 0.5;
	return n.xy;
}

float2 vec3_to_oct_with_border(float3 n, float2 border_size) {
	float2 uv = vec3_to_oct(n);
	return uv * border_size.y + border_size.x;
}

float vec3_to_oct_lod(float3 n_ddx, float3 n_ddy, float pixel_size) {

	float pixel_size_sqr = 4.0 * pixel_size * pixel_size;
	float ddx = dot(n_ddx, n_ddx) / pixel_size_sqr;
	float ddy = dot(n_ddy, n_ddy) / pixel_size_sqr;
	float dd_sqr = max(ddx, ddy);
	return 0.25 * log2(dd_sqr + 1e-6f);
}

#endif
