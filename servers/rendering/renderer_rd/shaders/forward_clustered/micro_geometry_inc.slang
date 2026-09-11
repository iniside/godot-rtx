#ifndef MICRO_GEOMETRY_INC_SLANG
#define MICRO_GEOMETRY_INC_SLANG

#include "../oct_inc.slang"

static const uint MICRO_GEOMETRY_FORMAT_POSITION_SNORM16 = 1;
static const uint MICRO_GEOMETRY_FORMAT_NORMAL_OCTAHEDRAL8 = 2;
static const uint MICRO_GEOMETRY_FORMAT_COLOR_UNORM8 = 3;
static const uint MICRO_GEOMETRY_FORMAT_UV_UNORM16 = 4;
static const uint MICRO_GEOMETRY_FORMAT_UV_HALF = 5;
static const uint MICRO_GEOMETRY_FORMAT_CUSTOM = 6;
static const uint MICRO_GEOMETRY_OFFSET_NONE = 0xffffffff;

struct MicroGeometryPage {
	uint slot;
	uint generation;
	uint ready;
	uint pad;
};

struct MicroGeometryCluster {
	uint surface;
	uint group;
	uint refined_group;
	uint page;
	uint payload_offset;
	uint vertex_count;
	uint triangle_count;
	uint first_primitive;
	float center[3];
	float radius;
	float error;
	uint padding[3];
};

struct MicroGeometryGroup {
	uint first_cluster;
	uint cluster_count;
	uint depth;
	uint first_parent;
	float center[3];
	float radius;
	float error;
	uint parent_count;
	uint padding[2];
};

struct MicroGeometrySurface {
	uint64_t format;
	uint source_surface;
	uint vertex_stride;
	uint attribute_offsets[10];
	uint attribute_formats[10];
	uint source_vertex_count;
	uint source_triangle_count;
	float frame_center[3];
	float frame_scale;
	uint uv_mode[2];
	float uv_min[4];
	float uv_scale[4];
};

struct MicroGeometryNode {
	uint group;
	uint first_child;
	uint child_count;
	uint pad;
	float center[3];
	float radius;
	float error;
	uint padding[3];
};

struct MicroGeometryAsset {
	uint64_t identity;
	uint64_t clusters;
	uint64_t groups;
	uint64_t surfaces;
	uint64_t nodes;
	uint64_t terminals;
	uint64_t roots;
	uint64_t pages;
	uint64_t group_states;
	uint cluster_count;
	uint group_count;
	uint surface_count;
	uint node_count;
	uint terminal_count;
	uint root_count;
	uint page_count;
	uint ready;
	uint64_t residency_generation;
	uint64_t parent_groups;
};

struct MicroGeometryInstance {
	uint64_t handle;
	uint64_t identity;
	uint64_t scenario;
	uint64_t asset;
	uint64_t asset_address;
	float transform[12];
	float previous_transform[12];
	float origin_low[4];
	float previous_origin_low[4];
	float aabb_position[4];
	float aabb_size[4];
	uint64_t multimesh_address;
	uint64_t multimesh_generation;
	uint64_t lightmap;
	float lightmap_uv_scale[4];
	float lightmap_sh[36];
	uint64_t first_surface;
	uint surface_count;
	uint flags;
	uint layer_mask;
	uint instance_uniforms_offset;
	uint multimesh_stride;
	uint multimesh_current_offset;
	uint multimesh_previous_offset;
	uint multimesh_count;
	uint lightmap_slice;
	uint visible;
	uint shadows;
	uint deformed;
	float fade_near_begin;
	float fade_near_end;
	float fade_far_begin;
	float fade_far_end;
	float force_alpha;
	float parent_fade_alpha;
	float lod_bias;
	float model_scale;
};

struct MicroGeometryInstanceSurface {
	uint64_t handle;
	uint64_t instance;
	uint64_t next_surface;
	uint64_t material;
	uint64_t material_generation;
	uint material_slot;
	uint source_surface;
	uint pass_index;
	uint flags;
	uint rt_pass_flags;
	uint material_flags;
	uint force_finest;
	uint pad;
};

struct MicroGeometrySelectedCluster {
	uint64_t instance;
	uint64_t surface;
	uint64_t asset;
	uint64_t asset_address;
	uint64_t material_generation;
	uint cluster;
	uint page_generation;
	uint multimesh_instance;
	uint bin;
	uint material_slot;
	uint raster_instance;
};

struct MicroGeometryRasterParameters {
	uint64_t page_pool;
	uint selected_cluster_color;
	uint dispatch_width;
};

uint micro_geometry_load_byte(uint64_t p_address) {
	uint word = *((uint*)(p_address & ~uint64_t(3)));
	return (word >> (uint(p_address & 3) * 8)) & 255;
}

uint micro_geometry_load_word(uint64_t p_address) {
	if ((p_address & 3) == 0) {
		return *((uint*)p_address);
	}
	return micro_geometry_load_byte(p_address) | (micro_geometry_load_byte(p_address + 1) << 8) | (micro_geometry_load_byte(p_address + 2) << 16) | (micro_geometry_load_byte(p_address + 3) << 24);
}

uint micro_geometry_attribute_size(MicroGeometrySurface p_surface, uint p_attribute) {
	uint format = p_surface.attribute_formats[p_attribute];
	if (format == MICRO_GEOMETRY_FORMAT_POSITION_SNORM16) {
		return 6;
	}
	if (format == MICRO_GEOMETRY_FORMAT_NORMAL_OCTAHEDRAL8) {
		return 2;
	}
	if (format == MICRO_GEOMETRY_FORMAT_COLOR_UNORM8 || format == MICRO_GEOMETRY_FORMAT_UV_UNORM16 || format == MICRO_GEOMETRY_FORMAT_UV_HALF) {
		return 4;
	}
	if (format == MICRO_GEOMETRY_FORMAT_CUSTOM) {
		uint custom = uint(p_surface.format >> (13 + (p_attribute - 6) * 3)) & 7;
		return custom < 3 ? 4 : (custom == 3 ? 8 : (custom - 3) * 4);
	}
	return 0;
}

uint micro_geometry_index_offset(uint p_vertex_count, uint p_vertex_stride) {
	return (p_vertex_count * p_vertex_stride + 3) & ~3u;
}

uint micro_geometry_vertex_id_offset(uint p_vertex_count, uint p_triangle_count, uint p_vertex_stride) {
	return (micro_geometry_index_offset(p_vertex_count, p_vertex_stride) + p_triangle_count * 3 + 3) & ~3u;
}

bool micro_geometry_surface_valid(MicroGeometrySurface p_surface, uint p_payload_offset, uint p_vertex_count, uint p_triangle_count) {
	if (p_surface.vertex_stride == 0 || (p_surface.vertex_stride & 3) != 0 || (p_payload_offset & 3) != 0) {
		return false;
	}
	if (uint64_t(p_payload_offset) + micro_geometry_vertex_id_offset(p_vertex_count, p_triangle_count, p_surface.vertex_stride) + uint64_t(p_vertex_count) * 4 > 65536) {
		return false;
	}
	if (p_surface.attribute_offsets[0] == MICRO_GEOMETRY_OFFSET_NONE || p_surface.attribute_formats[0] != MICRO_GEOMETRY_FORMAT_POSITION_SNORM16) {
		return false;
	}
	for (uint attribute = 0; attribute < 10; attribute++) {
		uint offset = p_surface.attribute_offsets[attribute];
		if (offset != MICRO_GEOMETRY_OFFSET_NONE && uint64_t(offset) + micro_geometry_attribute_size(p_surface, attribute) > p_surface.vertex_stride) {
			return false;
		}
	}
	return true;
}

float3 micro_geometry_decode_position(uint64_t p_address) {
	uint low = micro_geometry_load_word(p_address);
	uint high = micro_geometry_load_word(p_address + 4);
	int3 packed = int3(int(low << 16) >> 16, int(low) >> 16, int(high << 16) >> 16);
	return max(float3(packed) / 32767.0, float3(-1.0));
}

float3 micro_geometry_decode_normal(uint64_t p_address) {
	float2 encoded = float2(micro_geometry_load_byte(p_address), micro_geometry_load_byte(p_address + 1)) / 127.5 - 1.0;
	return oct_to_vec3(encoded);
}

float2 micro_geometry_decode_uv(uint64_t p_address, MicroGeometrySurface p_surface, uint p_channel, uint p_format) {
	uint packed = micro_geometry_load_word(p_address);
	if (p_format == MICRO_GEOMETRY_FORMAT_UV_HALF) {
		return float2(f16tof32(packed & 65535), f16tof32(packed >> 16));
	}
	uint slot = p_channel * 2;
	float2 minimum = float2(p_surface.uv_min[slot], p_surface.uv_min[slot + 1]);
	float2 scale = float2(p_surface.uv_scale[slot], p_surface.uv_scale[slot + 1]);
	return minimum + float2(packed & 65535, packed >> 16) * scale / 65535.0;
}

float3 micro_geometry_frame_position(MicroGeometrySurface p_surface, float3 p_position) {
	return float3(p_surface.frame_center[0], p_surface.frame_center[1], p_surface.frame_center[2]) + p_position * p_surface.frame_scale;
}

float4x4 micro_geometry_frame_matrix(MicroGeometrySurface p_surface) {
	float scale = p_surface.frame_scale;
	return float4x4(
			float4(scale, 0.0, 0.0, p_surface.frame_center[0]),
			float4(0.0, scale, 0.0, p_surface.frame_center[1]),
			float4(0.0, 0.0, scale, p_surface.frame_center[2]),
			float4(0.0, 0.0, 0.0, 1.0));
}

float4 micro_geometry_attribute(uint64_t p_vertex, MicroGeometrySurface p_surface, uint p_attribute, float4 p_default) {
	uint offset = p_surface.attribute_offsets[p_attribute];
	if (offset == MICRO_GEOMETRY_OFFSET_NONE) {
		return p_default;
	}
	uint64_t address = p_vertex + offset;
	uint format = p_surface.attribute_formats[p_attribute];
	if (format == MICRO_GEOMETRY_FORMAT_POSITION_SNORM16) {
		return float4(micro_geometry_decode_position(address), p_default.w);
	}
	if (format == MICRO_GEOMETRY_FORMAT_NORMAL_OCTAHEDRAL8) {
		return float4(micro_geometry_decode_normal(address), p_default.w);
	}
	if (format == MICRO_GEOMETRY_FORMAT_COLOR_UNORM8) {
		uint packed = micro_geometry_load_word(address);
		return float4(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255, packed >> 24) / 255.0;
	}
	if (format == MICRO_GEOMETRY_FORMAT_UV_UNORM16 || format == MICRO_GEOMETRY_FORMAT_UV_HALF) {
		return float4(micro_geometry_decode_uv(address, p_surface, p_attribute - 4, format), p_default.zw);
	}
	return p_default;
}

float4 micro_geometry_custom(uint64_t p_vertex, MicroGeometrySurface p_surface, uint p_channel) {
	uint offset = p_surface.attribute_offsets[6 + p_channel];
	if (offset == MICRO_GEOMETRY_OFFSET_NONE) {
		return float4(0.0);
	}
	uint format = uint(p_surface.format >> (13 + p_channel * 3)) & 7;
	uint64_t address = p_vertex + offset;
	uint packed = micro_geometry_load_word(address);
	if (format == 0) {
		return float4(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255, packed >> 24) / 255.0;
	}
	if (format == 1) {
		return max(float4(int(packed << 24) >> 24, int(packed << 16) >> 24, int(packed << 8) >> 24, int(packed) >> 24) / 127.0, -1.0);
	}
	if (format < 4) {
		float4 value = float4(f16tof32(packed & 65535), f16tof32(packed >> 16), 0.0, 1.0);
		if (format == 3) {
			packed = micro_geometry_load_word(address + 4);
			value.zw = float2(f16tof32(packed & 65535), f16tof32(packed >> 16));
		}
		return value;
	}
	float4 value = float4(asfloat(packed), 0.0, 0.0, 1.0);
	for (uint component = 1; component <= format - 4; component++) {
		value[component] = asfloat(micro_geometry_load_word(address + component * 4));
	}
	return value;
}

float3 micro_geometry_cluster_color(MicroGeometrySelectedCluster p_selected) {
	uint hash = uint(p_selected.asset) ^ uint(p_selected.asset >> 32) ^ (p_selected.cluster * 0x9e3779b9);
	hash = (hash ^ (hash >> 16)) * 0x7feb352d;
	hash = (hash ^ (hash >> 15)) * 0x846ca68b;
	hash ^= hash >> 16;
	return 0.2 + 0.8 * float3(hash & 255, (hash >> 8) & 255, (hash >> 16) & 255) / 255.0;
}

#endif
