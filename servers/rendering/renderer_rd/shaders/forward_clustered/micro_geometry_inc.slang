#ifndef MICRO_GEOMETRY_INC_SLANG
#define MICRO_GEOMETRY_INC_SLANG

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

float4 micro_geometry_attribute(uint64_t p_vertex, MicroGeometrySurface p_surface, uint p_attribute, uint p_components, float4 p_default) {
	uint offset = p_surface.attribute_offsets[p_attribute];
	if (offset == 0xffffffff) {
		return p_default;
	}
	for (uint component = 0; component < p_components; component++) {
		p_default[component] = asfloat(micro_geometry_load_word(p_vertex + offset + component * 4));
	}
	return p_default;
}

float4 micro_geometry_custom(uint64_t p_vertex, MicroGeometrySurface p_surface, uint p_channel) {
	uint offset = p_surface.attribute_offsets[6 + p_channel];
	if (offset == 0xffffffff) {
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
