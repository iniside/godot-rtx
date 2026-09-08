/**************************************************************************/
/*  nrd_effect.cpp                                                        */
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

#include "nrd_effect.h"

#include "thirdparty/nrd/Include/NRD.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_scene_render.h"

namespace RendererRD {

NRDEffect::Context::~Context() {
	RD *rd = RD::get_singleton();
	for (RID shader : shaders) {
		if (shader.is_valid()) {
			rd->free_rid(shader);
		}
	}
	for (const LocalVector<RID> *pool : { &permanent_pool, &transient_pool, &constants }) {
		for (RID resource : *pool) {
			if (resource.is_valid()) {
				rd->free_rid(resource);
			}
		}
	}
	for (RID resource : { normal_roughness, view_depth, diffuse, specular, rr_diffuse_albedo, rr_specular_albedo, rr_normal_roughness, rr_specular_hit_distance }) {
		if (resource.is_valid()) {
			rd->free_rid(resource);
		}
	}
	if (instance) {
		nrd::DestroyInstance(*instance);
	}
}

RID NRDEffect::_create_texture(const Size2i &p_size, RD::DataFormat p_format) {
	RD::TextureFormat format;
	format.width = p_size.x;
	format.height = p_size.y;
	format.format = p_format;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	ERR_FAIL_COND_V_MSG(!RD::get_singleton()->texture_is_format_supported_for_usage(format.format, format.usage_bits), RID(), "The NRD texture format is unsupported on this device.");
	return RD::get_singleton()->texture_create(format, RD::TextureView());
}

NRDEffect::Context *NRDEffect::create_context(const Size2i &p_size) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0 || p_size.x > UINT16_MAX || p_size.y > UINT16_MAX, nullptr);
	const nrd::LibraryDesc &library = *nrd::GetLibraryDesc();
	ERR_FAIL_COND_V_MSG(library.normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM || library.roughnessEncoding != nrd::RoughnessEncoding::LINEAR, nullptr, "NRD guide encoding does not match the imported shader library.");
	Context *context = memnew(Context);
	context->size = p_size;
	nrd::DenoiserDesc denoiser = { 0, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR };
	nrd::InstanceCreationDesc creation = {};
	creation.denoisers = &denoiser;
	creation.denoisersNum = 1;
	if (nrd::CreateInstance(creation, context->instance) != nrd::Result::SUCCESS) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "Failed to create NRD RELAX.");
	}
	const nrd::InstanceDesc &description = *nrd::GetInstanceDesc(*context->instance);
	bool valid = true;
	for (uint32_t pool_index = 0; pool_index < 2; pool_index++) {
		const nrd::TextureDesc *descriptors = pool_index == 0 ? description.permanentPool : description.transientPool;
		const uint32_t count = pool_index == 0 ? description.permanentPoolSize : description.transientPoolSize;
		LocalVector<RID> &pool = pool_index == 0 ? context->permanent_pool : context->transient_pool;
		for (uint32_t i = 0; i < count; i++) {
			RD::DataFormat format = RD::DATA_FORMAT_MAX;
			switch (descriptors[i].format) {
				case nrd::Format::R8_UNORM: {
					format = RD::DATA_FORMAT_R8_UNORM;
				} break;
				case nrd::Format::R16_SFLOAT: {
					format = RD::DATA_FORMAT_R16_SFLOAT;
				} break;
				case nrd::Format::R32_SFLOAT: {
					format = RD::DATA_FORMAT_R32_SFLOAT;
				} break;
				case nrd::Format::RGBA8_UNORM: {
					format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
				} break;
				case nrd::Format::RGBA16_SFLOAT: {
					format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
				} break;
				default: {
				} break;
			}
			if (format == RD::DATA_FORMAT_MAX || descriptors[i].downsampleFactor == 0) {
				valid = false;
				break;
			}
			const uint32_t factor = descriptors[i].downsampleFactor;
			RID texture = _create_texture(Size2i((p_size.x + factor - 1) / factor, (p_size.y + factor - 1) / factor), format);
			pool.push_back(texture);
			valid &= texture.is_valid();
		}
	}
	for (uint32_t i = 0; valid && i < description.pipelinesNum; i++) {
		const nrd::ComputeShaderDesc &spirv = description.pipelines[i].computeShaderSPIRV;
		if (!spirv.bytecode || spirv.size == 0 || spirv.size > INT32_MAX) {
			valid = false;
			break;
		}
		RD::ShaderStageSPIRVData stage;
		stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
		stage.spirv.resize(spirv.size);
		memcpy(stage.spirv.ptrw(), spirv.bytecode, spirv.size);
		RID shader = RD::get_singleton()->shader_create_from_spirv(Vector<RD::ShaderStageSPIRVData>({ stage }), String("NRD ") + description.pipelines[i].shaderIdentifier);
		context->shaders.push_back(shader);
		RID pipeline = shader.is_valid() ? RD::get_singleton()->compute_pipeline_create(shader) : RID();
		context->pipelines.push_back(pipeline);
		valid &= pipeline.is_valid();
	}
	context->normal_roughness = _create_texture(p_size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
	context->view_depth = _create_texture(p_size, RD::DATA_FORMAT_R32_SFLOAT);
	context->diffuse = _create_texture(p_size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
	context->specular = _create_texture(p_size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
	context->rr_diffuse_albedo = _create_texture(p_size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
	context->rr_specular_albedo = _create_texture(p_size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
	context->rr_normal_roughness = _create_texture(p_size, RD::DATA_FORMAT_R16G16B16A16_SFLOAT);
	context->rr_specular_hit_distance = _create_texture(p_size, RD::DATA_FORMAT_R16_SFLOAT);
	valid &= context->normal_roughness.is_valid() && context->view_depth.is_valid() && context->diffuse.is_valid() && context->specular.is_valid();
	valid &= context->rr_diffuse_albedo.is_valid() && context->rr_specular_albedo.is_valid() && context->rr_normal_roughness.is_valid() && context->rr_specular_hit_distance.is_valid();
	nrd::RelaxSettings settings;
	settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
	valid &= nrd::SetDenoiserSettings(*context->instance, 0, &settings) == nrd::Result::SUCCESS;
	if (!valid) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "Failed to allocate the NRD RELAX pipeline and resources.");
	}
	return context;
}

bool NRDEffect::_process_frame(Context *p_context, const Frame &p_frame, bool p_compose, bool p_denoised) {
	const uint32_t pass = p_compose ? 1 : 0;
	RID shader = frame_shader.version_get_shader(shader_version, pass);
	ERR_FAIL_COND_V(shader.is_null() || frame_pipelines[pass].is_null(), false);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_frame.scene_data));
	for (uint32_t i = 0; i < 6; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1 + i, p_frame.surface[i]));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, p_frame.depth));
	if (p_compose) {
		RID black = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 8, p_denoised ? p_context->diffuse : (p_frame.noisy_diffuse.is_valid() ? p_frame.noisy_diffuse : black)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 9, p_denoised ? p_context->specular : (p_frame.noisy_specular.is_valid() ? p_frame.noisy_specular : black)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, p_frame.color));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, p_frame.fog));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 12, p_frame.radiance));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, p_frame.directional_lights));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, p_frame.separate_specular.is_valid() ? p_frame.separate_specular : p_context->normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 15, samplers[1]));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 16, p_frame.indirect_diffuse.is_valid() ? p_frame.indirect_diffuse : black));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 17, p_frame.camera_radiance.is_valid() ? p_frame.camera_radiance : black));
	} else {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_context->normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_context->view_depth));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, p_frame.velocity));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, p_frame.normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, p_context->rr_diffuse_albedo));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, p_context->rr_specular_albedo));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, p_context->rr_normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, p_context->rr_specular_hit_distance));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 16, p_frame.reflection_hit_distance.is_valid() ? p_frame.reflection_hit_distance : TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK)));
	}
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms);
	ERR_FAIL_COND_V(uniform_set.is_null(), false);
	struct PushConstant {
		uint32_t width;
		uint32_t height;
		uint32_t orthogonal;
		uint32_t fog_enabled;
		float fog_inverse_length;
		float fog_spread;
		uint32_t fog_legacy_blending;
		uint32_t separate_specular;
		float environment_energy;
		uint32_t ddgi_enabled;
		uint32_t camera_radiance_enabled;
		uint32_t camera_radiance_full;
		uint32_t reflection_hit_distance_enabled;
		uint32_t padding[3];
	} push = { uint32_t(p_context->size.x), uint32_t(p_context->size.y), p_frame.orthogonal, p_frame.fog_enabled, p_frame.fog_inverse_length, p_frame.fog_spread, p_frame.fog_legacy_blending, p_frame.separate_specular.is_valid(), p_frame.environment_energy, p_frame.indirect_diffuse.is_valid(), p_frame.camera_radiance.is_valid(), p_frame.camera_radiance.is_valid() && !p_denoised, p_frame.reflection_hit_distance.is_valid(), { 0, 0, 0 } };
	RD *rd = RD::get_singleton();
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, frame_pipelines[pass]);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch_threads(list, p_context->size.x, p_context->size.y, 1);
	rd->compute_list_end();
	return true;
}

bool NRDEffect::prepare(Context *p_context, const Frame &p_frame) {
	ERR_FAIL_NULL_V(p_context, false);
	p_context->rr_specular_hit_distance_valid = false;
	p_context->rr_reset_history = !p_frame.history_valid;
	ERR_FAIL_COND_V(!_process_frame(p_context, p_frame, false), false);
	p_context->rr_specular_hit_distance_valid = p_frame.reflection_hit_distance.is_valid();
	return true;
}

bool NRDEffect::process(Context *p_context, const Frame &p_frame, bool p_denoise) {
	ERR_FAIL_NULL_V(p_context, false);
	if (!p_denoise) {
		p_context->last_frame = UINT64_MAX;
		return _process_frame(p_context, p_frame, true, false);
	}
	nrd::CommonSettings common;
	Projection correction;
	correction.set_depth_correction(false, false, true);
	MaterialStorage::store_camera(correction * p_frame.projection, common.viewToClipMatrix);
	MaterialStorage::store_camera(correction * p_frame.previous_projection, common.viewToClipMatrixPrev);
	MaterialStorage::store_camera(Projection(p_frame.camera.affine_inverse()), common.worldToViewMatrix);
	MaterialStorage::store_camera(Projection(p_frame.previous_camera.affine_inverse()), common.worldToViewMatrixPrev);
	for (uint32_t i = 0; i < 2; i++) {
		common.resourceSize[i] = p_context->size[i];
		common.resourceSizePrev[i] = p_context->size[i];
		common.rectSize[i] = p_context->size[i];
		common.rectSizePrev[i] = p_context->size[i];
		common.motionVectorScale[i] = 1.0f / p_context->size[i];
		common.cameraJitter[i] = -p_frame.jitter[i] * 0.5f * p_context->size[i];
		common.cameraJitterPrev[i] = -p_frame.previous_jitter[i] * 0.5f * p_context->size[i];
	}
	common.motionVectorScale[2] = 1.0f;
	common.timeDeltaBetweenFrames = p_frame.time_step * 1000.0f;
	common.frameIndex = p_frame.frame_index;
	common.accumulationMode = p_frame.history_valid && p_context->last_frame != UINT64_MAX && p_frame.frame_index == p_context->last_frame + 1 ? nrd::AccumulationMode::CONTINUE : nrd::AccumulationMode::CLEAR_AND_RESTART;
	ERR_FAIL_COND_V_MSG(nrd::SetCommonSettings(*p_context->instance, common) != nrd::Result::SUCCESS, false, "NRD rejected the frame settings.");
	const nrd::DispatchDesc *dispatches = nullptr;
	uint32_t dispatch_count = 0;
	const nrd::Identifier identifier = 0;
	ERR_FAIL_COND_V_MSG(nrd::GetComputeDispatches(*p_context->instance, &identifier, 1, dispatches, dispatch_count) != nrd::Result::SUCCESS, false, "NRD failed to produce RELAX dispatches.");
	const nrd::InstanceDesc &description = *nrd::GetInstanceDesc(*p_context->instance);
	const nrd::SPIRVBindingOffsets &offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
	RD *rd = RD::get_singleton();
	while (p_context->constants.size() < dispatch_count) {
		RID buffer = rd->uniform_buffer_create(description.constantBufferMaxDataSize);
		ERR_FAIL_COND_V(buffer.is_null(), false);
		p_context->constants.push_back(buffer);
	}
	for (uint32_t i = 0; i < dispatch_count; i++) {
		const nrd::DispatchDesc &dispatch = dispatches[i];
		ERR_FAIL_UNSIGNED_INDEX_V(dispatch.pipelineIndex, p_context->pipelines.size(), false);
		RID shader = p_context->shaders[dispatch.pipelineIndex];
		LocalVector<RD::Uniform> resources;
		uint32_t texture_index = description.resourcesBaseRegisterIndex + offsets.textureOffset;
		uint32_t image_index = description.resourcesBaseRegisterIndex + offsets.storageTextureAndBufferOffset;
		for (uint32_t j = 0; j < dispatch.resourcesNum; j++) {
			const nrd::ResourceDesc &resource = dispatch.resources[j];
			RID texture;
			switch (resource.type) {
				case nrd::ResourceType::IN_MV: {
					texture = p_frame.surface[3];
				} break;
				case nrd::ResourceType::IN_NORMAL_ROUGHNESS: {
					texture = p_context->normal_roughness;
				} break;
				case nrd::ResourceType::IN_VIEWZ: {
					texture = p_context->view_depth;
				} break;
				case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: {
					texture = p_frame.noisy_diffuse;
				} break;
				case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: {
					texture = p_frame.noisy_specular;
				} break;
				case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: {
					texture = p_context->diffuse;
				} break;
				case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: {
					texture = p_context->specular;
				} break;
				case nrd::ResourceType::PERMANENT_POOL:
					ERR_FAIL_UNSIGNED_INDEX_V(resource.indexInPool, p_context->permanent_pool.size(), false);
					texture = p_context->permanent_pool[resource.indexInPool];
					break;
				case nrd::ResourceType::TRANSIENT_POOL:
					ERR_FAIL_UNSIGNED_INDEX_V(resource.indexInPool, p_context->transient_pool.size(), false);
					texture = p_context->transient_pool[resource.indexInPool];
					break;
				default: {
					ERR_FAIL_V_MSG(false, "Unexpected NRD RELAX resource.");
				}
			}
			ERR_FAIL_COND_V(texture.is_null(), false);
			const bool storage = resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
			resources.push_back(RD::Uniform(storage ? RD::UNIFORM_TYPE_IMAGE : RD::UNIFORM_TYPE_TEXTURE, storage ? image_index++ : texture_index++, texture));
		}
		RID resource_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, description.resourcesSpaceIndex, resources);
		ERR_FAIL_COND_V(resource_set.is_null(), false);
		RID constant_set;
		if (description.pipelines[dispatch.pipelineIndex].hasConstantData) {
			ERR_FAIL_COND_V(dispatch.constantBufferDataSize > description.constantBufferMaxDataSize, false);
			rd->buffer_update(p_context->constants[i], 0, dispatch.constantBufferDataSize, dispatch.constantBufferData);
			LocalVector<RD::Uniform> constants;
			constants.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, description.constantBufferRegisterIndex + offsets.constantBufferOffset, p_context->constants[i]));
			for (uint32_t j = 0; j < description.samplersNum; j++) {
				constants.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, description.samplersBaseRegisterIndex + offsets.samplerOffset + j, samplers[uint32_t(description.samplers[j])]));
			}
			constant_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, description.constantBufferAndSamplersSpaceIndex, constants);
			ERR_FAIL_COND_V(constant_set.is_null(), false);
		}
		rd->draw_command_begin_label(Span<char>(dispatch.name, strlen(dispatch.name)));
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, p_context->pipelines[dispatch.pipelineIndex]);
		rd->compute_list_bind_uniform_set(list, resource_set, description.resourcesSpaceIndex);
		if (constant_set.is_valid()) {
			rd->compute_list_bind_uniform_set(list, constant_set, description.constantBufferAndSamplersSpaceIndex);
		}
		rd->compute_list_dispatch(list, dispatch.gridWidth, dispatch.gridHeight, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}
	ERR_FAIL_COND_V(!_process_frame(p_context, p_frame, true, true), false);
	p_context->last_frame = p_frame.frame_index;
	return true;
}

NRDEffect::NRDEffect(bool p_radiance_array, uint32_t p_roughness_layers) {
	String defines = "#define MAX_ROUGHNESS_LOD " + itos(p_roughness_layers - 1) + ".0\n";
	defines += "#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(RendererSceneRender::MAX_DIRECTIONAL_LIGHTS) + "\n";
	if (p_radiance_array) {
		defines += "#define USE_RADIANCE_OCTMAP_ARRAY\n";
	}
#ifdef REAL_T_IS_DOUBLE
	defines += "#define USE_DOUBLE_PRECISION\n";
#endif
	Vector<ShaderRD::VariantDefine> modes;
	modes.push_back(ShaderRD::VariantDefine(0, "#define MODE_PREPARE\n", true));
	modes.push_back(ShaderRD::VariantDefine(0, "#define MODE_COMPOSE\n", true));
	frame_shader.initialize(modes, defines, Vector<RD::PipelineImmutableSampler>(), Vector<uint64_t>(), false, false);
	shader_version = frame_shader.version_create();
	for (uint32_t i = 0; i < 2; i++) {
		frame_pipelines[i] = RD::get_singleton()->compute_pipeline_create(frame_shader.version_get_shader(shader_version, i));
		RD::SamplerState sampler;
		sampler.mag_filter = sampler.min_filter = i == 0 ? RD::SAMPLER_FILTER_NEAREST : RD::SAMPLER_FILTER_LINEAR;
		sampler.repeat_u = sampler.repeat_v = sampler.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		samplers[i] = RD::get_singleton()->sampler_create(sampler);
	}
}

NRDEffect::~NRDEffect() {
	frame_shader.version_free(shader_version);
	for (RID sampler : samplers) {
		if (sampler.is_valid()) {
			RD::get_singleton()->free_rid(sampler);
		}
	}
}

} // namespace RendererRD
