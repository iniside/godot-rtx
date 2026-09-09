#include "micro_geometry_selection.h"

#include "core/io/marshalls.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererSceneRenderImplementation;

MicroGeometrySelection::Pass::~Pass() {
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	if (raster_memory_accounted) {
		storage->remove_raster_selection_memory(memory_bytes);
	}
	for (const Pin &pin : pins) {
		storage->unpin_group(pin.asset, pin.group);
	}
	for (RID asset : assets) {
		storage->release(asset);
	}
	if (feedback.is_valid()) {
		RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->feedback_free(feedback);
	}
	for (RID resource : resources) {
		RD::get_singleton()->free_rid(resource);
	}
}

MicroGeometrySelection::DepthPyramid::~DepthPyramid() {
	if (texture.is_valid()) {
		RD::get_singleton()->free_rid(texture);
	}
}

MicroGeometrySelection::MicroGeometrySelection() {
	Vector<String> modes;
	modes.push_back("");
	String defines;
#ifdef REAL_T_IS_DOUBLE
	defines = "\n#define USE_DOUBLE_PRECISION\n";
#endif
	shader.initialize(modes, defines);
	version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(version, 0));
	hzb_shader.initialize(modes);
	hzb_version = hzb_shader.version_create();
	hzb_pipeline = RD::get_singleton()->compute_pipeline_create(hzb_shader.version_get_shader(hzb_version, 0));
}

MicroGeometrySelection::~MicroGeometrySelection() {
	shader.version_free(version);
	hzb_shader.version_free(hzb_version);
}

RID MicroGeometrySelection::_buffer(Pass &r_pass, uint64_t p_size, const void *p_data, uint32_t p_usage) {
	p_size = MAX(p_size, uint64_t(16));
	ERR_FAIL_COND_V(p_size > UINT32_MAX, RID());
	Vector<uint8_t> bytes;
	if (p_data) {
		bytes.resize(p_size);
		memcpy(bytes.ptrw(), p_data, p_size);
	}
	RID buffer = RD::get_singleton()->storage_buffer_create(p_size, bytes, p_usage, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (buffer.is_valid()) {
		r_pass.resources.push_back(buffer);
		r_pass.memory_bytes += p_size;
	}
	return buffer;
}

MicroGeometrySelection::Pass *MicroGeometrySelection::create(const Vector<Task> &p_tasks, const Vector<Bin> &p_bins, const Parameters &p_parameters, uint32_t p_levels, uint32_t p_native_stride, RID p_instances, RID p_surfaces, const Vector<RID> &p_dependencies) {
	ERR_FAIL_COND_V(p_tasks.is_empty() || p_bins.is_empty() || pipeline.is_null(), nullptr);
	Pass *pass = memnew(Pass);
	pass->data = p_parameters;
	pass->data.task_count = p_tasks.size();
	pass->data.bin_count = p_bins.size();
	pass->levels = p_levels;
	pass->persistent_instances = p_instances;
	pass->persistent_surfaces = p_surfaces;
	pass->dependencies = p_dependencies;
	pass->bin_data = p_bins;
	pass->selected_capacity = p_bins[p_bins.size() - 1].offset + p_bins[p_bins.size() - 1].capacity;
	if (pass->data.group_work > MAX_WORK_ITEMS || pass->data.cluster_work > MAX_WORK_ITEMS) {
		pass->data.flags |= 8;
	}
	pass->tasks = _buffer(*pass, uint64_t(p_tasks.size()) * sizeof(Task), p_tasks.ptr());
	pass->bins = _buffer(*pass, MAX(uint64_t(16), uint64_t(p_bins.size()) * sizeof(Bin)));

	pass->group_states = _buffer(*pass, pass->data.flags & 8 ? 16 : uint64_t(pass->data.group_work) * 4);
	pass->rejected = _buffer(*pass, uint64_t(pass->data.flags & 8 ? pass->data.coarse_work : pass->data.cluster_work) * 4);
	pass->validity = _buffer(*pass, uint64_t(p_tasks.size()) * 4);
	pass->counts = _buffer(*pass, uint64_t(p_bins.size()) * 4, nullptr, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	pass->initial_counts = _buffer(*pass, uint64_t(p_bins.size()) * 4);
	pass->capacity_state = _buffer(*pass, 16);
	pass->statistics = _buffer(*pass, 16);
	pass->commands = _buffer(*pass, uint64_t(pass->selected_capacity) * 20, nullptr, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	pass->selected = _buffer(*pass, uint64_t(pass->selected_capacity) * sizeof(MicroGeometrySelectedCluster));
	pass->native_instances = _buffer(*pass, uint64_t(p_tasks.size()) * p_native_stride);
	pass->parameters = _buffer(*pass, sizeof(Parameters));
	pass->raster_parameters = RD::get_singleton()->uniform_buffer_create(sizeof(MicroGeometryRasterParameters));
	for (RID uniform : { pass->raster_parameters }) {
		if (uniform.is_valid()) {
			pass->resources.push_back(uniform);
		}
	}
	for (RID required : { pass->tasks, pass->bins, pass->group_states, pass->rejected, pass->validity, pass->counts, pass->initial_counts, pass->capacity_state, pass->commands, pass->selected, pass->native_instances, pass->parameters, pass->raster_parameters, pass->statistics }) {
		if (required.is_null()) {
			memdelete(pass);
			return nullptr;
		}
	}
	RD::get_singleton()->buffer_update(pass->bins, 0, p_bins.size() * sizeof(Bin), p_bins.ptr());
	pass->memory_bytes += sizeof(MicroGeometryRasterParameters);
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	pass->feedback = storage->feedback_create();
	pass->requests_fallback = _buffer(*pass, sizeof(RendererRD::MicroGeometryStorage::FeedbackHeader));
	if (pass->requests_fallback.is_null()) {
		memdelete(pass);
		return nullptr;
	}
	MicroGeometryRasterParameters raster;
	raster.page_pool = RD::get_singleton()->buffer_get_device_address(storage->get_pool());
	RD::get_singleton()->buffer_update(pass->raster_parameters, 0, sizeof(raster), &raster);
	if ((pass->data.flags & 32) == 0) {
		storage->add_raster_selection_memory(pass->memory_bytes);
		pass->raster_memory_accounted = true;
	}
	return pass;
}

void MicroGeometrySelection::_dispatch(Pass *p_pass, uint32_t p_mode, uint32_t p_items, RID p_hzb) {
	if (p_items == 0) {
		return;
	}
	p_pass->data.mode = p_mode;
	RD::get_singleton()->buffer_update(p_pass->parameters, 0, sizeof(Parameters), &p_pass->data);
	if (p_hzb.is_null()) {
		p_hzb = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	}
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_pass->parameters));
	uint32_t binding = 1;
	for (RID buffer : { p_pass->tasks, p_pass->bins, p_pass->persistent_instances, p_pass->persistent_surfaces, p_pass->group_states, p_pass->rejected, p_pass->validity, p_pass->counts, p_pass->initial_counts, p_pass->capacity_state, p_pass->commands, p_pass->selected, p_pass->native_instances, p_pass->requests }) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding++, buffer));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 15, p_hzb));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, p_pass->statistics));
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader.version_get_shader(version, 0), 0, uniforms);
	RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(list, uniform_set, 0);
	for (RID dependency : p_pass->dependencies) {
		RD::get_singleton()->compute_list_add_buffer_dependency(list, dependency);
	}
	uint32_t groups = (p_items + 63) / 64;
	RD::get_singleton()->compute_list_dispatch(list, MIN(groups, 1024u), (groups + 1023) / 1024, 1);
	RD::get_singleton()->compute_list_end();
}

void MicroGeometrySelection::select(Pass *p_pass, RID p_hzb) {
	ERR_FAIL_NULL(p_pass);
	const bool rt = (p_pass->data.flags & 32) != 0;
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Selection Reset" : "Microgeometry Raster Selection Reset");
	p_pass->requests = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->feedback_begin(p_pass->feedback);
	p_pass->feedback_active = p_pass->requests.is_valid();
	if (!p_pass->feedback_active) {
		p_pass->requests = p_pass->requests_fallback;
		RD::get_singleton()->buffer_clear(p_pass->requests, 0, sizeof(RendererRD::MicroGeometryStorage::FeedbackHeader));
	}
	p_pass->recovered = false;
	RD::get_singleton()->buffer_clear(p_pass->statistics, 0, 16);
	RD::get_singleton()->draw_command_begin_label("Microgeometry Selection");
	RD::get_singleton()->buffer_clear(p_pass->counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	RD::get_singleton()->buffer_clear(p_pass->initial_counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	RD::get_singleton()->buffer_clear(p_pass->capacity_state, 0, 16);
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Task Init" : "Microgeometry Raster Task Init");
	_dispatch(p_pass, 0, p_pass->data.task_count, p_hzb);
	if (!(p_pass->data.flags & 8)) {
		RENDER_TIMESTAMP(rt ? "Microgeometry RT Group Evaluate" : "Microgeometry Raster Group Evaluate");
		_dispatch(p_pass, 1, p_pass->data.group_work, p_hzb);
		RENDER_TIMESTAMP(rt ? "Microgeometry RT DAG Resolve" : "Microgeometry Raster DAG Resolve");
		for (uint32_t depth = p_pass->levels; depth > 0; depth--) {
			p_pass->data.level = depth - 1;
			_dispatch(p_pass, 2, p_pass->data.group_work, p_hzb);
		}
		RENDER_TIMESTAMP(rt ? "Microgeometry RT Cluster Count" : "Microgeometry Raster Cluster Count");
		_dispatch(p_pass, 3, p_pass->data.cluster_work, p_hzb);
		RENDER_TIMESTAMP(rt ? "Microgeometry RT Capacity Check" : "Microgeometry Raster Capacity Check");
		_dispatch(p_pass, 4, p_pass->data.bin_count, p_hzb);
	}
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Cluster Emit" : "Microgeometry Raster Cluster Emit");
	RD::get_singleton()->buffer_clear(p_pass->counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	_dispatch(p_pass, 5, p_pass->data.flags & 8 ? p_pass->data.coarse_work : p_pass->data.cluster_work, p_hzb);
	RD::get_singleton()->draw_command_end_label();
	RENDER_TIMESTAMP(rt ? "Microgeometry RT Selection Complete" : "Microgeometry Raster Selection Complete");
}

void MicroGeometrySelection::recover(Pass *p_pass, RID p_hzb) {
	ERR_FAIL_NULL(p_pass);
	RENDER_TIMESTAMP("Microgeometry Raster Recovery Select");
	p_pass->recovered = true;
	RD::get_singleton()->buffer_clear(p_pass->counts, 0, MAX(16u, p_pass->data.bin_count * 4));
	_dispatch(p_pass, 6, p_pass->data.flags & 8 ? p_pass->data.coarse_work : p_pass->data.cluster_work, p_hzb);
	RENDER_TIMESTAMP("Microgeometry Raster Recovery Select Complete");
}

void MicroGeometrySelection::update_frozen(Pass *p_pass) {
	ERR_FAIL_NULL(p_pass);
	p_pass->requests = p_pass->requests_fallback;
	_dispatch(p_pass, 0, p_pass->data.task_count, RID());
}

bool MicroGeometrySelection::freeze(Pass *p_pass) {
	ERR_FAIL_NULL_V(p_pass, false);
	RENDER_TIMESTAMP("Microgeometry Freeze Prepare");
	if (p_pass->recovered) {
		_dispatch(p_pass, 7, p_pass->data.bin_count, RID());
	}
	_dispatch(p_pass, 8, p_pass->selected_capacity, RID());
	RENDER_TIMESTAMP("Microgeometry Freeze Readback");
	Vector<uint8_t> counts = RD::get_singleton()->buffer_get_data(p_pass->counts);
	Vector<uint8_t> selected = RD::get_singleton()->buffer_get_data(p_pass->selected);
	RENDER_TIMESTAMP("Microgeometry Freeze Pinning");
	ERR_FAIL_COND_V(counts.size() < int64_t(p_pass->data.bin_count) * 4 || selected.size() < int64_t(p_pass->selected_capacity) * int64_t(sizeof(MicroGeometrySelectedCluster)), false);
	auto *storage = RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage();
	HashMap<RID, HashSet<uint32_t>> used;
	for (uint32_t bin = 0; bin < p_pass->bin_data.size(); bin++) {
		uint32_t count = decode_uint32(counts.ptr() + bin * 4);
		ERR_FAIL_COND_V(count > p_pass->bin_data[bin].capacity, false);
		for (uint32_t index = 0; index < count; index++) {
			MicroGeometrySelectedCluster record;
			memcpy(&record, selected.ptr() + uint64_t(p_pass->bin_data[bin].offset + index) * sizeof(record), sizeof(record));
			RID asset = RID::from_uint64(record.asset);
			Ref<MicroGeometryData> source = storage->get_source(asset);
			ERR_FAIL_COND_V(source.is_null() || record.cluster >= uint32_t(source->get_metadata().clusters.size()), false);
			used[asset].insert(source->get_metadata().clusters[record.cluster].group);
		}
	}
	for (uint32_t index = 0; index < p_pass->pins.size();) {
		const Pass::Pin &pin = p_pass->pins[index];
		if (!used.has(pin.asset) || !used[pin.asset].has(pin.group)) {
			storage->unpin_group(pin.asset, pin.group);
			p_pass->pins.remove_at(index);
		} else {
			index++;
		}
	}
	p_pass->frozen = true;
	RENDER_TIMESTAMP("Microgeometry Freeze Complete");
	return true;
}

void MicroGeometrySelection::build_depth_pyramid(DepthPyramid &r_pyramid, RID p_depth, const Size2i &p_size) {
	RENDER_TIMESTAMP("Microgeometry HZB");
	Size2i size(Math::nearest_power_of_2_templated(p_size.x), Math::nearest_power_of_2_templated(p_size.y));
	if (r_pyramid.texture.is_valid() && r_pyramid.size != size) {
		RD::get_singleton()->free_rid(r_pyramid.texture);
		r_pyramid.texture = RID();
		r_pyramid.levels.clear();
	}
	if (r_pyramid.texture.is_null()) {
		RD::TextureFormat format;
		format.width = size.x;
		format.height = size.y;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		format.mipmaps = Math::get_shift_from_power_of_2(uint32_t(MAX(size.x, size.y))) + 1;
		r_pyramid.texture = RD::get_singleton()->texture_create(format, RD::TextureView());
		ERR_FAIL_COND(r_pyramid.texture.is_null());
		r_pyramid.size = size;
		for (uint32_t level = 0; level < format.mipmaps; level++) {
			r_pyramid.levels.push_back(RD::get_singleton()->texture_create_shared_from_slice(RD::TextureView(), r_pyramid.texture, 0, level));
		}
	}
	RD::get_singleton()->draw_command_begin_label("Microgeometry Depth Pyramid");
	for (uint32_t level = 0; level < r_pyramid.levels.size(); level++) {
		uint32_t push[4] = { uint32_t(level == 0 ? p_size.x : MAX(1, size.x >> (level - 1))), uint32_t(level == 0 ? p_size.y : MAX(1, size.y >> (level - 1))), level == 0 ? 1u : 0u, 0 };
		RID source = level == 0 ? p_depth : r_pyramid.levels[level - 1];
		RID uniform = UniformSetCacheRD::get_singleton()->get_cache(hzb_shader.version_get_shader(hzb_version, 0), 0, RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, source), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, r_pyramid.levels[level]));
		RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(list, hzb_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(list, uniform, 0);
		RD::get_singleton()->compute_list_set_push_constant(list, push, sizeof(push));
		RD::get_singleton()->compute_list_dispatch_threads(list, MAX(1, size.x >> level), MAX(1, size.y >> level), 1);
		RD::get_singleton()->compute_list_end();
	}
	RD::get_singleton()->draw_command_end_label();
	RENDER_TIMESTAMP("Microgeometry HZB Complete");
}

RID MicroGeometrySelection::get_raster_uniform_set(Pass *p_pass, RID p_shader) {
	return UniformSetCacheRD::get_singleton()->get_cache(p_shader, 4,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_pass->selected),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_pass->persistent_instances),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_pass->persistent_surfaces),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, p_pass->raster_parameters),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_pass->native_instances));
}

void MicroGeometrySelection::add_draw_dependencies(Pass *p_pass, RD::DrawListID p_list) {
	for (RID dependency : p_pass->dependencies) {
		RD::get_singleton()->draw_list_add_buffer_dependency(p_list, dependency);
	}
}

void MicroGeometrySelection::submit_feedback(Pass *p_pass) {
	if (p_pass->feedback_active) {
		RendererRD::MeshStorage::get_singleton()->get_micro_geometry_storage()->feedback_submit(p_pass->feedback);
		p_pass->feedback_active = false;
	}
}
