#pragma once

#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/typedefs.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/micro_geometry_hzb.slang.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/micro_geometry_select.slang.gen.h"
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
	uint32_t dispatch_width = 1;
};

static_assert(sizeof(MicroGeometrySelectedCluster) == 64);
static_assert(sizeof(MicroGeometryRasterParameters) == 16);

class MicroGeometrySelection {
public:
	struct Task {
		uint64_t instance = 0;
		uint64_t surface = 0;
		uint32_t group_count = 0;
		uint32_t cluster_count = 0;
		uint32_t coarse_count = 0;
		uint32_t multimesh_count = 0;
		uint32_t bin = 0;
		uint32_t flags = 0;
		uint32_t gi_offset = UINT32_MAX;
		uint32_t pad = 0;
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
		uint32_t queue_work = 0;
		uint32_t record_work = 0;
		uint32_t unit_count = 0;
		uint32_t bin_count = 0;
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
		uint32_t cull_plane_count = 0;
		float cull_planes[96] = {};
	};
	struct Unit {
		uint32_t task = 0;
		uint32_t ordinal = 0;
		uint32_t queue_offset = 0;
		uint32_t queue_capacity = 0;
		uint32_t record_offset = 0;
		uint32_t record_capacity = 0;
		uint32_t hash_offset = 0;
		uint32_t pad = 0;
	};
	struct CapacityKey {
		uint64_t surface = 0;
		uint64_t asset = 0;
		uint32_t ordinal = 0;
		bool operator==(const CapacityKey &p_other) const { return surface == p_other.surface && asset == p_other.asset && ordinal == p_other.ordinal; }
		static uint32_t hash(const CapacityKey &p_key) { return hash_murmur3_one_32(p_key.ordinal, hash_murmur3_one_64(p_key.asset, hash_murmur3_one_64(p_key.surface))); }
	};
	struct Capacity {
		uint32_t queue = 0;
		uint32_t records = 0;
	};
	struct CapacityHistory : RefCounted {
		struct Entry {
			Capacity capacity;
			uint32_t owners = 0;
			uint64_t last_owner = 0;
			List<CapacityKey>::Element *retired = nullptr;
		};
		HashMap<CapacityKey, Entry, CapacityKey> capacities;
		List<CapacityKey> retired;
		uint64_t next_owner = 0;
		uint64_t retired_callback_floor = 0;
		uint64_t retirement_revision = 0;
	};
	struct CapacityFeedback : RefCounted {
		Ref<CapacityHistory> history;
		Vector<CapacityKey> keys;
		Vector<Capacity> limits;
		Vector<Capacity> allocated;
		Vector<uint8_t> leased;
		uint64_t owner = 0;
		uint64_t blocked_revision = 0;
		uint64_t sample_frame = 0;
		uint64_t completed_frame = 0;
		uint32_t queue_overflows = 0;
		uint32_t record_overflows = 0;
		bool retired = false;
		bool pending = false;
		bool retry = false;
		bool failed = false;
	};
	struct Pass {
		struct Pin {
			RID asset;
			uint32_t group = 0;
		};
		Vector<Pin> pins;
		Vector<RID> assets;
		uint64_t input_version = 0;
		uint32_t input_kind = 0;
		bool frozen = false;
		bool freeze_requested = false;
		MicroGeometryRasterParameters raster_data;
		bool recovered = false;
		bool raster_memory_accounted = false;
		RID statistics;
		LocalVector<RID> resources;
		Vector<RID> dependencies;
		RID tasks;
		RID bins;
		RID parameters;
		RID units;
		RID record_order;
		RID queue_order;
		RID queue;
		RID sparse_states;
		RID unit_states;
		RID candidate;
		RID committed;
		RID rejected;
		RID validity;
		RID counts;
		RID initial_counts;
		RID dispatch_arguments;
		RID selected;
		RID native_instances;
		RID raster_parameters;
		RID feedback;
		RID requests;
		RID requests_fallback;
		bool feedback_active = false;
		RID persistent_instances;
		RID persistent_surfaces;
		Vector<Bin> bin_data;
		Vector<Task> task_data;
		Vector<Unit> unit_data;
		Vector<uint32_t> record_order_data;
		Vector<uint32_t> queue_order_data;
		Ref<CapacityFeedback> capacity_feedback;
		bool admission_failed = false;
		LocalVector<RID> retired_buffers;
		uint64_t retired_submission = 0;
		uint64_t retired_bytes = 0;
		uint64_t dynamic_memory_bytes = 0;
		uint64_t fixed_memory_bytes = 0;
		uint64_t requested_bytes = 0;
		uint64_t replacement_peak_bytes = 0;
		uint64_t profile_frame = UINT64_MAX;
		uint64_t last_used_frame = 0;
		uint32_t resize_attempts = 0;
		uint32_t append_attempts = 0;
		const char *allocation_status = "not_attempted";
		Parameters data;
		uint32_t levels = 0;
		uint32_t selected_capacity = 0;
		uint32_t queue_reserved = 0;
		uint32_t record_reserved = 0;
		uint32_t queue_holes = 0;
		uint32_t record_holes = 0;
		uint64_t memory_bytes = 0;
		~Pass();
	};
	struct DepthPyramid {
		RID texture;
		Vector<RID> levels;
		Size2i size;
		~DepthPyramid();
	};
	static constexpr uint64_t MAX_PASS_BYTES = 2ULL * 1024 * 1024 * 1024;
	static constexpr uint32_t STATISTICS_BYTES = 380;
	static constexpr uint32_t RESERVE_PERCENT = 125;
	static constexpr uint32_t HOLE_PERCENT = 50;
	Pass *create(const Vector<Task> &p_tasks, uint32_t p_bin_count, const Parameters &p_parameters, uint32_t p_levels, uint32_t p_native_stride, RID p_instances, RID p_surfaces, const Vector<RID> &p_dependencies);
	bool needs_retry(Pass *p_pass) const;
	void select(Pass *p_pass, RID p_hzb);
	void update_unit_schedule(Pass *p_pass, const LocalVector<uint8_t> &p_deferred);
	void recover(Pass *p_pass, RID p_hzb);
	void update_frozen(Pass *p_pass);
	bool freeze(Pass *p_pass);
	void build_depth_pyramid(DepthPyramid &r_pyramid, RID p_depth, RID p_classification, const Size2i &p_size);
	RID get_raster_uniform_set(Pass *p_pass, RID p_shader);
	void add_draw_dependencies(Pass *p_pass, RD::DrawListID p_list);
	void submit_feedback(Pass *p_pass);
	MicroGeometrySelection();
	~MicroGeometrySelection();

private:
	struct PushConstant {
		uint32_t mode;
		uint32_t level;
	};
	static_assert(sizeof(PushConstant) == 8);

	MicroGeometrySelectShaderRD shader;
	MicroGeometryHzbShaderRD hzb_shader;
	RID version;
	RID pipeline;
	RID hzb_version;
	RID hzb_pipeline;
	Ref<CapacityHistory> capacity_history;
	RID _buffer(Pass &r_pass, uint64_t p_size, const void *p_data = nullptr, uint32_t p_usage = 0);
	static uint32_t _build_bins(const Vector<Task> &p_tasks, const Vector<Unit> &p_units, uint32_t p_bin_count, Vector<Bin> &r_bins);
	static bool _validate_bins(Pass *p_pass, const Vector<Bin> &p_bins);
	static void _upload_order(Pass *p_pass);
	bool _grow(Pass *p_pass, const Vector<Unit> &p_units);
	bool _resize(Pass *p_pass, const Vector<Unit> &p_units);
	static void _retire_buffers(Pass *p_pass);
	static void _report_selection(Pass *p_pass);
	static Capacity *_capacity_entry(CapacityFeedback *p_feedback, uint32_t p_index, bool p_create);
	static void _retire_capacity(CapacityFeedback *p_feedback);
	static void _capacity_feedback(const Vector<uint8_t> &p_bytes, Ref<RefCounted> p_feedback);
	RID _prepare_dispatch(Pass *p_pass, RID p_hzb);
	void _dispatch(Pass *p_pass, uint32_t p_mode, uint32_t p_items, RID p_uniform_set, uint32_t p_level = 0);
};

static_assert(sizeof(MicroGeometrySelection::Task) == 64);
static_assert(sizeof(MicroGeometrySelection::Unit) == 32);
static_assert(sizeof(MicroGeometrySelection::Parameters) == 808);

} //namespace RendererSceneRenderImplementation
