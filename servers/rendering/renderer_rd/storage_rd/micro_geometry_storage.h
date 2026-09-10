/**************************************************************************/
/*  micro_geometry_storage.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/micro_geometry_data.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/micro_geometry_page.slang.gen.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

class MicroGeometryStorage {
public:
	static constexpr uint32_t INVALID_SLOT = UINT32_MAX;
	static constexpr uint32_t PAGE_SIZE = MicroGeometryData::MAX_PAGE_SIZE;
	static constexpr uint32_t DEFAULT_PAGE_COUNT = 4096;
	static constexpr uint32_t MAX_IO_TASKS = 16;

	struct GPURequest {
		uint64_t asset = 0;
		uint32_t group = 0;
		uint32_t pad = 0;
	};
	struct FeedbackHeader {
		uint32_t count = 0;
		uint32_t overflow = 0;
		uint32_t capacity = 0;
		uint32_t pad = 0;
	};
	struct GPUPage {
		uint32_t slot = INVALID_SLOT;
		uint32_t generation = 0;
		uint32_t ready = 0;
		uint32_t pad = 0;
	};
	struct GPUCluster {
		uint32_t surface;
		uint32_t group;
		uint32_t refined_group;
		uint32_t page;
		uint32_t payload_offset;
		uint32_t vertex_count;
		uint32_t triangle_count;
		uint32_t pad;
		float center[3];
		float radius;
		float error;
		uint32_t padding[3];
	};
	struct GPUGroup {
		uint32_t first_cluster;
		uint32_t cluster_count;
		uint32_t depth;
		uint32_t first_parent;
		float center[3];
		float radius;
		float error;
		uint32_t parent_count;
		uint32_t padding[2];
	};
	struct GPUSurface {
		uint64_t format;
		uint32_t source_surface;
		uint32_t vertex_stride;
		uint32_t attribute_offsets[10];
		uint32_t source_vertex_count;
		uint32_t source_triangle_count;
	};
	struct GPUNode {
		uint32_t group;
		uint32_t first_child;
		uint32_t child_count;
		uint32_t pad;
		float center[3];
		float radius;
		float error;
		uint32_t padding[3];
	};
	struct GPUAsset {
		uint64_t identity = 0;
		uint64_t clusters = 0;
		uint64_t groups = 0;
		uint64_t surfaces = 0;
		uint64_t nodes = 0;
		uint64_t terminals = 0;
		uint64_t roots = 0;
		uint64_t pages = 0;
		uint64_t group_states = 0;
		uint32_t cluster_count = 0;
		uint32_t group_count = 0;
		uint32_t surface_count = 0;
		uint32_t node_count = 0;
		uint32_t terminal_count = 0;
		uint32_t root_count = 0;
		uint32_t page_count = 0;
		uint32_t ready = 0;
		uint64_t residency_generation = 0;
		uint64_t parent_groups = 0;
	};
	struct PagePin {
		RID asset;
		uint32_t page = 0;
		uint32_t generation = 0;
	};
	struct Statistics {
		uint64_t acceleration_structure_bytes = 0;
		uint64_t pool_bytes = 0;
		uint64_t metadata_bytes = 0;
		uint64_t retired_metadata_bytes = 0;
		uint64_t io_bytes = 0;
		uint64_t feedback_bytes = 0;
		uint64_t device_buffer_bytes = 0;
		uint32_t resident_pages = 0;
		uint32_t retiring_pages = 0;
		uint32_t pending_pages = 0;
		uint64_t pressure = 0;
		uint64_t failed_reads = 0;
		uint64_t clas_builds = 0;
		uint64_t clas_page_builds = 0;
		uint64_t resident_clas = 0;
		uint64_t raster_selection_bytes = 0;
	};

private:
	enum PageStatus { ABSENT,
		REQUESTED,
		READING,
		UPLOADING,
		RESIDENT,
		FAILED };
	struct Page {
		LocalVector<RID> clas_resources;
		RID clas_storage;
		uint64_t clas_bytes = 0;
		uint64_t clas_storage_bytes = 0;
		PageStatus status = ABSENT;
		GPUPage gpu;
		uint32_t pins = 0;
		bool terminal = false;
		uint64_t requested = 0;
		uint64_t upload_submission = 0;
		uint64_t clas_submission = 0;
	};
	struct Asset {
		Dependency dependency;
		LocalVector<RID> rt_resources;
		uint64_t rt_bytes = 0;
		RID clas_addresses;
		RID primitive_lookup;
		RID primitive_offset_buffer;
		Vector<uint32_t> primitive_offsets;
		Vector<uint8_t> empty_primitive_lookup;
		Ref<MicroGeometryData> source;
		bool publication_pending = false;
		bool groups_dirty = false;
		bool has_requests = false;
		uint32_t references = 1;
		LocalVector<Page> pages;
		LocalVector<Vector<uint32_t>> group_pages;
		LocalVector<uint32_t> group_pins;
		LocalVector<uint32_t> group_states;
		LocalVector<RID> buffers;
		RID descriptor_buffer;
		RID page_buffer;
		RID group_buffer;
		GPUAsset gpu;
		uint64_t metadata_bytes = 0;
	};
	struct Slot {
		RID asset;
		uint32_t page = 0;
		uint64_t retirement = 0;
	};
	struct TriangleInfo {
		uint32_t cluster_id = 0;
		uint32_t cluster_flags = 0;
		uint32_t packed_counts = 0;
		uint32_t geometry_flags = 0;
		uint16_t index_stride = 0;
		uint16_t vertex_stride = 0;
		uint16_t geometry_stride = 0;
		uint16_t opacity_stride = 0;
		uint64_t indices = 0;
		uint64_t vertices = 0;
		uint64_t geometry = 0;
		uint64_t opacity = 0;
		uint64_t opacity_indices = 0;
	};
	static_assert(sizeof(TriangleInfo) == 64);
	struct ReadTask {
		Ref<MicroGeometryData> source;
		RID asset;
		uint32_t page = 0;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		Vector<uint8_t> decoded;
		LocalVector<TriangleInfo> triangle_infos;
		RD::ClusterBuildInput build_input;
		uint32_t max_vertices_per_cluster = 0;
		uint32_t max_triangles_per_cluster = 0;
		Error error = OK;
	};
	struct RetiredMetadata {
		LocalVector<RID> buffers;
		uint64_t bytes = 0;
		uint64_t rt_bytes = 0;
		uint64_t submission = 0;
	};
	struct Feedback {
		bool retired = false;
		RID buffer;
		uint32_t capacity = 0;
		bool pending = false;
		bool retry = false;
	};
	mutable RID_Owner<Feedback> feedbacks;
	LocalVector<RID> active_feedbacks;
	uint32_t pending_feedback_count = 0;
	static void _feedback_dispatch(const Vector<uint8_t> &p_bytes, uint64_t p_storage, RID p_feedback);
	void _feedback_received(const Vector<uint8_t> &p_bytes, RID p_feedback);
	mutable RID_Owner<Asset> assets;
	HashMap<const MicroGeometryData *, RID> sources;
	LocalVector<RID> active_assets;
	LocalVector<Slot> slots;
	LocalVector<ReadTask *> tasks;
	LocalVector<RetiredMetadata> retired_metadata;
	RID pool;
	uint32_t page_count = DEFAULT_PAGE_COUNT;
	uint64_t clock = 0;
	uint64_t last_update_submission = 0;
	uint64_t admission_generation = 1;
	Statistics statistics;
	MicroGeometryPageShaderRD page_shader;
	RID page_shader_version;
	RID page_pipeline;

	static void _read_page(void *p_userdata);
	RID _create_buffer(Asset &r_asset, const void *p_data, uint64_t p_size);
	void _publish(Asset &r_asset);
	void _unpublish_page(Asset &r_asset, uint32_t p_page);
	uint32_t _allocate_slot();
	void _free_asset_buffers(Asset &r_asset);
	bool _build_page_clas(Asset &r_asset, uint32_t p_page, ReadTask &r_task);

public:
	RID feedback_create(uint32_t p_capacity = 4096);
	RID feedback_begin(RID p_feedback);
	bool feedback_needs_retry(RID p_feedback) const;
	void feedback_submit(RID p_feedback);
	void feedback_free(RID p_feedback);
	RID acquire(const Ref<MicroGeometryData> &p_source);
	void release(RID p_asset);
	void update_dependency(RID p_asset, DependencyTracker *p_tracker);
	void update();
	bool request_group(RID p_asset, uint32_t p_group);
	bool pin_page(const PagePin &p_pin);
	void unpin_page(const PagePin &p_pin);
	void lease_resident_pages(Vector<PagePin> &r_pages);
	RID get_page_clas(const PagePin &p_pin) const;
	bool pin_group(RID p_asset, uint32_t p_group);
	void unpin_group(RID p_asset, uint32_t p_group);
	bool is_group_ready(RID p_asset, uint32_t p_group, bool p_require_clas = false) const;
	bool is_ready(RID p_asset, bool p_require_clas = false) const;
	GPUAsset get_asset(RID p_asset) const;
	RID get_asset_buffer(RID p_asset) const;
	RID get_clas_addresses(RID p_asset) const;
	uint64_t get_primitive_lookup(RID p_asset, uint32_t p_surface) const;
	GPUPage get_page(RID p_asset, uint32_t p_page) const;
	Ref<MicroGeometryData> get_source(RID p_asset) const;
	RID get_pool() const { return pool; }
	void get_dependencies(RID p_asset, Vector<RID> &r_dependencies) const;
	Statistics get_statistics() const;
	uint64_t get_admission_generation() const { return admission_generation; }
	void add_raster_selection_memory(uint64_t p_bytes) { statistics.raster_selection_bytes += p_bytes; }
	void remove_raster_selection_memory(uint64_t p_bytes) { statistics.raster_selection_bytes -= p_bytes; }
	bool set_page_count(uint32_t p_count);
	~MicroGeometryStorage();
};

static_assert(sizeof(MicroGeometryStorage::GPUPage) == 16);
static_assert(sizeof(MicroGeometryStorage::GPUCluster) == 64);
static_assert(sizeof(MicroGeometryStorage::GPUGroup) == 48);
static_assert(sizeof(MicroGeometryStorage::GPUSurface) == 64);
static_assert(sizeof(MicroGeometryStorage::GPUNode) == 48);
static_assert(sizeof(MicroGeometryStorage::GPUAsset) == 120);

} // namespace RendererRD
