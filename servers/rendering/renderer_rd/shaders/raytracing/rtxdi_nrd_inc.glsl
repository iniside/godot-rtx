// SPDX-FileCopyrightText: Copyright (c) 2022 NVIDIA CORPORATION. All rights reserved.
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary

#ifndef RTXDI_NRD_INC_GLSL
#define RTXDI_NRD_INC_GLSL

vec3 rtxdi_nrd_environment_term(vec3 f0, float nov, float roughness) {
	float m = clamp(roughness * roughness, 0.0, 1.0);
	vec4 x = vec4(1.0, nov, nov * nov, nov * nov * nov);
	vec4 y = vec4(1.0, m, m * m, m * m * m);
	mat2 m1 = transpose(mat2(0.99044, -1.28514, 1.29678, -0.755907));
	mat3 m2 = transpose(mat3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627));
	mat2 m3 = transpose(mat2(0.0365463, 3.32707, 9.0632, -9.04756));
	mat3 m4 = transpose(mat3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123));
	float bias = dot(m1 * x.xy, y.xy) / max(dot(m2 * x.xyw, y.xyw), 1e-6);
	float scale = dot(m3 * x.xy, y.xy) / max(dot(m4 * x.xzw, y.xyw), 1e-6);
	return clamp(f0 * scale + bias, 0.0, 1.0);
}

void rtxdi_nrd_material_factors(vec3 normal, vec3 view, vec3 albedo, vec3 f0, float roughness, out vec3 diffuse, out vec3 specular) {
	vec3 environment = rtxdi_nrd_environment_term(f0, abs(dot(normal, view)), roughness);
	diffuse = mix(vec3(0.02), vec3(1.0), (1.0 - environment) * albedo);
	specular = mix(vec3(0.02), vec3(1.0), environment * mix(0.1, 1.0, roughness));
}

vec4 rtxdi_nrd_pack_radiance_distance(vec3 radiance, float distance) {
	radiance = any(isnan(radiance)) || any(isinf(radiance)) ? vec3(0.0) : clamp(radiance, 0.0, 65504.0);
	distance = isnan(distance) || isinf(distance) ? 0.0 : clamp(distance, 0.0, 65504.0);
	return vec4(radiance, distance);
}

#endif
