/**************************************************************************/
/*  micro_geometry_storage.cpp                                              */
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

#include "micro_geometry_storage.h"

#include "core/object/callable_mp.h"
#include "core/profiling/profiling.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

void MicroGeometryStorage::_read_page(void *p_userdata) {
	ReadTask *task = static_cast<ReadTask *>(p_userdata);
	task->error = task->source->read_page(task->page, task->decoded);
	if (task->error != OK) {
		return;
	}
	const auto &metadata = task->source->get_metadata();
	const auto &page = metadata.pages[task->page];
	task->build_input.max_acceleration_structure_count = page.cluster_count;
	task->build_input.flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
	task->build_input.vertex_format = RD::DATA_FORMAT_R16G16B16_SNORM;
	for (uint32_t cluster_id = page.first_cluster; cluster_id < page.first_cluster + page.cluster_count; cluster_id++) {
		const auto &cluster = metadata.clusters[cluster_id];
		const auto &surface = metadata.surfaces[cluster.surface];
		const MicroGeometryData::ClusterLayout layout = MicroGeometryData::compute_cluster_layout(cluster.payload_offset, cluster.vertex_count, cluster.triangle_count, surface.vertex_stride);
		if (cluster.vertex_count > task->max_vertices_per_cluster || cluster.triangle_count > task->max_triangles_per_cluster ||
				layout.vertices != cluster.payload_offset || layout.end > PAGE_SIZE || uint64_t(cluster.disk_offset) >= uint64_t(task->decoded.size())) {
			task->error = ERR_INVALID_DATA;
			return;
		}
		TriangleInfo info;
		info.cluster_id = cluster_id;
		info.packed_counts = cluster.triangle_count | (cluster.vertex_count << 9) | (1u << 24);
		info.vertex_stride = surface.vertex_stride;
		info.vertices = layout.vertices;
		info.indices = layout.indices;
		task->triangle_infos.push_back(info);
		ClusterTranscodeInfo transcode;
		transcode.vertices = layout.vertices;
		transcode.indices = layout.indices;
		transcode.vertex_ids = layout.vertex_ids;
		transcode.disk_offset = cluster.disk_offset;
		task->transcode_infos.push_back(transcode);
		task->build_input.max_cluster_triangle_count = MAX(task->build_input.max_cluster_triangle_count, cluster.triangle_count);
		task->build_input.max_cluster_vertex_count = MAX(task->build_input.max_cluster_vertex_count, cluster.vertex_count);
		task->build_input.max_total_triangle_count += cluster.triangle_count;
		task->build_input.max_total_vertex_count += cluster.vertex_count;
	}
}

RID MicroGeometryStorage::_create_buffer(Asset &r_asset, const void *p_data, uint64_t p_size) {
	if (p_size == 0) {
		return RID();
	}
	ERR_FAIL_COND_V_MSG(p_size > UINT32_MAX, RID(), "Microgeometry metadata buffer exceeds the RenderingDevice size limit.");
	Span<uint8_t> bytes(static_cast<const uint8_t *>(p_data), uint32_t(p_size));
	RID buffer = RD::get_singleton()->storage_buffer_create(uint32_t(p_size), bytes, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (buffer.is_valid()) {
		r_asset.buffers.push_back(buffer);
		r_asset.metadata_bytes += p_size;
	}
	return buffer;
}

void MicroGeometryStorage::_free_asset_buffers(Asset &r_asset) {
	for (Page &page : r_asset.pages) {
		for (RID buffer : page.clas_resources) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	for (RID buffer : r_asset.rt_resources) {
		RD::get_singleton()->free_rid(buffer);
	}
	for (RID buffer : r_asset.buffers) {
		RD::get_singleton()->free_rid(buffer);
	}
	r_asset.buffers.clear();
	r_asset.rt_resources.clear();
}

bool MicroGeometryStorage::_build_page_clas(Asset &r_asset, uint32_t p_page, uint32_t p_staging_slot, ReadTask &r_task) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_COND_V(!rd->clas_is_supported(), false);
	if (page_pipeline.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		page_shader.initialize(modes);
		page_shader_version = page_shader.version_create();
		page_pipeline = rd->compute_pipeline_create(page_shader.version_get_shader(page_shader_version, 0));
	}
	ERR_FAIL_COND_V(page_pipeline.is_null(), false);
	const MicroGeometryData::Build &metadata = r_asset.source->get_metadata();
	if (r_asset.clas_addresses.is_null()) {
		auto allocate = [&](uint32_t p_size) {
			RID buffer = rd->storage_buffer_create(MAX(p_size, 16u), Vector<uint8_t>(), 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
			if (buffer.is_valid()) {
				r_asset.rt_resources.push_back(buffer);
				r_asset.rt_bytes += MAX(p_size, 16u);
				statistics.acceleration_structure_bytes += MAX(p_size, 16u);
			}
			return buffer;
		};
		r_asset.clas_addresses = allocate(metadata.clusters.size() * 8);
		r_asset.primitive_lookup = allocate(r_asset.empty_primitive_lookup.size());
		r_asset.primitive_offset_buffer = allocate(r_asset.primitive_offsets.size() * 4);
		ERR_FAIL_COND_V(r_asset.clas_addresses.is_null() || r_asset.primitive_lookup.is_null() || r_asset.primitive_offset_buffer.is_null(), false);
		rd->buffer_clear(r_asset.clas_addresses, 0, MAX(metadata.clusters.size() * 8, 16));
		rd->buffer_update(r_asset.primitive_lookup, 0, r_asset.empty_primitive_lookup.size(), r_asset.empty_primitive_lookup.ptr());
		r_asset.empty_primitive_lookup.clear();
		rd->buffer_update(r_asset.primitive_offset_buffer, 0, r_asset.primitive_offsets.size() * 4, r_asset.primitive_offsets.ptr());
	}
	ERR_FAIL_COND_V(r_asset.clas_addresses.is_null() || r_asset.primitive_lookup.is_null() || r_asset.primitive_offset_buffer.is_null(), false);
	const auto &source_page = metadata.pages[p_page];
	Page &page = r_asset.pages[p_page];
	const uint64_t page_address = rd->buffer_get_device_address(pool) + uint64_t(page.gpu.slot) * PAGE_SIZE;
	LocalVector<TriangleInfo> &infos = r_task.triangle_infos;
	RD::ClusterBuildInput &input = r_task.build_input;
	for (TriangleInfo &info : infos) {
		info.vertices += page_address;
		info.indices += page_address;
	}
	RD::ClusterBuildSizes sizes;
	rd->clas_get_build_sizes(input, sizes);
	ERR_FAIL_COND_V(sizes.acceleration_structure_size == 0 || sizes.acceleration_structure_size > UINT32_MAX || sizes.build_scratch_size == 0 || sizes.build_scratch_size > UINT32_MAX, false);
	auto allocate = [&](uint32_t p_size, BitField<RD::BufferCreationBits> p_flags, const void *p_data = nullptr) {
		Span<uint8_t> bytes;
		if (p_data) {
			bytes = Span<uint8_t>((const uint8_t *)p_data, p_size);
		}
		RID buffer = rd->storage_buffer_create(p_size, bytes, 0, p_flags | RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
		if (buffer.is_valid()) {
			page.clas_resources.push_back(buffer);
			page.clas_bytes += p_size;
			statistics.acceleration_structure_bytes += p_size;
		}
		return buffer;
	};
	page.clas_storage = allocate(sizes.acceleration_structure_size, RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_STORAGE_BIT);
	page.clas_storage_bytes = sizes.acceleration_structure_size;
	RID scratch = allocate(sizes.build_scratch_size, {});
	RID info_buffer = allocate(infos.size() * sizeof(TriangleInfo), RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT, infos.ptr());
	RID count_buffer = allocate(4, RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT, &source_page.cluster_count);
	RID transcode_buffer = allocate(r_task.transcode_infos.size() * sizeof(ClusterTranscodeInfo), {}, r_task.transcode_infos.ptr());
	ERR_FAIL_COND_V(page.clas_storage.is_null() || scratch.is_null() || info_buffer.is_null() || count_buffer.is_null() || transcode_buffer.is_null(), false);
	struct Parameters {
		uint64_t asset;
		uint64_t offsets;
		uint32_t first_cluster;
		uint32_t cluster_count;
		uint32_t pool_slot;
		uint32_t staging_slot;
		float position_step;
		uint32_t pad;
	} parameters = { rd->buffer_get_device_address(r_asset.descriptor_buffer), rd->buffer_get_device_address(r_asset.primitive_offset_buffer), source_page.first_cluster, source_page.cluster_count, page.gpu.slot, p_staging_slot, metadata.position_step, 0 };
	RID page_shader_rid = page_shader.version_get_shader(page_shader_version, 0);
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(page_shader_rid, 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, r_asset.primitive_lookup),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, staging),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, pool),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, transcode_buffer),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, transcode_faults));
	ERR_FAIL_COND_V(uniform_set.is_null(), false);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, page_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, &parameters, sizeof(parameters));
	for (RID buffer : r_asset.buffers) {
		rd->compute_list_add_buffer_dependency(list, buffer);
	}
	for (RID buffer : { pool, r_asset.primitive_lookup, r_asset.primitive_offset_buffer }) {
		rd->compute_list_add_buffer_dependency(list, buffer);
	}
	rd->compute_list_dispatch(list, source_page.cluster_count, 1, 1);
	rd->compute_list_end();
	transcode_faults_dirty = true;
	RD::ClusterAddressRegion addresses = { r_asset.clas_addresses, uint64_t(source_page.first_cluster) * 8, 8, uint64_t(source_page.cluster_count) * 8 };
	RD::ClusterAddressRegion source = { info_buffer, 0, sizeof(TriangleInfo), uint64_t(infos.size()) * sizeof(TriangleInfo) };
	RID geometry_dependency = pool;
	if (rd->clas_build(input, page.clas_storage, addresses, {}, scratch, source, count_buffer, { &geometry_dependency, 1 }) != OK) {
		return false;
	}
	statistics.clas_builds += source_page.cluster_count;
	statistics.clas_page_builds++;
	page.clas_submission = rd->get_pending_submission_serial();
	return true;
}

RID MicroGeometryStorage::acquire(const Ref<MicroGeometryData> &p_source) {
	ERR_FAIL_COND_V(p_source.is_null(), RID());
	if (const RID *existing = sources.getptr(p_source.ptr())) {
		assets.get_or_null(*existing)->references++;
		return *existing;
	}
	const MicroGeometryData::Build &metadata = p_source->get_metadata();
	const uint64_t required = sizeof(GPUAsset) + uint64_t(metadata.clusters.size()) * sizeof(GPUCluster) + uint64_t(metadata.groups.size()) * (sizeof(GPUGroup) + sizeof(uint32_t)) +
			uint64_t(metadata.surfaces.size()) * sizeof(GPUSurface) + uint64_t(metadata.nodes.size()) * sizeof(GPUNode) +
			(uint64_t(metadata.terminals.size()) + metadata.roots.size() + metadata.parent_groups.size()) * sizeof(uint32_t) + uint64_t(metadata.pages.size()) * sizeof(GPUPage);
	if (pool.is_null()) {
		pool = RD::get_singleton()->storage_buffer_create(page_count * PAGE_SIZE, Vector<uint8_t>(), 0,
				RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		ERR_FAIL_COND_V(pool.is_null(), RID());
		staging = RD::get_singleton()->storage_buffer_create(STAGING_SIZE);
		transcode_faults = RD::get_singleton()->storage_buffer_create(16);
		if (staging.is_null() || transcode_faults.is_null()) {
			if (staging.is_valid()) {
				RD::get_singleton()->free_rid(staging);
				staging = RID();
			}
			if (transcode_faults.is_valid()) {
				RD::get_singleton()->free_rid(transcode_faults);
				transcode_faults = RID();
			}
			RD::get_singleton()->free_rid(pool);
			pool = RID();
			ERR_FAIL_V_MSG(RID(), "Unable to create the microgeometry page staging buffers.");
		}
		RD::get_singleton()->buffer_clear(transcode_faults, 0, 16);
		staging_submissions.resize_initialized(MAX_IO_TASKS);
		slots.resize(page_count);
	}
	RID id = assets.make_rid();
	Asset &asset = *assets.get_or_null(id);
	asset.source = p_source;
	asset.gpu.identity = id.get_id();
	asset.gpu.residency_generation = 1;
	LocalVector<GPUPage> pages;
	LocalVector<GPUCluster> clusters;
	LocalVector<GPUGroup> groups;
	LocalVector<GPUSurface> surfaces;
	LocalVector<GPUNode> nodes;
	bool payload_valid = true;
	auto prepare = [&](uint32_t) {
		asset.pages.resize(metadata.pages.size());
		asset.group_pages.resize(metadata.groups.size());
		asset.group_pins.resize_initialized(metadata.groups.size());
		asset.group_states.resize_initialized(metadata.groups.size());
		pages.resize(metadata.pages.size());
		for (const MicroGeometryData::Cluster &source : metadata.clusters) {
			GPUCluster cluster = {};
			cluster.surface = source.surface;
			cluster.group = source.group;
			cluster.refined_group = source.refined_group;
			cluster.page = source.page;
			cluster.payload_offset = source.payload_offset;
			cluster.vertex_count = source.vertex_count;
			cluster.triangle_count = source.triangle_count;
			cluster.first_primitive = source.first_primitive;
			memcpy(cluster.center, source.bounds.center, sizeof(cluster.center));
			cluster.radius = source.bounds.radius;
			cluster.error = source.bounds.error;
			clusters.push_back(cluster);
		}
		for (const MicroGeometryData::Group &source : metadata.groups) {
			GPUGroup group = {};
			group.first_cluster = source.first_cluster;
			group.cluster_count = source.cluster_count;
			group.depth = source.depth;
			group.first_parent = source.first_parent;
			group.parent_count = source.parent_count;
			memcpy(group.center, source.bounds.center, sizeof(group.center));
			group.radius = source.bounds.radius;
			group.error = source.bounds.error;
			Vector<uint32_t> &group_pages = asset.group_pages[groups.size()];
			for (uint32_t i = source.first_cluster; i < source.first_cluster + source.cluster_count; i++) {
				const uint32_t page = metadata.clusters[i].page;
				if (!group_pages.has(page)) {
					group_pages.push_back(page);
				}
			}
			groups.push_back(group);
		}
		for (const MicroGeometryData::Surface &source : metadata.surfaces) {
			GPUSurface surface = {};
			surface.format = source.format;
			surface.source_surface = source.source_surface;
			surface.vertex_stride = source.vertex_stride;
			memcpy(surface.attribute_offsets, source.attribute_offsets, sizeof(surface.attribute_offsets));
			memcpy(surface.attribute_formats, source.attribute_formats, sizeof(surface.attribute_formats));
			surface.source_vertex_count = source.source_vertex_count;
			surface.source_triangle_count = source.source_triangle_count;
			memcpy(surface.frame_center, source.frame_center, sizeof(surface.frame_center));
			surface.frame_scale = source.frame_scale;
			memcpy(surface.uv_mode, source.uv_mode, sizeof(surface.uv_mode));
			memcpy(surface.uv_min, source.uv_min, sizeof(surface.uv_min));
			memcpy(surface.uv_scale, source.uv_scale, sizeof(surface.uv_scale));
			surfaces.push_back(surface);
		}
		for (const MicroGeometryData::Node &source : metadata.nodes) {
			GPUNode node = {};
			node.group = source.group;
			node.first_child = source.first_child;
			node.child_count = source.child_count;
			memcpy(node.center, source.bounds.center, sizeof(node.center));
			node.radius = source.bounds.radius;
			node.error = source.bounds.error;
			nodes.push_back(node);
		}

		uint64_t primitives = 0;
		for (const auto &surface : metadata.surfaces) {
			asset.primitive_offsets.push_back(primitives);
			primitives += surface.source_triangle_count;
		}
		if (primitives * 8 > UINT32_MAX) {
			payload_valid = false;
			return;
		}
		asset.empty_primitive_lookup.resize(MAX(uint32_t(primitives * 8), 16u));
		memset(asset.empty_primitive_lookup.ptrw(), 0xff, asset.empty_primitive_lookup.size());
	};
	WorkerThreadPool *workers = WorkerThreadPool::get_singleton();
	auto job = workers->add_native_group_task([](void *p_data, uint32_t p_index) {
		GodotProfileZone("MicrogeometryAssetPayload");
		(*static_cast<decltype(prepare) *>(p_data))(p_index);
	},
			&prepare, 1, 1, true, SNAME("MicrogeometryAssetPayload"));
	workers->wait_for_group_task_completion(job);
	if (!payload_valid) {
		assets.free(id);
		return RID();
	}
	asset.page_buffer = _create_buffer(asset, pages.ptr(), uint64_t(pages.size()) * sizeof(GPUPage));
	asset.group_buffer = _create_buffer(asset, asset.group_states.ptr(), uint64_t(asset.group_states.size()) * sizeof(uint32_t));
	asset.gpu.pages = asset.page_buffer.is_valid() ? RD::get_singleton()->buffer_get_device_address(asset.page_buffer) : 0;
	asset.gpu.group_states = asset.group_buffer.is_valid() ? RD::get_singleton()->buffer_get_device_address(asset.group_buffer) : 0;
	auto create_address = [&](const void *p_data, uint64_t p_size) -> uint64_t {
		RID buffer = _create_buffer(asset, p_data, p_size);
		return buffer.is_valid() ? RD::get_singleton()->buffer_get_device_address(buffer) : 0;
	};
	asset.gpu.clusters = create_address(clusters.ptr(), uint64_t(clusters.size()) * sizeof(GPUCluster));
	asset.gpu.groups = create_address(groups.ptr(), uint64_t(groups.size()) * sizeof(GPUGroup));
	asset.gpu.surfaces = create_address(surfaces.ptr(), uint64_t(surfaces.size()) * sizeof(GPUSurface));
	asset.gpu.nodes = create_address(nodes.ptr(), uint64_t(nodes.size()) * sizeof(GPUNode));
	asset.gpu.terminals = create_address(metadata.terminals.ptr(), uint64_t(metadata.terminals.size()) * sizeof(uint32_t));
	asset.gpu.roots = create_address(metadata.roots.ptr(), uint64_t(metadata.roots.size()) * sizeof(uint32_t));
	asset.gpu.parent_groups = create_address(metadata.parent_groups.ptr(), uint64_t(metadata.parent_groups.size()) * sizeof(uint32_t));
	asset.gpu.cluster_count = clusters.size();
	asset.gpu.group_count = groups.size();
	asset.gpu.surface_count = surfaces.size();
	asset.gpu.node_count = nodes.size();
	asset.gpu.terminal_count = metadata.terminals.size();
	asset.gpu.root_count = metadata.roots.size();
	asset.gpu.page_count = pages.size();
	asset.descriptor_buffer = _create_buffer(asset, &asset.gpu, sizeof(GPUAsset));
	if (asset.metadata_bytes != required) {
		_free_asset_buffers(asset);
		assets.free(id);
		return RID();
	}
	statistics.metadata_bytes += required;
	sources.insert(p_source.ptr(), id);
	admission_generation++;
	active_assets.push_back(id);
	for (uint32_t group : metadata.terminals) {
		for (uint32_t page : asset.group_pages[group]) {
			asset.pages[page].terminal = true;
		}
		request_group(id, group);
	}
	print_verbose(vformat("Microgeometry metadata admitted: asset=%d bytes, live=%d bytes, retired=%d bytes.", asset.metadata_bytes, statistics.metadata_bytes, statistics.retired_metadata_bytes));
	return id;
}

void MicroGeometryStorage::_publish(Asset &r_asset) {
	if (!r_asset.publication_pending) {
		return;
	}
	r_asset.publication_pending = false;
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	bool changed = r_asset.groups_dirty;
	for (uint32_t i = 0; i < r_asset.pages.size(); i++) {
		Page &page = r_asset.pages[i];
		uint32_t ready = page.gpu.ready;
		r_asset.publication_pending |= (page.status == UPLOADING && page.upload_submission > completed) || (page.clas_submission > completed);
		if (page.status == UPLOADING && page.upload_submission <= completed) {
			page.status = RESIDENT;
			page.gpu.ready = 1;
		}
		if (page.status == RESIDENT && page.clas_submission != 0 && page.clas_submission <= completed) {
			if (!(page.gpu.ready & 2)) {
				for (RID resource : page.clas_resources) {
					if (resource != page.clas_storage) {
						RD::get_singleton()->free_rid(resource);
					}
				}
				page.clas_resources.clear();
				page.clas_resources.push_back(page.clas_storage);
				statistics.acceleration_structure_bytes -= page.clas_bytes - page.clas_storage_bytes;
				page.clas_bytes = page.clas_storage_bytes;
			}
			page.gpu.ready |= 2;
		}
		if (ready != page.gpu.ready) {
			RD::get_singleton()->buffer_update(r_asset.page_buffer, i * sizeof(GPUPage), sizeof(GPUPage), &page.gpu);
			changed = true;
		}
	}
	if (!changed && !r_asset.groups_dirty) {
		return;
	}
	r_asset.groups_dirty = false;
	for (uint32_t i = 0; i < r_asset.group_pages.size(); i++) {
		uint32_t ready = 3;
		for (uint32_t page : r_asset.group_pages[i]) {
			ready &= r_asset.pages[page].gpu.ready;
		}
		if (ready != r_asset.group_states[i]) {
			r_asset.group_states[i] = ready;
			RD::get_singleton()->buffer_update(r_asset.group_buffer, i * sizeof(uint32_t), sizeof(uint32_t), &ready);
			changed = true;
		}
	}
	const uint32_t previous_ready = r_asset.gpu.ready;
	r_asset.gpu.ready = 3;
	for (uint32_t group : r_asset.source->get_metadata().terminals) {
		r_asset.gpu.ready &= r_asset.group_states[group];
	}
	if (changed) {
		r_asset.gpu.residency_generation++;
		RD::get_singleton()->buffer_update(r_asset.descriptor_buffer, 0, sizeof(GPUAsset), &r_asset.gpu);
	}
	if (previous_ready != r_asset.gpu.ready) {
		r_asset.dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
	}
}

void MicroGeometryStorage::_unpublish_page(Asset &r_asset, uint32_t p_page) {
	Page &page = r_asset.pages[p_page];
	if (!page.clas_resources.is_empty()) {
		RetiredMetadata retired;
		retired.buffers = page.clas_resources;
		retired.rt_bytes = page.clas_bytes;
		retired.submission = RD::get_singleton()->get_pending_submission_serial();
		retired_metadata.push_back(retired);
		page.clas_resources.clear();
		page.clas_storage = RID();
		page.clas_bytes = 0;
	}
	if (page.gpu.slot != INVALID_SLOT) {
		Slot &slot = slots[page.gpu.slot];
		slot.asset = RID();
		slot.retirement = RD::get_singleton()->get_pending_submission_serial();
	}
	page.gpu.slot = INVALID_SLOT;
	page.gpu.ready = 0;
	page.status = ABSENT;
	page.clas_submission = 0;
	RD::get_singleton()->buffer_update(r_asset.page_buffer, p_page * sizeof(GPUPage), sizeof(GPUPage), &page.gpu);
	r_asset.gpu.residency_generation++;
	r_asset.publication_pending = true;
	r_asset.groups_dirty = true;
	_publish(r_asset);
}

void MicroGeometryStorage::release(RID p_asset) {
	Asset *asset = assets.get_or_null(p_asset);
	if (!asset || --asset->references != 0) {
		return;
	}
	asset->gpu.identity = 0;
	asset->gpu.ready = 0;
	RD::get_singleton()->buffer_update(asset->descriptor_buffer, 0, sizeof(GPUAsset), &asset->gpu);
	for (const Page &page : asset->pages) {
		if (page.gpu.slot != INVALID_SLOT) {
			Slot &slot = slots[page.gpu.slot];
			slot.asset = RID();
			slot.retirement = RD::get_singleton()->get_pending_submission_serial();
		}
	}
	RetiredMetadata retired;
	retired.buffers = asset->buffers;
	for (RID buffer : asset->rt_resources) {
		retired.buffers.push_back(buffer);
	}
	retired.rt_bytes = asset->rt_bytes;
	for (Page &page : asset->pages) {
		for (RID buffer : page.clas_resources) {
			retired.buffers.push_back(buffer);
		}
		retired.rt_bytes += page.clas_bytes;
	}
	retired.bytes = asset->metadata_bytes;
	retired.submission = RD::get_singleton()->get_pending_submission_serial();
	retired_metadata.push_back(retired);
	statistics.metadata_bytes -= retired.bytes;
	statistics.retired_metadata_bytes += retired.bytes;
	sources.erase(asset->source.ptr());
	active_assets.erase(p_asset);
	asset->dependency.deleted_notify(p_asset);
	assets.free(p_asset);
}

void MicroGeometryStorage::update_dependency(RID p_asset, DependencyTracker *p_tracker) {
	Asset *asset = assets.get_or_null(p_asset);
	if (asset) {
		p_tracker->update_dependency(&asset->dependency);
	}
}

bool MicroGeometryStorage::request_group(RID p_asset, uint32_t p_group) {
	Asset *asset = assets.get_or_null(p_asset);
	ERR_FAIL_NULL_V(asset, false);
	ERR_FAIL_UNSIGNED_INDEX_V(p_group, asset->group_pages.size(), false);
	bool valid = true;
	for (uint32_t index : asset->group_pages[p_group]) {
		Page &page = asset->pages[index];
		page.requested = clock;
		if (page.status == ABSENT) {
			page.status = REQUESTED;
			asset->has_requests = true;
		}
		valid &= page.status != FAILED;
	}
	return valid;
}

bool MicroGeometryStorage::pin_page(const PagePin &p_pin) {
	Asset *asset = assets.get_or_null(p_pin.asset);
	if (!asset || p_pin.page >= asset->pages.size()) {
		return false;
	}
	Page &page = asset->pages[p_pin.page];
	if (page.status != RESIDENT || page.gpu.generation != p_pin.generation || (page.gpu.ready & 3) != 3) {
		return false;
	}
	page.pins++;
	return true;
}

void MicroGeometryStorage::unpin_page(const PagePin &p_pin) {
	Asset *asset = assets.get_or_null(p_pin.asset);
	if (!asset || p_pin.page >= asset->pages.size()) {
		return;
	}
	Page &page = asset->pages[p_pin.page];
	ERR_FAIL_COND(page.gpu.generation != p_pin.generation || page.pins == 0);
	page.pins--;
}

void MicroGeometryStorage::lease_resident_pages(Vector<PagePin> &r_pages) {
	ERR_FAIL_COND(!r_pages.is_empty());
	for (const Slot &slot : slots) {
		Asset *asset = assets.get_or_null(slot.asset);
		if (!asset) {
			continue;
		}
		Page &page = asset->pages[slot.page];
		if (page.status == RESIDENT && (page.gpu.ready & 3) == 3) {
			page.pins++;
			r_pages.push_back({ slot.asset, slot.page, page.gpu.generation });
		}
	}
}

RID MicroGeometryStorage::get_page_clas(const PagePin &p_pin) const {
	const Asset *asset = assets.get_or_null(p_pin.asset);
	if (!asset || p_pin.page >= asset->pages.size()) {
		return RID();
	}
	const Page &page = asset->pages[p_pin.page];
	return page.status == RESIDENT && page.gpu.generation == p_pin.generation && (page.gpu.ready & 3) == 3 ? page.clas_storage : RID();
}

bool MicroGeometryStorage::pin_group(RID p_asset, uint32_t p_group) {
	Asset *asset = assets.get_or_null(p_asset);
	ERR_FAIL_NULL_V(asset, false);
	ERR_FAIL_UNSIGNED_INDEX_V(p_group, asset->group_pages.size(), false);
	asset->group_pins[p_group]++;
	for (uint32_t page : asset->group_pages[p_group]) {
		asset->pages[page].pins++;
	}
	return request_group(p_asset, p_group);
}

void MicroGeometryStorage::unpin_group(RID p_asset, uint32_t p_group) {
	Asset *asset = assets.get_or_null(p_asset);
	if (!asset) {
		return;
	}
	ERR_FAIL_UNSIGNED_INDEX(p_group, asset->group_pages.size());
	ERR_FAIL_COND(asset->group_pins[p_group] == 0);
	asset->group_pins[p_group]--;
	for (uint32_t page : asset->group_pages[p_group]) {
		asset->pages[page].pins--;
	}
}

uint32_t MicroGeometryStorage::_allocate_staging_slot() {
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	for (uint32_t i = 0; i < staging_submissions.size(); i++) {
		if (staging_submissions[i] <= completed) {
			return i;
		}
	}
	return INVALID_SLOT;
}

void MicroGeometryStorage::_faults_dispatch(const Vector<uint8_t> &p_bytes, uint64_t p_storage) {
	reinterpret_cast<MicroGeometryStorage *>(uintptr_t(p_storage))->_faults_received(p_bytes);
}

void MicroGeometryStorage::_faults_received(const Vector<uint8_t> &p_bytes) {
	transcode_faults_pending = false;
	ERR_FAIL_COND(p_bytes.size() < 4);
	uint32_t count = 0;
	memcpy(&count, p_bytes.ptr(), sizeof(count));
	statistics.transcode_faults = count;
}

uint32_t MicroGeometryStorage::_allocate_slot() {
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	for (uint32_t i = 0; i < slots.size(); i++) {
		Slot &slot = slots[i];
		if (slot.asset.is_null() && slot.retirement <= completed) {
			slot.retirement = 0;
			return i;
		}
	}
	uint64_t oldest = UINT64_MAX;
	uint32_t selected = INVALID_SLOT;
	for (uint32_t i = 0; i < slots.size(); i++) {
		Slot &slot = slots[i];
		Asset *asset = assets.get_or_null(slot.asset);
		if (!asset) {
			continue;
		}
		const Page &page = asset->pages[slot.page];
		if (page.status == RESIDENT && page.pins == 0 && !page.terminal && page.requested + 2 < clock && page.requested < oldest) {
			oldest = page.requested;
			selected = i;
		}
	}
	if (selected != INVALID_SLOT) {
		Slot &slot = slots[selected];
		_unpublish_page(*assets.get_or_null(slot.asset), slot.page);
	}
	statistics.pressure++;
	return INVALID_SLOT;
}

void MicroGeometryStorage::update() {
	const uint64_t submission = RD::get_singleton()->get_pending_submission_serial();
	if (last_update_submission == submission) {
		return;
	}
	last_update_submission = submission;
	RENDER_TIMESTAMP("Microgeometry Streaming Retire");
	clock++;
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	for (uint32_t i = 0; i < retired_metadata.size();) {
		RetiredMetadata &retired = retired_metadata[i];
		if (retired.submission > completed) {
			i++;
			continue;
		}
		for (RID buffer : retired.buffers) {
			RD::get_singleton()->free_rid(buffer);
		}
		statistics.retired_metadata_bytes -= retired.bytes;
		if (retired.bytes != 0) {
			admission_generation++;
		}
		statistics.acceleration_structure_bytes -= retired.rt_bytes;
		retired_metadata.remove_at_unordered(i);
	}
	RENDER_TIMESTAMP("Microgeometry Streaming Upload and CLAS");
	for (uint32_t i = 0; i < tasks.size();) {
		ReadTask *task = tasks[i];
		if (!WorkerThreadPool::get_singleton()->is_task_completed(task->task)) {
			i++;
			continue;
		}
		Asset *asset = assets.get_or_null(task->asset);
		if (asset && task->error == OK) {
			const uint32_t staging_index = _allocate_staging_slot();
			if (staging_index == INVALID_SLOT) {
				i++;
				continue;
			}
			uint32_t slot_index = _allocate_slot();
			if (slot_index == INVALID_SLOT) {
				i++;
				continue;
			}
			Slot &slot = slots[slot_index];
			Page &page = asset->pages[task->page];
			slot.asset = task->asset;
			slot.page = task->page;
			const Error error = RD::get_singleton()->buffer_update(staging, staging_index * PAGE_SIZE, task->decoded.size(), task->decoded.ptr());
			page.gpu.slot = slot_index;
			page.gpu.generation++;
			page.upload_submission = RD::get_singleton()->get_pending_submission_serial();
			page.status = UPLOADING;
			asset->publication_pending = true;
			staging_submissions[staging_index] = page.upload_submission;
			if (error != OK || !_build_page_clas(*asset, task->page, staging_index, *task)) {
				_unpublish_page(*asset, task->page);
				page.status = FAILED;
			}
		} else if (asset) {
			asset->pages[task->page].status = FAILED;
			statistics.failed_reads++;
			ERR_PRINT(vformat("Failed to read microgeometry page %d from %s (error %d).", task->page, task->source->get_source_path(), task->error));
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task->task);
		memdelete(task);
		tasks.remove_at_unordered(i);
	}
	if (transcode_faults_dirty && !transcode_faults_pending && transcode_faults.is_valid()) {
		transcode_faults_dirty = false;
		transcode_faults_pending = true;
		if (RD::get_singleton()->buffer_get_data_async(transcode_faults, callable_mp_static(&MicroGeometryStorage::_faults_dispatch).bind(uint64_t(uintptr_t(this)))) != OK) {
			transcode_faults_dirty = true;
			transcode_faults_pending = false;
		}
	}
	RENDER_TIMESTAMP("Microgeometry Streaming Publish");
	for (RID id : active_assets) {
		_publish(*assets.get_or_null(id));
	}
	RENDER_TIMESTAMP("Microgeometry Streaming Schedule");
	const auto limits = RD::get_singleton()->clas_get_limits();
	for (uint32_t priority = 0; priority < 2 && tasks.size() < MAX_IO_TASKS; priority++) {
		for (RID id : active_assets) {
			Asset &asset = *assets.get_or_null(id);
			if (!asset.has_requests) {
				continue;
			}
			bool remaining = false;
			for (uint32_t i = 0; i < asset.pages.size(); i++) {
				Page &page = asset.pages[i];
				if (page.status != REQUESTED || tasks.size() >= MAX_IO_TASKS || (priority == 0 && page.pins == 0 && !page.terminal) || (priority == 1 && (page.pins != 0 || page.terminal))) {
					remaining |= page.status == REQUESTED;
					continue;
				}
				ReadTask *task = memnew(ReadTask);
				task->source = asset.source;
				task->asset = id;
				task->page = i;
				task->max_vertices_per_cluster = limits.max_vertices_per_cluster;
				task->max_triangles_per_cluster = limits.max_triangles_per_cluster;
				page.status = READING;
				task->task = WorkerThreadPool::get_singleton()->add_native_task(_read_page, task, false, "Microgeometry page");
				tasks.push_back(task);
			}
			asset.has_requests = remaining;
		}
	}
	RENDER_TIMESTAMP("Microgeometry Streaming Complete");
}

bool MicroGeometryStorage::is_group_ready(RID p_asset, uint32_t p_group, bool p_require_clas) const {
	const Asset *asset = assets.get_or_null(p_asset);
	const uint32_t required = p_require_clas ? 3 : 1;
	return asset && p_group < asset->group_states.size() && (asset->group_states[p_group] & required) == required;
}

bool MicroGeometryStorage::is_ready(RID p_asset, bool p_require_clas) const {
	const Asset *asset = assets.get_or_null(p_asset);
	const uint32_t required = p_require_clas ? 3 : 1;
	return asset && (asset->gpu.ready & required) == required;
}

MicroGeometryStorage::GPUAsset MicroGeometryStorage::get_asset(RID p_asset) const {
	const Asset *asset = assets.get_or_null(p_asset);
	return asset ? asset->gpu : GPUAsset();
}

RID MicroGeometryStorage::get_asset_buffer(RID p_asset) const {
	const Asset *asset = assets.get_or_null(p_asset);
	return asset ? asset->descriptor_buffer : RID();
}

RID MicroGeometryStorage::get_clas_addresses(RID p_asset) const {
	const Asset *asset = assets.get_or_null(p_asset);
	return asset ? asset->clas_addresses : RID();
}

uint64_t MicroGeometryStorage::get_primitive_lookup(RID p_asset, uint32_t p_surface) const {
	const Asset *asset = assets.get_or_null(p_asset);
	return asset && asset->primitive_lookup.is_valid() && p_surface < uint32_t(asset->primitive_offsets.size()) ? RD::get_singleton()->buffer_get_device_address(asset->primitive_lookup) + uint64_t(asset->primitive_offsets[p_surface]) * 8 : 0;
}

MicroGeometryStorage::GPUPage MicroGeometryStorage::get_page(RID p_asset, uint32_t p_page) const {
	const Asset *asset = assets.get_or_null(p_asset);
	return asset && p_page < asset->pages.size() ? asset->pages[p_page].gpu : GPUPage();
}

Ref<MicroGeometryData> MicroGeometryStorage::get_source(RID p_asset) const {
	const Asset *asset = assets.get_or_null(p_asset);
	return asset ? asset->source : Ref<MicroGeometryData>();
}

void MicroGeometryStorage::get_dependencies(RID p_asset, Vector<RID> &r_dependencies) const {
	const Asset *asset = assets.get_or_null(p_asset);
	if (!asset) {
		return;
	}
	r_dependencies.push_back(pool);
	for (RID buffer : asset->buffers) {
		r_dependencies.push_back(buffer);
	}
	for (RID buffer : asset->rt_resources) {
		r_dependencies.push_back(buffer);
	}
}

MicroGeometryStorage::Statistics MicroGeometryStorage::get_statistics() const {
	Statistics result = statistics;
	result.pool_bytes = pool.is_valid() ? uint64_t(page_count) * PAGE_SIZE + STAGING_SIZE : 0;
	result.device_buffer_bytes = RD::get_singleton()->get_memory_usage(RD::MEMORY_BUFFERS);
	for (const ReadTask *task : tasks) {
		const MicroGeometryData::Page &page = task->source->get_metadata().pages[task->page];
		result.io_bytes += page.encoded_size + page.decoded_size;
	}
	for (RID id : active_feedbacks) {
		result.feedback_bytes += sizeof(FeedbackHeader) + uint64_t(feedbacks.get_or_null(id)->capacity) * sizeof(GPURequest);
	}
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	for (const Slot &slot : slots) {
		result.retiring_pages += slot.asset.is_null() && slot.retirement > completed;
	}
	for (RID id : active_assets) {
		const Asset *asset = assets.get_or_null(id);
		for (uint32_t index = 0; index < asset->pages.size(); index++) {
			const Page &page = asset->pages[index];
			if (page.status == RESIDENT && (page.gpu.ready & 3) == 3) {
				result.resident_clas += asset->source->get_metadata().pages[index].cluster_count;
			}
			result.resident_pages += page.status == RESIDENT;
			result.pending_pages += page.status == REQUESTED || page.status == READING || page.status == UPLOADING;
		}
	}
	return result;
}

bool MicroGeometryStorage::set_page_count(uint32_t p_count) {
	ERR_FAIL_COND_V(pool.is_valid(), false);
	ERR_FAIL_COND_V(p_count == 0 || uint64_t(p_count) * PAGE_SIZE > UINT32_MAX, false);
	page_count = p_count;
	return true;
}

MicroGeometryStorage::~MicroGeometryStorage() {
	if (page_shader_version.is_valid()) {
		page_shader.version_free(page_shader_version);
	}
	for (ReadTask *task : tasks) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task->task);
		memdelete(task);
	}
	if (pool.is_valid() || !active_feedbacks.is_empty() || pending_feedback_count != 0) {
		RD::get_singleton()->flush_and_stall();
	}
	for (RID id : active_feedbacks) {
		RD::get_singleton()->free_rid(feedbacks.get_or_null(id)->buffer);
		feedbacks.free(id);
	}
	for (RID id : active_assets) {
		_free_asset_buffers(*assets.get_or_null(id));
		assets.free(id);
	}
	for (RetiredMetadata &retired : retired_metadata) {
		for (RID buffer : retired.buffers) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	if (pool.is_valid()) {
		RD::get_singleton()->free_rid(pool);
	}
	if (staging.is_valid()) {
		RD::get_singleton()->free_rid(staging);
	}
	if (transcode_faults.is_valid()) {
		RD::get_singleton()->free_rid(transcode_faults);
	}
}

RID MicroGeometryStorage::feedback_create(uint32_t p_capacity) {
	ERR_FAIL_COND_V(p_capacity == 0 || p_capacity > 65536, RID());
	RID id = feedbacks.make_rid();
	Feedback &feedback = *feedbacks.get_or_null(id);
	feedback.capacity = p_capacity;
	feedback.buffer = RD::get_singleton()->storage_buffer_create(sizeof(FeedbackHeader) + p_capacity * sizeof(GPURequest));
	if (feedback.buffer.is_null()) {
		feedbacks.free(id);
		return RID();
	}
	active_feedbacks.push_back(id);
	return id;
}

RID MicroGeometryStorage::feedback_begin(RID p_feedback) {
	Feedback *feedback = feedbacks.get_or_null(p_feedback);
	if (!feedback || feedback->pending) {
		return RID();
	}
	FeedbackHeader header;
	header.capacity = feedback->capacity;
	feedback->retry = false;
	RD::get_singleton()->buffer_update(feedback->buffer, 0, sizeof(header), &header);
	return feedback->buffer;
}

bool MicroGeometryStorage::feedback_needs_retry(RID p_feedback) const {
	const Feedback *feedback = feedbacks.get_or_null(p_feedback);
	return feedback && feedback->retry;
}

void MicroGeometryStorage::feedback_submit(RID p_feedback) {
	Feedback *feedback = feedbacks.get_or_null(p_feedback);
	if (!feedback || feedback->pending) {
		return;
	}
	feedback->pending = true;
	pending_feedback_count++;
	RENDER_TIMESTAMP("Microgeometry Request Readback");
	Error error = RD::get_singleton()->buffer_get_data_async(feedback->buffer, callable_mp_static(&MicroGeometryStorage::_feedback_dispatch).bind(uint64_t(uintptr_t(this)), p_feedback));
	if (error != OK) {
		pending_feedback_count--;
		feedback->pending = false;
		feedback->retry = true;
	}
	RENDER_TIMESTAMP("Microgeometry Request Readback Complete");
}

void MicroGeometryStorage::_feedback_received(const Vector<uint8_t> &p_bytes, RID p_feedback) {
	pending_feedback_count--;
	Feedback *feedback = feedbacks.get_or_null(p_feedback);
	if (!feedback) {
		return;
	}
	feedback->pending = false;
	feedback->retry = p_bytes.size() != int64_t(sizeof(FeedbackHeader)) + int64_t(feedback->capacity) * sizeof(GPURequest);
	ERR_FAIL_COND(p_bytes.size() != int64_t(sizeof(FeedbackHeader)) + int64_t(feedback->capacity) * sizeof(GPURequest));
	FeedbackHeader header;
	memcpy(&header, p_bytes.ptr(), sizeof(header));
	statistics.pressure += header.overflow != 0 || header.count > feedback->capacity;
	feedback->retry = header.overflow != 0 || header.count > feedback->capacity;
	for (uint32_t i = 0; i < MIN(header.count, feedback->capacity); i++) {
		GPURequest request;
		memcpy(&request, p_bytes.ptr() + sizeof(header) + i * sizeof(request), sizeof(request));
		RID asset = RID::from_uint64(request.asset);
		const Asset *source = assets.get_or_null(asset);
		if (source && request.group < source->group_pages.size()) {
			request_group(asset, request.group);
		}
	}
	if (feedback->retired) {
		feedback_free(p_feedback);
	}
}

void MicroGeometryStorage::feedback_free(RID p_feedback) {
	Feedback *feedback = feedbacks.get_or_null(p_feedback);
	if (!feedback) {
		return;
	}
	if (feedback->pending) {
		feedback->retired = true;
		return;
	}
	RD::get_singleton()->free_rid(feedback->buffer);
	active_feedbacks.erase(p_feedback);
	feedbacks.free(p_feedback);
}

void MicroGeometryStorage::_feedback_dispatch(const Vector<uint8_t> &p_bytes, uint64_t p_storage, RID p_feedback) {
	reinterpret_cast<MicroGeometryStorage *>(uintptr_t(p_storage))->_feedback_received(p_bytes, p_feedback);
}
