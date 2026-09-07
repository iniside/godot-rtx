#ifndef GEOMETRY_DECODE_INC_SLANG
#define GEOMETRY_DECODE_INC_SLANG

#include "geometry_data_inc.slang"
#include "../oct_inc.slang"

struct GeometryHitInput {
	GeometryData geometry;
	uint geometry_index;
	uint primitive_id;
	int cluster_id;
	float2 barycentrics;
	float3x4 object_to_world;
};

struct GeometryFootprint {
	float4x4 world_to_clip;
	float2 pixel_size;
};

struct GeometryTriangle {
	uint3 indices;
	float3 p0;
	float3 p1;
	float3 p2;
	float2 uv0;
	float2 uv1;
	float2 uv2;
};

float4x4 geometry_inverse(float4x4 value) {
	float4x4 cofactors;
	[unroll]
	for (uint row = 0u; row < 4u; row++) {
		uint3 rows = uint3(row > 0u ? 0u : 1u, row > 1u ? 1u : 2u, row > 2u ? 2u : 3u);
		[unroll]
		for (uint column = 0u; column < 4u; column++) {
			uint3 columns = uint3(column > 0u ? 0u : 1u, column > 1u ? 1u : 2u, column > 2u ? 2u : 3u);
			float3 a = float3(value[rows.x][columns.x], value[rows.x][columns.y], value[rows.x][columns.z]);
			float3 b = float3(value[rows.y][columns.x], value[rows.y][columns.y], value[rows.y][columns.z]);
			float3 c = float3(value[rows.z][columns.x], value[rows.z][columns.y], value[rows.z][columns.z]);
			cofactors[row][column] = (((row + column) & 1u) == 0u ? 1.0 : -1.0) * dot(a, cross(b, c));
		}
	}
	return transpose(cofactors) / dot(value[0], cofactors[0]);
}

bool geometry_resolve_primitive(GeometryData geometry, int cluster_id, uint primitive_id, out uint primitive) {
	primitive = 0u;
	if ((geometry.flags & FLAG_CLUSTERED) != 0u && cluster_id != -1) {
		uint64_t address = (uint64_t(geometry.cluster_remap_address_hi) << 32u) | uint64_t(geometry.cluster_remap_address_lo);
		if (address == 0 || cluster_id < 0 || uint(cluster_id) >= geometry.cluster_count) {
			return false;
		}
		uint first_primitive = ((uint*)address)[uint(cluster_id)];
		if (first_primitive >= geometry.primitive_count || primitive_id >= geometry.primitive_count - first_primitive) {
			return false;
		}
		primitive = first_primitive + primitive_id;
		return true;
	}
	if (primitive_id >= geometry.primitive_count) {
		return false;
	}
	primitive = primitive_id;
	return true;
}

bool geometry_triangle_indices(GeometryData geometry, uint primitive, out uint3 indices) {
	indices = uint3(0u);
	if (primitive >= geometry.primitive_count) {
		return false;
	}
	if (geometry.index_format == 2u) {
		if (geometry.vertex_count < 3u || primitive > (geometry.vertex_count - 3u) / 3u) {
			return false;
		}
		indices = primitive * 3u + uint3(0u, 1u, 2u);
	} else {
		if (geometry.index_address == 0) {
			return false;
		}
		if (geometry.index_format == 0u) {
			uint16_t* source = (uint16_t*)(geometry.index_address + uint64_t(primitive) * 6u);
			indices = uint3(source[0], source[1], source[2]);
		} else if (geometry.index_format == 1u) {
			uint* source = (uint*)(geometry.index_address + uint64_t(primitive) * 12u);
			indices = uint3(source[0], source[1], source[2]);
		} else {
			return false;
		}
	}
	return all(indices < geometry.vertex_count);
}

void geometry_fetch_bones(GeometryData geometry, uint vertex, out uint4 bones, out float4 weights) {
	bones = uint4(0u);
	weights = float4(0.0);
	if (geometry.skin_address == 0) {
		return;
	}
	if (geometry.multimesh_address != 0) {
		vertex %= geometry.source_vertex_count;
	}
	uint64_t address = geometry.skin_address + uint64_t(vertex) * geometry.skin_stride;
	uint2 packed_bones = *((uint2*)address);
	uint2 packed_weights = *((uint2*)(address + geometry.skin_weight_offset));
	bones = uint4(packed_bones.x & 65535u, packed_bones.x >> 16u, packed_bones.y & 65535u, packed_bones.y >> 16u);
	weights = float4(packed_weights.x & 65535u, packed_weights.x >> 16u, packed_weights.y & 65535u, packed_weights.y >> 16u) / 65535.0;
}

float3 geometry_fetch_position(GeometryData geometry, uint vertex) {
	if (geometry.vertex_address == 0 || vertex >= geometry.vertex_count) {
		return float3(0.0);
	}
	if ((geometry.flags & FLAG_COMPRESSED) != 0u) {
		uint* source = (uint*)(geometry.vertex_address + uint64_t(vertex) * 8u);
		uint word0 = source[0];
		uint word1 = source[1];
		float3 position = float3(word0 & 0xFFFFu, word0 >> 16u, word1 & 0xFFFFu) / 65535.0;
		return geometry.position_offset + position * geometry.position_scale;
	}
	if (geometry.position_stride < 12u || (geometry.position_stride & 3u) != 0u) {
		return float3(0.0);
	}
	float* source = (float*)(geometry.vertex_address + uint64_t(vertex) * geometry.position_stride);
	return float3(source[0], source[1], source[2]);
}

float2 _geometry_fetch_vertex_uv(GeometryData geometry, uint vertex) {
	bool compressed = (geometry.flags & FLAG_COMPRESSED) != 0u;
	uint size = compressed ? 4u : 8u;
	if (vertex >= geometry.vertex_count || geometry.attribute_address == 0 ||
			geometry.uv_byte_offset == OFFSET_NONE || geometry.attribute_stride < size ||
			geometry.uv_byte_offset > geometry.attribute_stride - size ||
			((geometry.attribute_stride | geometry.uv_byte_offset) & 3u) != 0u) {
		return float2(0.0);
	}
	uint64_t address = geometry.attribute_address + uint64_t(vertex) * geometry.attribute_stride + geometry.uv_byte_offset;
	if (compressed) {
		uint packed_uv = *((uint*)address);
		float2 uv = float2(packed_uv & 0xFFFFu, packed_uv >> 16u) / 65535.0;
		float2 scale = float2(f16tof32(geometry.uv_scale_packed), f16tof32(geometry.uv_scale_packed >> 16u));
		return any(scale != 0.0) ? (uv - 0.5) * scale : uv;
	}
	float* source = (float*)address;
	return float2(source[0], source[1]);
}

float2 geometry_fetch_uv(GeometryData geometry, uint3 indices, float3 bary) {
	return _geometry_fetch_vertex_uv(geometry, indices.x) * bary.x +
			_geometry_fetch_vertex_uv(geometry, indices.y) * bary.y +
			_geometry_fetch_vertex_uv(geometry, indices.z) * bary.z;
}

uint geometry_instance_index(GeometryData geometry, uint vertex) {
	return geometry.source_vertex_count > 0u ? vertex / geometry.source_vertex_count : geometry.instance_index;
}

float4x4 geometry_instance_transform(GeometryData geometry, uint vertex) {
	if (geometry.multimesh_address == 0) {
		return float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
	}
	float* data = (float*)(geometry.multimesh_address + uint64_t(geometry.multimesh_offset + geometry_instance_index(geometry, vertex)) * geometry.multimesh_stride * 4u);
	return float4x4(float4(data[0], data[1], data[2], data[3]), float4(data[4], data[5], data[6], data[7]), float4(data[8], data[9], data[10], data[11]), float4(0.0, 0.0, 0.0, 1.0));
}

float4 geometry_instance_custom(GeometryData geometry, uint vertex) {
	if (geometry.multimesh_address == 0 || (geometry.multimesh_flags & 2u) == 0u) {
		return geometry.instance_custom;
	}
	uint offset = (geometry.multimesh_offset + geometry_instance_index(geometry, vertex)) * geometry.multimesh_stride + 12u + ((geometry.multimesh_flags & 1u) != 0u ? 4u : 0u);
	float* data = (float*)(geometry.multimesh_address + uint64_t(offset) * 4u);
	return float4(data[0], data[1], data[2], data[3]);
}

float4 geometry_instance_color(GeometryData geometry, uint vertex) {
	if (geometry.multimesh_address == 0 || (geometry.multimesh_flags & 1u) == 0u) {
		return geometry.instance_color;
	}
	uint offset = (geometry.multimesh_offset + geometry_instance_index(geometry, vertex)) * geometry.multimesh_stride + 12u;
	float* data = (float*)(geometry.multimesh_address + uint64_t(offset) * 4u);
	return float4(data[0], data[1], data[2], data[3]);
}

float4 geometry_fetch_color(GeometryData geometry, uint3 indices, float3 bary) {
	if (any(indices >= geometry.vertex_count) || geometry.attribute_address == 0 ||
			geometry.color_byte_offset == OFFSET_NONE || geometry.attribute_stride < 4u ||
			geometry.color_byte_offset > geometry.attribute_stride - 4u ||
			((geometry.attribute_stride | geometry.color_byte_offset) & 3u) != 0u) {
		return geometry_instance_color(geometry, indices.x);
	}
	float4 color = float4(0.0);
	for (uint i = 0u; i < 3u; i++) {
		uint64_t address = geometry.attribute_address + uint64_t(indices[i]) * geometry.attribute_stride + geometry.color_byte_offset;
		uint packed_color = *((uint*)address);
		color += float4(packed_color & 0xFFu, (packed_color >> 8u) & 0xFFu,
				(packed_color >> 16u) & 0xFFu, packed_color >> 24u) * (bary[i] / 255.0);
	}
	return color * geometry_instance_color(geometry, indices.x);
}

float2 geometry_fetch_uv2(GeometryData geometry, uint3 indices, float3 bary) {
	geometry.uv_byte_offset = geometry.uv2_byte_offset;
	geometry.uv_scale_packed = geometry.uv2_scale_packed;
	return geometry_fetch_uv(geometry, indices, bary);
}

float4 geometry_fetch_custom(GeometryData geometry, uint vertex, uint channel) {
	uint offset = geometry.custom_byte_offsets[channel];
	if (geometry.attribute_address == 0 || offset == OFFSET_NONE || vertex >= geometry.vertex_count) {
		return float4(0.0);
	}
	uint format = (geometry.custom_formats >> (channel * 3u)) & 7u;
	uint* source = (uint*)(geometry.attribute_address + uint64_t(vertex) * geometry.attribute_stride + offset);
	if (format < 2u) {
		uint packed = source[0];
		if (format == 0u) {
			return float4(packed & 255u, (packed >> 8u) & 255u, (packed >> 16u) & 255u, packed >> 24u) / 255.0;
		}
		return max(float4(int(packed << 24u) >> 24, int(packed << 16u) >> 24, int(packed << 8u) >> 24, int(packed) >> 24) / 127.0, -1.0);
	}
	if (format < 4u) {
		float4 value = float4(f16tof32(source[0] & 65535u), f16tof32(source[0] >> 16u), 0.0, 1.0);
		if (format == 3u) {
			value.zw = float2(f16tof32(source[1] & 65535u), f16tof32(source[1] >> 16u));
		}
		return value;
	}
	float4 value = float4(asfloat(source[0]), 0.0, 0.0, 1.0);
	for (uint component = 1u; component <= format - 4u; component++) {
		value[component] = asfloat(source[component]);
	}
	return value;
}

void geometry_fetch_tbn(GeometryData geometry, uint vertex, out float3 normal, out float3 tangent, out float3 binormal) {
	normal = float3(0.0, 0.0, 1.0);
	tangent = float3(1.0, 0.0, 0.0);
	binormal = float3(0.0, 1.0, 0.0);
	if (geometry.normal_byte_offset == OFFSET_NONE || geometry.vertex_address == 0 || vertex >= geometry.vertex_count) {
		return;
	}
	uint packed = *((uint*)(geometry.vertex_address + geometry.normal_byte_offset + uint64_t(vertex) * geometry.normal_stride));
	normal = oct_to_vec3(float2(packed & 65535u, packed >> 16u) / 32767.5 - 1.0);
	if ((geometry.flags & FLAG_COMPRESSED) != 0u) {
		float angle = float(*((uint*)(geometry.vertex_address + uint64_t(vertex) * 8u + 4u)) >> 16u) / 65535.0;
		float handedness = angle > 0.5 ? 1.0 : -1.0;
		angle = abs(angle * 2.0 - 1.0) * 3.141592653589793;
		float c = cos(angle);
		float s = sin(angle);
		float3 axis = normal;
		float3 a = (1.0 - c) * axis;
		float3 b = s * axis;
		tangent = a.xxx * axis + float3(c, -b.z, b.y);
		binormal = (a.yyy * axis + float3(b.z, c, -b.x)) * handedness;
		normal = a.zzz * axis + float3(-b.y, b.x, c);
	} else if (geometry.tangent_byte_offset != OFFSET_NONE) {
		uint packed_tangent = *((uint*)(geometry.vertex_address + geometry.tangent_byte_offset + uint64_t(vertex) * geometry.tangent_stride + 4u));
		float2 encoded = float2(packed_tangent & 65535u, packed_tangent >> 16u) / 32767.5 - 1.0;
		tangent = oct_to_vec3(float2(encoded.x, abs(encoded.y) * 2.0 - 1.0));
		binormal = normalize(cross(normal, tangent)) * sign(encoded.y);
	} else {
		float3 axis = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
		tangent = normalize(cross(axis, normal));
		binormal = cross(normal, tangent);
	}
}

GeometryData geometry_previous_positions(GeometryData geometry) {
	uint64_t address = (uint64_t(geometry.prev_vertex_address_hi) << 32u) | uint64_t(geometry.prev_vertex_address_lo);
	if (address != 0) {
		geometry.vertex_address = address;
		geometry.position_stride = 12u;
		geometry.flags &= ~FLAG_COMPRESSED;
	}
	return geometry;
}

bool geometry_decode_triangle(GeometryHitInput hit, out GeometryTriangle triangle) {
	triangle = (GeometryTriangle)0;
	GeometryData geometry = hit.geometry;
	if (geometry.vertex_address == 0 ||
			((geometry.flags & FLAG_COMPRESSED) == 0u && (geometry.position_stride < 12u || (geometry.position_stride & 3u) != 0u))) {
		return false;
	}
	uint primitive;
	if (!geometry_resolve_primitive(geometry, hit.cluster_id, hit.primitive_id, primitive) ||
			!geometry_triangle_indices(geometry, primitive, triangle.indices)) {
		return false;
	}
	triangle.p0 = geometry_fetch_position(geometry, triangle.indices.x);
	triangle.p1 = geometry_fetch_position(geometry, triangle.indices.y);
	triangle.p2 = geometry_fetch_position(geometry, triangle.indices.z);
	triangle.uv0 = _geometry_fetch_vertex_uv(geometry, triangle.indices.x);
	triangle.uv1 = _geometry_fetch_vertex_uv(geometry, triangle.indices.y);
	triangle.uv2 = _geometry_fetch_vertex_uv(geometry, triangle.indices.z);
	return true;
}

void geometry_footprint_derivatives(GeometryHitInput hit, GeometryTriangle triangle, GeometryFootprint footprint,
		out float3 object_position, out float3 position_dx, out float3 position_dy,
		out float2 uv, out float2 uv_dx, out float2 uv_dy) {
	float3 bary = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
	object_position = triangle.p0 * bary.x + triangle.p1 * bary.y + triangle.p2 * bary.z;
	uv = triangle.uv0 * bary.x + triangle.uv1 * bary.y + triangle.uv2 * bary.z;
	float3 normal = cross(triangle.p1 - triangle.p0, triangle.p2 - triangle.p0);
	float4x4 object_to_world = float4x4(hit.object_to_world[0], hit.object_to_world[1], hit.object_to_world[2], float4(0.0, 0.0, 0.0, 1.0));
	float4x4 object_to_clip = mul(footprint.world_to_clip, object_to_world);
	float4x4 clip_to_object = geometry_inverse(object_to_clip);
	float4 clip = mul(object_to_clip, float4(object_position, 1.0));
	float4 clip_plane = mul(transpose(clip_to_object), float4(normal, -dot(normal, triangle.p0)));
	float plane_denominator = (clip_plane.z < 0.0 ? -1.0 : 1.0) * max(abs(clip_plane.z), 1e-20);
	float3 derivatives[2];
	float4x4 inverse_columns = transpose(clip_to_object);
	for (uint axis = 0u; axis < 2u; axis++) {
		float4 homogeneous_derivative = inverse_columns[axis] - inverse_columns[2] * (clip_plane[axis] / plane_denominator);
		derivatives[axis] = (homogeneous_derivative.xyz - object_position * homogeneous_derivative.w) * clip.w * 2.0 * footprint.pixel_size[axis];
	}
	position_dx = derivatives[0];
	position_dy = derivatives[1];
	float3 barycentric_u = cross(triangle.p2 - triangle.p0, normal) / max(dot(normal, normal), 1e-20);
	float3 barycentric_v = cross(normal, triangle.p1 - triangle.p0) / max(dot(normal, normal), 1e-20);
	uv_dx = (triangle.uv1 - triangle.uv0) * dot(barycentric_u, position_dx) + (triangle.uv2 - triangle.uv0) * dot(barycentric_v, position_dx);
	uv_dy = (triangle.uv1 - triangle.uv0) * dot(barycentric_u, position_dy) + (triangle.uv2 - triangle.uv0) * dot(barycentric_v, position_dy);
}

#endif
