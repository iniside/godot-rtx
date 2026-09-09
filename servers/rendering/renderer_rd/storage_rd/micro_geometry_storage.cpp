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

using namespace RendererRD;

void MicroGeometryStorage::_read_page(void *p_userdata) {
	ReadTask *task = static_cast<ReadTask *>(p_userdata);
	task->error = task->source->read_page(task->page, task->decoded);
}

RID MicroGeometryStorage::_create_buffer(Asset &r_asset, const void *p_data, uint32_t p_size) {
	if (p_size == 0) {
		return RID();
	}
	Vector<uint8_t> bytes;
	bytes.resize(p_size);
	memcpy(bytes.ptrw(), p_data, p_size);
	RID buffer = RD::get_singleton()->storage_buffer_create(p_size, bytes, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (buffer.is_valid()) {
		r_asset.buffers.push_back(buffer);
		r_asset.metadata_bytes += p_size;
	}
	return buffer;
}

void MicroGeometryStorage::_free_asset_buffers(Asset &r_asset) {
	for (RID buffer : r_asset.buffers) {
		RD::get_singleton()->free_rid(buffer);
	}
	r_asset.buffers.clear();
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
	ERR_FAIL_COND_V_MSG(required > METADATA_BUDGET - MIN(METADATA_BUDGET, statistics.metadata_bytes + statistics.retired_metadata_bytes), RID(), "Microgeometry metadata budget exhausted.");
	if (pool.is_null()) {
		pool = RD::get_singleton()->storage_buffer_create(page_count * PAGE_SIZE, Vector<uint8_t>(), 0,
				RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		ERR_FAIL_COND_V(pool.is_null(), RID());
		slots.resize(page_count);
	}
	RID id = assets.make_rid();
	Asset &asset = *assets.get_or_null(id);
	asset.source = p_source;
	asset.gpu.identity = id.get_id();
	asset.gpu.residency_generation = 1;
	asset.pages.resize(metadata.pages.size());
	asset.group_pages.resize(metadata.groups.size());
	asset.group_pins.resize_initialized(metadata.groups.size());
	asset.group_states.resize_initialized(metadata.groups.size());
	LocalVector<GPUPage> pages;
	pages.resize(metadata.pages.size());
	asset.page_buffer = _create_buffer(asset, pages.ptr(), pages.size() * sizeof(GPUPage));
	asset.group_buffer = _create_buffer(asset, asset.group_states.ptr(), asset.group_states.size() * sizeof(uint32_t));
	asset.gpu.pages = RD::get_singleton()->buffer_get_device_address(asset.page_buffer);
	asset.gpu.group_states = RD::get_singleton()->buffer_get_device_address(asset.group_buffer);
	LocalVector<GPUCluster> clusters;
	for (const MicroGeometryData::Cluster &source : metadata.clusters) {
		GPUCluster cluster = {};
		cluster.surface = source.surface;
		cluster.group = source.group;
		cluster.refined_group = source.refined_group;
		cluster.page = source.page;
		cluster.payload_offset = source.payload_offset;
		cluster.vertex_count = source.vertex_count;
		cluster.triangle_count = source.triangle_count;
		memcpy(cluster.center, source.bounds.center, sizeof(cluster.center));
		cluster.radius = source.bounds.radius;
		cluster.error = source.bounds.error;
		clusters.push_back(cluster);
	}
	LocalVector<GPUGroup> groups;
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
	LocalVector<GPUSurface> surfaces;
	for (const MicroGeometryData::Surface &source : metadata.surfaces) {
		GPUSurface surface = {};
		surface.format = source.format;
		surface.source_surface = source.source_surface;
		surface.vertex_stride = source.vertex_stride;
		memcpy(surface.attribute_offsets, source.attribute_offsets, sizeof(surface.attribute_offsets));
		surface.source_vertex_count = source.source_vertex_count;
		surface.source_triangle_count = source.source_triangle_count;
		surfaces.push_back(surface);
	}
	LocalVector<GPUNode> nodes;
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
	auto create_address = [&](const void *p_data, uint32_t p_size) -> uint64_t {
		RID buffer = _create_buffer(asset, p_data, p_size);
		return buffer.is_valid() ? RD::get_singleton()->buffer_get_device_address(buffer) : 0;
	};
	asset.gpu.clusters = create_address(clusters.ptr(), clusters.size() * sizeof(GPUCluster));
	asset.gpu.groups = create_address(groups.ptr(), groups.size() * sizeof(GPUGroup));
	asset.gpu.surfaces = create_address(surfaces.ptr(), surfaces.size() * sizeof(GPUSurface));
	asset.gpu.nodes = create_address(nodes.ptr(), nodes.size() * sizeof(GPUNode));
	asset.gpu.terminals = create_address(metadata.terminals.ptr(), metadata.terminals.size() * sizeof(uint32_t));
	asset.gpu.roots = create_address(metadata.roots.ptr(), metadata.roots.size() * sizeof(uint32_t));
	asset.gpu.parent_groups = create_address(metadata.parent_groups.ptr(), metadata.parent_groups.size() * sizeof(uint32_t));
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
	active_assets.push_back(id);
	for (uint32_t group : metadata.terminals) {
		for (uint32_t page : asset.group_pages[group]) {
			asset.pages[page].terminal = true;
		}
		request_group(id, group);
	}
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
	r_asset.gpu.ready = 3;
	for (uint32_t group : r_asset.source->get_metadata().terminals) {
		r_asset.gpu.ready &= r_asset.group_states[group];
	}
	if (changed) {
		r_asset.gpu.residency_generation++;
		RD::get_singleton()->buffer_update(r_asset.descriptor_buffer, 0, sizeof(GPUAsset), &r_asset.gpu);
	}
}

void MicroGeometryStorage::_unpublish_page(Asset &r_asset, uint32_t p_page) {
	Page &page = r_asset.pages[p_page];
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
	retired.bytes = asset->metadata_bytes;
	retired.submission = RD::get_singleton()->get_pending_submission_serial();
	retired_metadata.push_back(retired);
	statistics.metadata_bytes -= retired.bytes;
	statistics.retired_metadata_bytes += retired.bytes;
	sources.erase(asset->source.ptr());
	active_assets.erase(p_asset);
	assets.free(p_asset);
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
		retired_metadata.remove_at_unordered(i);
	}
	for (uint32_t i = 0; i < tasks.size();) {
		ReadTask *task = tasks[i];
		if (!WorkerThreadPool::get_singleton()->is_task_completed(task->task)) {
			i++;
			continue;
		}
		Asset *asset = assets.get_or_null(task->asset);
		if (asset && task->error == OK) {
			uint32_t slot_index = _allocate_slot();
			if (slot_index == INVALID_SLOT) {
				i++;
				continue;
			}
			Slot &slot = slots[slot_index];
			Page &page = asset->pages[task->page];
			slot.asset = task->asset;
			slot.page = task->page;
			const Error error = RD::get_singleton()->buffer_update(pool, slot_index * PAGE_SIZE, task->decoded.size(), task->decoded.ptr());
			page.gpu.slot = slot_index;
			page.gpu.generation++;
			page.upload_submission = RD::get_singleton()->get_pending_submission_serial();
			page.status = UPLOADING;
			asset->publication_pending = true;
			if (error != OK) {
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
	for (RID id : active_assets) {
		_publish(*assets.get_or_null(id));
	}
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
				page.status = READING;
				task->task = WorkerThreadPool::get_singleton()->add_native_task(_read_page, task, false, "Microgeometry page");
				tasks.push_back(task);
			}
			asset.has_requests = remaining;
		}
	}
}

void MicroGeometryStorage::page_clas_submitted(RID p_asset, uint32_t p_page, uint32_t p_generation) {
	Asset *asset = assets.get_or_null(p_asset);
	if (!asset || p_page >= asset->pages.size()) {
		return;
	}
	Page &page = asset->pages[p_page];
	if (page.status == RESIDENT && page.gpu.generation == p_generation) {
		page.clas_submission = RD::get_singleton()->get_pending_submission_serial();
		asset->publication_pending = true;
	}
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
}

MicroGeometryStorage::Statistics MicroGeometryStorage::get_statistics() const {
	Statistics result = statistics;
	result.pool_bytes = pool.is_valid() ? uint64_t(page_count) * PAGE_SIZE : 0;
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
		for (const Page &page : assets.get_or_null(id)->pages) {
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
	RD::get_singleton()->buffer_update(feedback->buffer, 0, sizeof(header), &header);
	return feedback->buffer;
}

void MicroGeometryStorage::feedback_submit(RID p_feedback) {
	Feedback *feedback = feedbacks.get_or_null(p_feedback);
	if (!feedback || feedback->pending) {
		return;
	}
	feedback->pending = true;
	pending_feedback_count++;
	Error error = RD::get_singleton()->buffer_get_data_async(feedback->buffer, callable_mp_static(&MicroGeometryStorage::_feedback_dispatch).bind(uint64_t(uintptr_t(this)), p_feedback));
	if (error != OK) {
		pending_feedback_count--;
		feedback->pending = false;
	}
}

void MicroGeometryStorage::_feedback_received(const Vector<uint8_t> &p_bytes, RID p_feedback) {
	pending_feedback_count--;
	Feedback *feedback = feedbacks.get_or_null(p_feedback);
	if (!feedback) {
		return;
	}
	feedback->pending = false;
	ERR_FAIL_COND(p_bytes.size() != int64_t(sizeof(FeedbackHeader)) + int64_t(feedback->capacity) * sizeof(GPURequest));
	FeedbackHeader header;
	memcpy(&header, p_bytes.ptr(), sizeof(header));
	statistics.pressure += header.overflow != 0 || header.count > feedback->capacity;
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
