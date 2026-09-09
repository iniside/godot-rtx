#pragma once

#include "core/typedefs.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/micro_geometry_select.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/micro_geometry_hzb.slang.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/micro_geometry_storage.h"

namespace RendererSceneRenderImplementation {

struct MicroGeometrySelectedCluster {
	uint64_t instance = 0;
	uint64_t surface = 0;
	uint64_t asset = 0;
	uint64_t asset_address = 0;
	uint64_t material_generation = 0;
	uint32_t cluster = 0;
	uint32_t page_generation = 0;
	uint32_t multimesh_instance = 0;
	uint32_t bin = 0;
	uint32_t material_slot = 0;
	uint32_t raster_instance = 0;
};

struct MicroGeometryRasterParameters {
	uint64_t page_pool = 0;
	uint32_t selected_cluster_color = 0;
	uint32_t pad = 0;
};

static_assert(sizeof(MicroGeometrySelectedCluster) == 64);
static_assert(sizeof(MicroGeometryRasterParameters) == 16);

class MicroGeometrySelection {
public:
	struct Task {
		uint64_t instance = 0;
		uint64_t surface = 0;
		uint32_t group_offset = 0;
		uint32_t cluster_offset = 0;
		uint32_t coarse_offset = 0;
		uint32_t group_count = 0;
		uint32_t cluster_count = 0;
		uint32_t coarse_count = 0;
		uint32_t multimesh_count = 0;
		uint32_t bin = 0;
		uint32_t flags = 0;
		uint32_t gi_offset = UINT32_MAX;
		uint64_t asset = 0;
		uint64_t indirect_command = 0;
	};
	struct Bin {
		uint32_t offset = 0;
		uint32_t capacity = 0;
	};
	struct Parameters {
		float projection[16] = {};
		float previous_projection[16] = {};
		float view_rotation[12] = {};
		float previous_view_rotation[12] = {};
		float camera[4] = {};
		float camera_low[4] = {};
		float previous_camera[4] = {};
		float previous_camera_low[4] = {};
		uint64_t scenario = 0;
		uint32_t layer_mask = UINT32_MAX;
		uint32_t task_count = 0;
		uint32_t group_work = 0;
		uint32_t cluster_work = 0;
		uint32_t coarse_work = 0;
		uint32_t bin_count = 0;
		uint32_t level = 0;
		uint32_t mode = 0;
		float error = 1;
		float offscreen_multiplier = 1;
		float near_plane = 0;
		float output_height = 1;
		uint32_t flags = 0;
		uint32_t hzb_width = 0;
		uint32_t hzb_height = 0;
		uint32_t hzb_mips = 0;
		uint64_t lightmaps[8] = {};
		uint32_t lightmap_sh = 0;
		uint32_t pad = 0;
	};
	struct Pass {
		LocalVector<RID> resources;
		Vector<RID> dependencies;
		RID tasks;
		RID bins;
		RID parameters;
		RID group_states;
		RID rejected;
		RID validity;
		RID counts;
		RID initial_counts;
		RID capacity_state;
		RID commands;
		RID selected;
		RID native_instances;
		RID raster_parameters;
		RID feedback;
		RID requests;
		RID persistent_instances;
		RID persistent_surfaces;
		Vector<Bin> bin_data;
		Parameters data;
		uint32_t levels = 0;
		uint32_t selected_capacity = 0;
		uint64_t memory_bytes = 0;
		~Pass();
	};
	struct DepthPyramid {
		RID texture;
		Vector<RID> levels;
		Size2i size;
		~DepthPyramid();
	};
	static constexpr uint32_t MAX_WORK_ITEMS = 4 * 1024 * 1024;
	static constexpr uint32_t EXTRA_SELECTED_CLUSTERS = 256 * 1024;
	Pass *create(const Vector<Task> &p_tasks, const Vector<Bin> &p_bins, const Parameters &p_parameters, uint32_t p_levels, uint32_t p_native_stride, RID p_instances, RID p_surfaces, const Vector<RID> &p_dependencies);
	void select(Pass *p_pass, RID p_hzb);
	void recover(Pass *p_pass, RID p_hzb);
	void build_depth_pyramid(DepthPyramid &r_pyramid, RID p_depth, const Size2i &p_size);
	RID get_raster_uniform_set(Pass *p_pass, RID p_shader);
	void add_draw_dependencies(Pass *p_pass, RD::DrawListID p_list);
	void submit_feedback(Pass *p_pass);
	MicroGeometrySelection();
	~MicroGeometrySelection();

private:
	MicroGeometrySelectShaderRD shader;
	MicroGeometryHzbShaderRD hzb_shader;
	RID version;
	RID pipeline;
	RID hzb_version;
	RID hzb_pipeline;
	RID _buffer(Pass &r_pass, uint64_t p_size, const void *p_data = nullptr, uint32_t p_usage = 0);
	void _dispatch(Pass *p_pass, uint32_t p_mode, uint32_t p_items, RID p_hzb);
};

static_assert(sizeof(MicroGeometrySelection::Task) == 72);
static_assert(sizeof(MicroGeometrySelection::Parameters) == 432);

}
