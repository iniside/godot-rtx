/**************************************************************************/
/*  scene_shader_forward_clustered.cpp                                    */
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

#include "scene_shader_forward_clustered.h"

#include "core/config/project_settings.h"
#include "core/math/math_defs.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"

using namespace RendererSceneRenderImplementation;

uint32_t SceneShaderForwardClustered::ShaderData::get_surface_material_flags() const {
	uint32_t flags = RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_VALID;
	const bool unsupported_alpha = uses_alpha_pass();
	const bool rt_classification_mismatch = uses_alpha_pass() != rt_uses_alpha_pass() || cull_mode != rt_cull_mode();
	const bool procedural_coverage = (uses_alpha_clip || uses_discard) && !generated_standard_material;
	const bool procedural_emission = uses_emission && !generated_standard_material;
	if (!hit_code.rt_unsupported_reason.is_empty() || rtxdi_surface_unsupported || rt != nullptr || unsupported_alpha || rt_classification_mismatch || procedural_emission || procedural_coverage) {
		flags |= RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_UNSUPPORTED;
	}
	if (uses_alpha_clip) {
		flags |= RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_ALPHA_TESTED;
	}
	if (cull_mode == RSE::CULL_MODE_DISABLED) {
		flags |= RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_DOUBLE_SIDED;
	}
	if (uses_emission) {
		flags |= RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_EMISSIVE;
	}
	if (uses_normal_map) {
		flags |= RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_NORMAL_MAP;
	}
	if (uses_vertex || uses_position || writes_modelview_or_projection || uses_world_coordinates) {
		flags |= RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_DEFORMED;
	}
	return flags;
}

void SceneShaderForwardClustered::ShaderData::_compile_hit_code(const String &p_code) {
	SceneShaderForwardClustered *owner = SceneShaderForwardClustered::singleton;
	if (hit_version.is_valid()) {
		owner->hit_shader.version_free(hit_version);
		hit_version = RID();
	}
	hit_code = ShaderCompiler::GeneratedCode();
	hit_uniforms.clear();
	if (p_code.is_empty()) {
		return;
	}
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.uniforms = &hit_uniforms;
	Error error;
	{
		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		error = owner->hit_compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, path, hit_code);
	}
	ERR_FAIL_COND_MSG(error != OK, "Native ray tracing material frontend compilation failed.");
	const uint32_t surface_flags = get_surface_material_flags();
	const bool unsupported = (surface_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::RTXDI_MATERIAL_UNSUPPORTED) != 0;
	if (!hit_code.rt_unsupported_reason.is_empty()) {
		WARN_PRINT(vformat("Native ray tracing material %s: %s", path, hit_code.rt_unsupported_reason));
	}
	HashMap<String, String> sections;
	String callbacks = "void rt_load_material_uniforms() {\n" + (unsupported ? String() : hit_code.code["rt_uniform_init"]) + "}\n";
	callbacks += "void rt_material_vertex_program() {\n" + (unsupported ? String() : hit_code.code["vertex"]) + "}\n";
	callbacks += "void rt_material_fragment_program() {\n" + (unsupported ? String() : hit_code.code["fragment"]) + "}\n";
	callbacks += "void rt_material_vertex() {\n" + (unsupported ? String() : hit_code.code["rt_vertex_enter"]) + "rt_material_vertex_program();\n" + (unsupported ? String() : hit_code.code["rt_values_leave"]) + "}\n";
	callbacks += "void rt_material_fragment() {\n" + (unsupported ? String() : hit_code.code["rt_fragment_enter"]) + "rt_material_fragment_program();\n" + (unsupported ? String() : hit_code.code["rt_values_leave"]) + "}\n";
	callbacks += "void rt_material_interpolate_vertices(GeometryHitInput hit, GeometryTriangle triangle, float3 bary) {\n";
	callbacks += unsupported ? String() : hit_code.code["rt_varyings_init"];
	callbacks += "RTVertexInterpolants sums = (RTVertexInterpolants)0;\nrt_interpolants_dx = (RTVertexInterpolants)0;\nrt_interpolants_dy = (RTVertexInterpolants)0;\nfor (uint rt_vertex_index = 0u; rt_vertex_index < 3u; rt_vertex_index++) {\nfloat rt_vertex_weight = bary[rt_vertex_index];\nrt_initialize_vertex(hit, triangle, rt_vertex_index);\nrt_material_vertex();\nrt_accumulate_vertex(sums, rt_vertex_weight);\nrt_accumulate_vertex(rt_interpolants_dx, rt_bary_dx[rt_vertex_index]);\nrt_accumulate_vertex(rt_interpolants_dy, rt_bary_dy[rt_vertex_index]);\n";
	callbacks += unsupported ? String() : hit_code.code["rt_varyings_accumulate"];
	callbacks += "}\nrt_restore_vertex(sums);\n";
	callbacks += unsupported ? String() : hit_code.code["rt_varyings_restore"];
	callbacks += "}\n";
	sections["rt_material_callbacks"] = callbacks;
	Vector<String> defines = unsupported ? Vector<String>() : hit_code.defines;
	defines.push_back("#define RT_SURFACE_FLAGS " + uitos(surface_flags) + "u\n");
	defines.push_back("#define RT_CULL_MODE " + itos(rt_cull_mode()) + "\n");
	String globals = unsupported ? String() : hit_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	hit_version = owner->hit_shader.version_create(false);
	owner->hit_shader.version_set_raytracing_code(hit_version, sections, unsupported ? String() : hit_code.uniforms, String(), globals, globals, String(), String(), defines);
}

RID SceneShaderForwardClustered::ShaderData::get_hit_shader() const {
	return hit_version.is_valid() ? SceneShaderForwardClustered::singleton->hit_shader.version_get_shader(hit_version, 0) : RID();
}

void SceneShaderForwardClustered::ShaderData::set_code(const String &p_code) {
	//compile

	code = p_code;
	_compile_hit_code(String());
	ubo_size = 0;
	uniforms.clear();
	_clear_vertex_input_mask_cache();

	if (code.is_empty()) {
		return; //just invalid, but no error
	}

	ShaderCompiler::GeneratedCode gen_code;

	blend_mode = BLEND_MODE_MIX;
	depth_test_disabledi = 0;
	depth_test_invertedi = 0;
	alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;
	int cull_modei = RSE::CULL_MODE_BACK;

	uses_point_size = false;
	uses_alpha = false;
	uses_alpha_clip = false;
	uses_alpha_antialiasing = false;
	uses_blend_alpha = false;
	uses_depth_prepass_alpha = false;
	uses_discard = false;
	uses_roughness = false;
	uses_normal = false;
	uses_tangent = false;
	writes_tangent = false;
	uses_normal_map = false;
	uses_bent_normal_map = false;
	uses_emission = false;
	uses_rim = false;
	uses_clearcoat = false;
	uses_anisotropy = false;
	uses_backlight = false;
	uses_custom_radiance = false;
	uses_custom_irradiance = false;
	uses_custom_light = false;
	uses_light_vertex = false;
	uses_unsupported_shading_mode = false;
	rtxdi_surface_unsupported = false;
	wireframe = false;

	unshaded = false;
	uses_vertex = false;
	uses_position = false;
	writes_depth = false;
	uses_sss = false;
	uses_transmittance = false;
	uses_time = false;
	uses_previous_time = false;
	writes_modelview_or_projection = false;
	uses_world_coordinates = false;
	uses_particle_trails = false;
	uses_z_clip_scale = false;

	int depth_drawi = DEPTH_DRAW_OPAQUE;

	int stencil_readi = 0;
	int stencil_writei = 0;
	int stencil_write_depth_faili = 0;
	int stencil_comparei = STENCIL_COMPARE_ALWAYS;
	int stencil_referencei = -1;

	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;

	actions.render_mode_values["blend_add"] = Pair<int *, int>(&blend_mode, BLEND_MODE_ADD);
	actions.render_mode_values["blend_mix"] = Pair<int *, int>(&blend_mode, BLEND_MODE_MIX);
	actions.render_mode_values["blend_sub"] = Pair<int *, int>(&blend_mode, BLEND_MODE_SUB);
	actions.render_mode_values["blend_mul"] = Pair<int *, int>(&blend_mode, BLEND_MODE_MUL);
	actions.render_mode_values["blend_premul_alpha"] = Pair<int *, int>(&blend_mode, BLEND_MODE_PREMULTIPLIED_ALPHA);

	actions.render_mode_values["alpha_to_coverage"] = Pair<int *, int>(&alpha_antialiasing_mode, ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
	actions.render_mode_values["alpha_to_coverage_and_one"] = Pair<int *, int>(&alpha_antialiasing_mode, ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE);

	actions.render_mode_values["depth_draw_never"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_DISABLED);
	actions.render_mode_values["depth_draw_opaque"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_OPAQUE);
	actions.render_mode_values["depth_draw_always"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_ALWAYS);

	actions.render_mode_values["depth_test_disabled"] = Pair<int *, int>(&depth_test_disabledi, 1);
	actions.render_mode_values["depth_test_inverted"] = Pair<int *, int>(&depth_test_invertedi, 1);

	actions.render_mode_values["cull_disabled"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_DISABLED);
	actions.render_mode_values["cull_front"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_FRONT);
	actions.render_mode_values["cull_back"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_BACK);

	actions.render_mode_flags["unshaded"] = &unshaded;
	actions.render_mode_flags["wireframe"] = &wireframe;
	actions.render_mode_flags["particle_trails"] = &uses_particle_trails;
	actions.render_mode_flags["world_vertex_coords"] = &uses_world_coordinates;
	actions.render_mode_flags["diffuse_lambert_wrap"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["diffuse_toon"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["specular_toon"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["specular_disabled"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["vertex_lighting"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["shadows_disabled"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["ambient_light_disabled"] = &uses_unsupported_shading_mode;
	actions.render_mode_flags["shadow_to_opacity"] = &uses_unsupported_shading_mode;

	actions.usage_flag_pointers["ALPHA"] = &uses_alpha;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &uses_alpha_clip;
	actions.usage_flag_pointers["ALPHA_HASH_SCALE"] = &uses_alpha_clip;
	actions.usage_flag_pointers["ALPHA_ANTIALIASING_EDGE"] = &uses_alpha_antialiasing;
	actions.usage_flag_pointers["ALPHA_TEXTURE_COORDINATE"] = &uses_alpha_antialiasing;
	actions.render_mode_flags["depth_prepass_alpha"] = &uses_depth_prepass_alpha;

	actions.usage_flag_pointers["SSS_STRENGTH"] = &uses_sss;
	actions.usage_flag_pointers["SSS_TRANSMITTANCE_DEPTH"] = &uses_transmittance;

	actions.usage_flag_pointers["DISCARD"] = &uses_discard;
	actions.usage_flag_pointers["TIME"] = &uses_time;
	actions.usage_flag_pointers["PREV_TIME"] = &uses_previous_time;
	actions.usage_flag_pointers["ROUGHNESS"] = &uses_roughness;
	actions.usage_flag_pointers["NORMAL"] = &uses_normal;
	actions.usage_flag_pointers["NORMAL_MAP"] = &uses_normal_map;
	actions.usage_flag_pointers["BENT_NORMAL_MAP"] = &uses_bent_normal_map;
	actions.usage_flag_pointers["EMISSION"] = &uses_emission;
	actions.usage_flag_pointers["RIM"] = &uses_rim;
	actions.usage_flag_pointers["CLEARCOAT"] = &uses_clearcoat;
	actions.usage_flag_pointers["ANISOTROPY"] = &uses_anisotropy;
	actions.usage_flag_pointers["BACKLIGHT"] = &uses_backlight;
	actions.usage_flag_pointers["RADIANCE"] = &uses_custom_radiance;
	actions.usage_flag_pointers["IRRADIANCE"] = &uses_custom_irradiance;
	actions.usage_flag_pointers["LIGHT_VERTEX"] = &uses_light_vertex;

	actions.usage_flag_pointers["POINT_SIZE"] = &uses_point_size;
	actions.usage_flag_pointers["POINT_COORD"] = &uses_point_size;

	actions.usage_flag_pointers["TANGENT"] = &uses_tangent;
	actions.usage_flag_pointers["BINORMAL"] = &uses_tangent;
	actions.usage_flag_pointers["ANISOTROPY_FLOW"] = &uses_anisotropy;

	actions.write_flag_pointers["MODELVIEW_MATRIX"] = &writes_modelview_or_projection;
	actions.write_flag_pointers["PROJECTION_MATRIX"] = &writes_modelview_or_projection;
	actions.write_flag_pointers["VERTEX"] = &uses_vertex;
	actions.write_flag_pointers["POSITION"] = &uses_position;
	actions.write_flag_pointers["DEPTH"] = &writes_depth;
	actions.write_flag_pointers["TANGENT"] = &writes_tangent;
	actions.write_flag_pointers["BINORMAL"] = &writes_tangent;
	actions.write_flag_pointers["Z_CLIP_SCALE"] = &uses_z_clip_scale;

	actions.stencil_mode_values["read"] = Pair<int *, int>(&stencil_readi, STENCIL_FLAG_READ);
	actions.stencil_mode_values["write"] = Pair<int *, int>(&stencil_writei, STENCIL_FLAG_WRITE);
	actions.stencil_mode_values["write_depth_fail"] = Pair<int *, int>(&stencil_write_depth_faili, STENCIL_FLAG_WRITE_DEPTH_FAIL);

	actions.stencil_mode_values["compare_less"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_LESS);
	actions.stencil_mode_values["compare_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_EQUAL);
	actions.stencil_mode_values["compare_less_or_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_LESS_OR_EQUAL);
	actions.stencil_mode_values["compare_greater"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_GREATER);
	actions.stencil_mode_values["compare_not_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_NOT_EQUAL);
	actions.stencil_mode_values["compare_greater_or_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_GREATER_OR_EQUAL);
	actions.stencil_mode_values["compare_always"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_ALWAYS);

	actions.stencil_reference = &stencil_referencei;

	actions.uniforms = &uniforms;

	Error err = OK;
	{
		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		err = SceneShaderForwardClustered::singleton->compiler.compile(RSE::SHADER_SPATIAL, code, &actions, path, gen_code);
	}

	if (err != OK) {
		if (version.is_valid()) {
			SceneShaderForwardClustered::singleton->shader.version_free(version);
			version = RID();
		}
		ERR_FAIL_MSG("Shader compilation failed.");
	}

	if (version.is_null()) {
		version = SceneShaderForwardClustered::singleton->shader.version_create(false);
	}

	depth_draw = DepthDraw(depth_drawi);
	if (depth_test_disabledi) {
		depth_test = DEPTH_TEST_DISABLED;
	} else if (depth_test_invertedi) {
		depth_test = DEPTH_TEST_ENABLED_INVERTED;
	} else {
		depth_test = DEPTH_TEST_ENABLED;
	}
	cull_mode = RSE::CullMode(cull_modei);
	uses_screen_texture_mipmaps = gen_code.uses_screen_texture_mipmaps;
	uses_screen_texture = gen_code.uses_screen_texture;
	uses_depth_texture = gen_code.uses_depth_texture;
	uses_normal_texture = gen_code.uses_normal_roughness_texture;
	uses_vertex_time = gen_code.uses_vertex_time;
	uses_fragment_time = gen_code.uses_fragment_time;
	uses_custom_light = gen_code.code.has("light");
	uses_normal |= uses_normal_map;
	uses_normal |= uses_bent_normal_map;
	uses_tangent |= uses_normal_map;
	uses_tangent |= uses_bent_normal_map;
	uses_tangent |= uses_anisotropy;
	rtxdi_surface_unsupported = uses_custom_light || uses_light_vertex || uses_unsupported_shading_mode || uses_rim || uses_clearcoat || uses_anisotropy || uses_backlight || uses_sss || uses_transmittance || uses_custom_radiance || uses_custom_irradiance || unshaded || uses_screen_texture || uses_depth_texture || uses_normal_texture || uses_alpha_antialiasing || uses_world_coordinates || uses_vertex || uses_position || writes_modelview_or_projection;
	if (rtxdi_surface_unsupported) {
		WARN_PRINT(vformat("Forward Clustered RTXDI surface: shader %s uses shading or deformation that the RTXDI material contract cannot represent; affected surfaces render as a magenta diagnostic.", path.is_empty() ? String("<inline>") : path));
	}

	stencil_enabled = stencil_referencei != -1;
	stencil_flags = stencil_readi | stencil_writei | stencil_write_depth_faili;
	stencil_compare = StencilCompare(stencil_comparei);
	stencil_reference = stencil_referencei;

#if 0
	print_line("**compiling shader:");
	print_line("**defines:\n");
	for (int i = 0; i < gen_code.defines.size(); i++) {
		print_line(gen_code.defines[i]);
	}

	HashMap<String, String>::Iterator el = gen_code.code.begin();
	while (el) {
		print_line("\n**code " + el->key + ":\n" + el->value);
		++el;
	}

	print_line("\n**uniforms:\n" + gen_code.uniforms);
	print_line("\n**vertex_globals:\n" + gen_code.stage_globals[ShaderCompiler::STAGE_VERTEX]);
	print_line("\n**fragment_globals:\n" + gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT]);
#endif
	pipeline_hash_map.clear_pipelines();

	SceneShaderForwardClustered::singleton->shader.version_set_code(version, gen_code.code, gen_code.uniforms, gen_code.stage_globals[ShaderCompiler::STAGE_VERTEX], gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT], gen_code.defines);

	ubo_size = gen_code.uniform_total_size;
	ubo_offsets = gen_code.uniform_offsets;
	texture_uniforms = gen_code.texture_uniforms;

	// If any form of Alpha Antialiasing is enabled, set the blend mode to alpha to coverage.
	if (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF) {
		blend_mode = BLEND_MODE_ALPHA_TO_COVERAGE;
	}

	uses_blend_alpha = blend_mode_uses_blend_alpha(BlendMode(blend_mode));
	_compile_hit_code(code);
}

void SceneShaderForwardClustered::ShaderData::set_code_rt(const String &p_code_rt) {
	// No `#if defined(RT)` divergence
	if (p_code_rt.is_empty() || p_code_rt == code) {
		if (rt) {
			memdelete(rt);
			rt = nullptr;
		}
		_compile_hit_code(code);
		return;
	}

	// Flag-extraction-only pass; raster pipeline state is untouched.
	int blend_modei = BLEND_MODE_MIX;
	int depth_drawi = DEPTH_DRAW_OPAQUE;
	int depth_test_disabledi_local = 0;
	int depth_test_invertedi_local = 0;
	int alpha_antialiasing_modei = ALPHA_ANTIALIASING_OFF;
	int cull_modei = RSE::CULL_MODE_BACK;

	bool local_uses_alpha = false;
	bool local_uses_alpha_clip = false;
	bool local_uses_alpha_antialiasing = false;
	bool local_uses_depth_prepass_alpha = false;
	bool local_uses_time = false;
	bool local_uses_previous_time = false;

	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;

	actions.render_mode_values["blend_add"] = Pair<int *, int>(&blend_modei, BLEND_MODE_ADD);
	actions.render_mode_values["blend_mix"] = Pair<int *, int>(&blend_modei, BLEND_MODE_MIX);
	actions.render_mode_values["blend_sub"] = Pair<int *, int>(&blend_modei, BLEND_MODE_SUB);
	actions.render_mode_values["blend_mul"] = Pair<int *, int>(&blend_modei, BLEND_MODE_MUL);
	actions.render_mode_values["blend_premul_alpha"] = Pair<int *, int>(&blend_modei, BLEND_MODE_PREMULTIPLIED_ALPHA);

	actions.render_mode_values["alpha_to_coverage"] = Pair<int *, int>(&alpha_antialiasing_modei, ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
	actions.render_mode_values["alpha_to_coverage_and_one"] = Pair<int *, int>(&alpha_antialiasing_modei, ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE);

	actions.render_mode_values["depth_draw_never"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_DISABLED);
	actions.render_mode_values["depth_draw_opaque"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_OPAQUE);
	actions.render_mode_values["depth_draw_always"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_ALWAYS);

	actions.render_mode_values["depth_test_disabled"] = Pair<int *, int>(&depth_test_disabledi_local, 1);
	actions.render_mode_values["depth_test_inverted"] = Pair<int *, int>(&depth_test_invertedi_local, 1);

	actions.render_mode_values["cull_disabled"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_DISABLED);
	actions.render_mode_values["cull_front"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_FRONT);
	actions.render_mode_values["cull_back"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_BACK);

	actions.render_mode_flags["depth_prepass_alpha"] = &local_uses_depth_prepass_alpha;

	actions.usage_flag_pointers["ALPHA"] = &local_uses_alpha;
	actions.usage_flag_pointers["TIME"] = &local_uses_time;
	actions.usage_flag_pointers["PREV_TIME"] = &local_uses_previous_time;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &local_uses_alpha_clip;
	actions.usage_flag_pointers["ALPHA_HASH_SCALE"] = &local_uses_alpha_clip;
	actions.usage_flag_pointers["ALPHA_ANTIALIASING_EDGE"] = &local_uses_alpha_antialiasing;
	actions.usage_flag_pointers["ALPHA_TEXTURE_COORDINATE"] = &local_uses_alpha_antialiasing;

	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> rt_uniform_sink;
	actions.uniforms = &rt_uniform_sink;

	ShaderCompiler::GeneratedCode rt_gen_code;
	Error err = OK;
	{
		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		err = SceneShaderForwardClustered::singleton->compiler.compile(RSE::SHADER_SPATIAL, p_code_rt, &actions, path, rt_gen_code);
	}

	if (err != OK) {
		// RT compile failed: drop any prior divergence -> fall back to raster.
		if (rt) {
			memdelete(rt);
			rt = nullptr;
		}
		WARN_PRINT(vformat("Forward Clustered: RT-variant classification compile failed for shader %s. Falling back to raster-side flags for ray-tracing routing.", path.is_empty() ? String("<inline>") : path));
		return;
	}

	if (alpha_antialiasing_modei != ALPHA_ANTIALIASING_OFF) {
		blend_modei = BLEND_MODE_ALPHA_TO_COVERAGE;
	}

	if (!rt) {
		rt = memnew(RTClassification);
	}
	rt->code = p_code_rt;
	rt->uniforms = rt_uniform_sink;
	rt->uniform_offsets = rt_gen_code.uniform_offsets;
	rt->texture_uniforms = rt_gen_code.texture_uniforms;
	rt->uniform_total_size = rt_gen_code.uniform_total_size;
	rt->blend_mode = blend_modei;
	rt->alpha_antialiasing_mode = alpha_antialiasing_modei;
	rt->depth_draw = DepthDraw(depth_drawi);
	rt->cull_mode = RSE::CullMode(cull_modei);
	if (depth_test_disabledi_local) {
		rt->depth_test = DEPTH_TEST_DISABLED;
	} else if (depth_test_invertedi_local) {
		rt->depth_test = DEPTH_TEST_ENABLED_INVERTED;
	} else {
		rt->depth_test = DEPTH_TEST_ENABLED;
	}

	rt->uses_alpha = local_uses_alpha;
	rt->uses_time = local_uses_time;
	rt->uses_previous_time = local_uses_previous_time;
	rt->uses_alpha_clip = local_uses_alpha_clip;
	rt->uses_alpha_antialiasing = local_uses_alpha_antialiasing;
	rt->uses_depth_prepass_alpha = local_uses_depth_prepass_alpha;
	rt->uses_blend_alpha = blend_mode_uses_blend_alpha(BlendMode(rt->blend_mode));

	rt->uses_screen_texture = rt_gen_code.uses_screen_texture;
	rt->uses_depth_texture = rt_gen_code.uses_depth_texture;
	rt->uses_normal_texture = rt_gen_code.uses_normal_roughness_texture;
	_compile_hit_code(p_code_rt);
}

bool SceneShaderForwardClustered::ShaderData::is_animated() const {
	return (uses_fragment_time && uses_discard) || (uses_vertex_time && uses_vertex);
}

bool SceneShaderForwardClustered::ShaderData::casts_shadows() const {
	bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
	bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
	bool has_alpha = has_base_alpha || uses_blend_alpha;

	return !has_alpha || (uses_depth_prepass_alpha && !(depth_draw == DEPTH_DRAW_DISABLED || depth_test != DEPTH_TEST_ENABLED));
}

RenderingServerTypes::ShaderNativeSourceCode SceneShaderForwardClustered::ShaderData::get_native_source_code() const {
	if (version.is_valid()) {
		return SceneShaderForwardClustered::singleton->shader.version_get_native_source_code(version);
	} else {
		return RenderingServerTypes::ShaderNativeSourceCode();
	}
}

Pair<ShaderRD *, RID> SceneShaderForwardClustered::ShaderData::get_native_shader_and_version() const {
	if (version.is_valid()) {
		return { &SceneShaderForwardClustered::singleton->shader, version };
	} else {
		return {};
	}
}

uint16_t SceneShaderForwardClustered::ShaderData::_get_shader_version(PipelineVersion p_pipeline_version, bool p_ubershader, bool p_micro_geometry) const {
	uint32_t ubershader_base = (uint32_t(p_ubershader) + 2 * uint32_t(p_micro_geometry)) * ShaderVersion::SHADER_VERSION_COUNT;
	switch (p_pipeline_version) {
		case PIPELINE_VERSION_DEPTH_PASS:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_DP:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_DP + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MATERIAL + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_SDF:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_SDF + ubershader_base;
		case PIPELINE_VERSION_RTXDI_SURFACE:
			return ShaderVersion::SHADER_VERSION_RTXDI_SURFACE + ubershader_base;

		default: {
			DEV_ASSERT(false && "Unknown pipeline version.");
			return 0;
		} break;
	}
}

void SceneShaderForwardClustered::ShaderData::_create_pipeline(PipelineKey p_pipeline_key) {
#if PRINT_PIPELINE_COMPILATION_KEYS
	print_line(
			"HASH:", p_pipeline_key.hash(),
			"VERSION:", version,
			"VERTEX:", p_pipeline_key.vertex_format_id,
			"FRAMEBUFFER:", p_pipeline_key.framebuffer_format_id,
			"CULL:", p_pipeline_key.cull_mode,
			"PRIMITIVE:", p_pipeline_key.primitive_type,
			"VERSION:", p_pipeline_key.version,
			"SPEC PACKED #0:", p_pipeline_key.shader_specialization.packed_0,
			"WIREFRAME:", p_pipeline_key.wireframe);
#endif

	RD::PipelineColorBlendState blend_state_depth_normal_roughness = RD::PipelineColorBlendState::create_disabled(1);
	RD::PipelineColorBlendState blend_state_depth_normal_roughness_giprobe = RD::PipelineColorBlendState::create_disabled(2);
	RD::PipelineColorBlendState blend_state_rtxdi_surface = RD::PipelineColorBlendState::create_disabled(6);

	RD::PipelineDepthStencilState depth_stencil_state;

	if (depth_test != DEPTH_TEST_DISABLED) {
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.enable_depth_write = depth_draw != DEPTH_DRAW_DISABLED ? true : false;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;

		if (depth_test == DEPTH_TEST_ENABLED_INVERTED) {
			depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_LESS;
		}
	}

	RD::RenderPrimitive primitive_rd_table[RSE::PRIMITIVE_MAX] = {
		RD::RENDER_PRIMITIVE_POINTS,
		RD::RENDER_PRIMITIVE_LINES,
		RD::RENDER_PRIMITIVE_LINESTRIPS,
		RD::RENDER_PRIMITIVE_TRIANGLES,
		RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
	};

	bool emulate_point_size_flag = uses_point_size && SceneShaderForwardClustered::singleton->emulate_point_size;

	RD::RenderPrimitive primitive_rd;
	if (uses_point_size) {
		primitive_rd = emulate_point_size_flag ? RD::RENDER_PRIMITIVE_TRIANGLES : RD::RENDER_PRIMITIVE_POINTS;
	} else {
		primitive_rd = primitive_rd_table[p_pipeline_key.primitive_type];
	}

	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = p_pipeline_key.cull_mode;
	raster_state.wireframe = wireframe || p_pipeline_key.wireframe;

	RD::PipelineMultisampleState multisample_state;
	multisample_state.sample_count = RD::get_singleton()->framebuffer_format_get_texture_samples(p_pipeline_key.framebuffer_format_id, 0);

	RD::PipelineColorBlendState blend_state;
	switch (p_pipeline_key.version) {
		case PIPELINE_VERSION_RTXDI_SURFACE:
			blend_state = blend_state_rtxdi_surface;
			break;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS:
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW:
			blend_state = blend_state_depth_normal_roughness;
			break;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI:
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW:
			blend_state = blend_state_depth_normal_roughness_giprobe;
			break;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL:
			// Writes to normal and roughness in opaque way.
			blend_state = RD::PipelineColorBlendState::create_disabled(5);
			break;
		case PIPELINE_VERSION_DEPTH_PASS:
		case PIPELINE_VERSION_DEPTH_PASS_DP:
		case PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW:
		case PIPELINE_VERSION_DEPTH_PASS_WITH_SDF:
		default:
			break;
	}
	// Convert the specialization from the key to pipeline specialization constants.
	Vector<RD::PipelineSpecializationConstant> specialization_constants;
	RD::PipelineSpecializationConstant sc;
	sc.constant_id = 0;
	sc.int_value = p_pipeline_key.shader_specialization.packed_0;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
	specialization_constants.push_back(sc);

	sc.constant_id = 1;
	sc.int_value = p_pipeline_key.shader_specialization.packed_1;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
	specialization_constants.push_back(sc);

	sc = {}; // Sanitize value bits. "bool_value" only assigns 8 bits and keeps the remaining bits intact.
	sc.constant_id = 2;
	sc.bool_value = emulate_point_size_flag;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL;
	specialization_constants.push_back(sc);

	RID shader_rid = get_shader_variant(p_pipeline_key.version, p_pipeline_key.ubershader, p_pipeline_key.micro_geometry);
	ERR_FAIL_COND(shader_rid.is_null());

	RID pipeline = RD::get_singleton()->render_pipeline_create(shader_rid, p_pipeline_key.framebuffer_format_id, p_pipeline_key.vertex_format_id, primitive_rd, raster_state, multisample_state, depth_stencil_state, blend_state, 0, 0, specialization_constants);
	ERR_FAIL_COND(pipeline.is_null());

	pipeline_hash_map.add_compiled_pipeline(p_pipeline_key.hash(), pipeline);
}

RD::PolygonCullMode SceneShaderForwardClustered::ShaderData::get_cull_mode_from_cull_variant(CullVariant p_cull_variant) {
	const RD::PolygonCullMode cull_mode_rd_table[CULL_VARIANT_MAX][3] = {
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_FRONT, RD::POLYGON_CULL_BACK },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_BACK, RD::POLYGON_CULL_FRONT },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED }
	};

	return cull_mode_rd_table[p_cull_variant][cull_mode];
}

RID SceneShaderForwardClustered::ShaderData::_get_shader_variant(uint16_t p_shader_version) const {
	if (version.is_valid()) {
		ERR_FAIL_NULL_V(SceneShaderForwardClustered::singleton, RID());
		return SceneShaderForwardClustered::singleton->shader.version_get_shader(version, p_shader_version);
	} else {
		return RID();
	}
}

void SceneShaderForwardClustered::ShaderData::_clear_vertex_input_mask_cache() {
	for (uint32_t i = 0; i < VERTEX_INPUT_MASKS_SIZE; i++) {
		vertex_input_masks[i].store(0);
	}
}

RID SceneShaderForwardClustered::ShaderData::get_shader_variant(PipelineVersion p_pipeline_version, bool p_ubershader, bool p_micro_geometry) const {
	return _get_shader_variant(_get_shader_version(p_pipeline_version, p_ubershader, p_micro_geometry));
}

uint64_t SceneShaderForwardClustered::ShaderData::get_vertex_input_mask(PipelineVersion p_pipeline_version, bool p_ubershader, bool p_micro_geometry) {
	if (p_micro_geometry) {
		return 0;
	}
	// Vertex input masks require knowledge of the shader. Since querying the shader can be expensive due to high contention and the necessary mutex, we cache the result instead.
	uint16_t shader_version = _get_shader_version(p_pipeline_version, p_ubershader, p_micro_geometry);
	uint64_t input_mask = vertex_input_masks[shader_version].load(std::memory_order_relaxed);
	if (input_mask == 0) {
		RID shader_rid = _get_shader_variant(shader_version);
		ERR_FAIL_COND_V(shader_rid.is_null(), 0);

		input_mask = RD::get_singleton()->shader_get_vertex_input_attribute_mask(shader_rid);
		vertex_input_masks[shader_version].store(input_mask, std::memory_order_relaxed);
	}

	return input_mask;
}

bool SceneShaderForwardClustered::ShaderData::is_valid() const {
	if (version.is_valid()) {
		ERR_FAIL_NULL_V(SceneShaderForwardClustered::singleton, false);
		return SceneShaderForwardClustered::singleton->shader.version_is_valid(version);
	} else {
		return false;
	}
}

SceneShaderForwardClustered::ShaderData::ShaderData() :
		shader_list_element(this) {
	pipeline_hash_map.set_creation_object_and_function(this, &ShaderData::_create_pipeline);
	pipeline_hash_map.set_compilations(SceneShaderForwardClustered::singleton->pipeline_compilations, &SceneShaderForwardClustered::singleton_mutex);
}

SceneShaderForwardClustered::ShaderData::~ShaderData() {
	if (hit_version.is_valid()) {
		SceneShaderForwardClustered::singleton->hit_shader.version_free(hit_version);
	}
	pipeline_hash_map.clear_pipelines();

	if (version.is_valid()) {
		ERR_FAIL_NULL(SceneShaderForwardClustered::singleton);
		SceneShaderForwardClustered::singleton->shader.version_free(version);
	}

	if (rt) {
		memdelete(rt);
		rt = nullptr;
	}
}

RendererRD::MaterialStorage::ShaderData *SceneShaderForwardClustered::_create_shader_func() {
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	ShaderData *shader_data = memnew(ShaderData);
	singleton->shader_list.add(&shader_data->shader_list_element);
	return shader_data;
}

void SceneShaderForwardClustered::MaterialData::set_render_priority(int p_priority) {
	priority = p_priority - RSE::MATERIAL_RENDER_PRIORITY_MIN; //8 bits
}

void SceneShaderForwardClustered::MaterialData::set_next_pass(RID p_pass) {
	next_pass = p_pass;
}

bool SceneShaderForwardClustered::MaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	if (shader_data->version.is_valid()) {
		RID shader_rid = SceneShaderForwardClustered::singleton->shader.version_get_shader(shader_data->version, 0);

		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		return update_parameters_uniform_set(p_parameters, p_uniform_dirty, p_textures_dirty, shader_data->uniforms, shader_data->ubo_offsets.ptr(), shader_data->texture_uniforms, shader_data->default_texture_params, shader_data->ubo_size, uniform_set, shader_rid, RenderForwardClustered::MATERIAL_UNIFORM_SET, true, true);
	} else {
		return false;
	}
}

SceneShaderForwardClustered::MaterialData::~MaterialData() {
	free_parameters_uniform_set(uniform_set);
}

RendererRD::MaterialStorage::MaterialData *SceneShaderForwardClustered::_create_material_func(ShaderData *p_shader) {
	MaterialData *material_data = memnew(MaterialData);
	material_data->shader_data = p_shader;
	//update will happen later anyway so do nothing.
	return material_data;
}

SceneShaderForwardClustered *SceneShaderForwardClustered::singleton = nullptr;
Mutex SceneShaderForwardClustered::singleton_mutex;

SceneShaderForwardClustered::SceneShaderForwardClustered() {
	// there should be only one of these, contained within our RenderFM singleton.
	singleton = this;
}

SceneShaderForwardClustered::~SceneShaderForwardClustered() {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	RD::get_singleton()->free_rid(default_vec4_xform_buffer);
	RD::get_singleton()->free_rid(shadow_sampler);

	material_storage->shader_free(overdraw_material_shader);
	material_storage->shader_free(default_shader);
	material_storage->shader_free(debug_shadow_splits_material_shader);

	material_storage->material_free(overdraw_material);
	material_storage->material_free(default_material);
	material_storage->material_free(debug_shadow_splits_material);
}

void SceneShaderForwardClustered::init(const String p_defines) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	emulate_point_size = !RD::get_singleton()->has_feature(RD::SUPPORTS_POINT_SIZE);

	{
		Vector<ShaderRD::VariantDefine> shader_versions;
		for (uint32_t variant = 0; variant < 4; variant++) {
			String base_define = (variant & 1) ? "\n#define UBERSHADER\n" : "";
			if (variant & 2) {
				base_define += "\n#define MICRO_GEOMETRY_RASTER\n";
			}
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n", true)); // SHADER_VERSION_DEPTH_PASS
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_DUAL_PARABOLOID\n", true)); // SHADER_VERSION_DEPTH_PASS_DP
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n", true)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n", false)); // SHADER_VERSION_DEPTH_PASS_MULTIVIEW
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_MATERIAL\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_MATERIAL
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_SDF\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_SDF
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RTXDI_SURFACE\n#define MOTION_VECTORS\n#define NORMAL_USED\n", true)); // SHADER_VERSION_RTXDI_SURFACE
		}

		Vector<uint64_t> dynamic_buffers;
		dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(RenderForwardClustered::RENDER_PASS_UNIFORM_SET, 2));
		shader.initialize(shader_versions, p_defines, Vector<RD::PipelineImmutableSampler>(), dynamic_buffers, true, false);

		if (RendererCompositorRD::get_singleton()->is_xr_enabled()) {
			shader.enable_group(SHADER_GROUP_MULTIVIEW);
		}
	}

	material_storage->shader_set_data_request_function(RendererRD::MaterialStorage::SHADER_TYPE_3D, _create_shader_funcs);
	material_storage->material_set_data_request_function(RendererRD::MaterialStorage::SHADER_TYPE_3D, _create_material_funcs);

	{
		//shader compiler
		ShaderCompiler::DefaultIdentifierActions actions;
		actions.target = ShaderCompiler::TARGET_SLANG;

		actions.renames["MODEL_MATRIX"] = "read_model_matrix";
		actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
		actions.renames["VIEW_MATRIX"] = "read_view_matrix";
		actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
		actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
		actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
		actions.renames["MODELVIEW_MATRIX"] = "modelview";
		actions.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
		actions.renames["MAIN_CAM_INV_VIEW_MATRIX"] = "scene_data.main_cam_inv_view_matrix";

		actions.renames["VERTEX"] = "vertex";
		actions.renames["NORMAL"] = "normal_highp";
		actions.renames["TANGENT"] = "tangent";
		actions.renames["BINORMAL"] = "binormal";
		actions.renames["POSITION"] = "position";
		actions.renames["UV"] = "uv_interp";
		actions.renames["UV2"] = "uv2_interp";
		actions.renames["COLOR"] = "color_interp";
		actions.renames["POINT_SIZE"] = "point_size";
		actions.renames["INSTANCE_ID"] = "INSTANCE_INDEX";
		actions.renames["VERTEX_ID"] = "VERTEX_INDEX";
		actions.renames["Z_CLIP_SCALE"] = "z_clip_scale";

		actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
		actions.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
		actions.renames["ALPHA_ANTIALIASING_EDGE"] = "alpha_antialiasing_edge";
		actions.renames["ALPHA_TEXTURE_COORDINATE"] = "alpha_texture_coordinate";

		//builtins

		actions.renames["TIME"] = "global_time";
		actions.renames["PREV_TIME"] = "global_prev_time";
		actions.renames["EXPOSURE"] = "(1.0 / scene_data_block.data.emissive_exposure_normalization)";
		actions.renames["PI"] = String::num(Math::PI);
		actions.renames["TAU"] = String::num(Math::TAU);
		actions.renames["E"] = String::num(Math::E);
		actions.renames["OUTPUT_IS_SRGB"] = "SHADER_IS_SRGB";
		actions.renames["CLIP_SPACE_FAR"] = "SHADER_SPACE_FAR";
		actions.renames["IN_SHADOW_PASS"] = "bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_IN_SHADOW_PASS)";
		actions.renames["VIEWPORT_SIZE"] = "read_viewport_size";

		actions.renames["FRAGCOORD"] = "fragment_coord";
		actions.renames["FRONT_FACING"] = "front_facing_builtin";
		actions.renames["NORMAL_MAP"] = "normal_map";
		actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
		actions.renames["BENT_NORMAL_MAP"] = "bent_normal_map";
		actions.renames["ALBEDO"] = "albedo_highp";
		actions.renames["ALPHA"] = "alpha_highp";
		actions.renames["PREMUL_ALPHA_FACTOR"] = "premul_alpha";
		actions.renames["METALLIC"] = "metallic_highp";
		actions.renames["SPECULAR"] = "specular";
		actions.renames["ROUGHNESS"] = "roughness_highp";
		actions.renames["RIM"] = "rim";
		actions.renames["RIM_TINT"] = "rim_tint";
		actions.renames["CLEARCOAT"] = "clearcoat";
		actions.renames["CLEARCOAT_ROUGHNESS"] = "clearcoat_roughness";
		actions.renames["ANISOTROPY"] = "anisotropy";
		actions.renames["ANISOTROPY_FLOW"] = "anisotropy_flow";
		actions.renames["SSS_STRENGTH"] = "sss_strength";
		actions.renames["SSS_TRANSMITTANCE_COLOR"] = "transmittance_color";
		actions.renames["SSS_TRANSMITTANCE_DEPTH"] = "transmittance_depth";
		actions.renames["SSS_TRANSMITTANCE_BOOST"] = "transmittance_boost";
		actions.renames["BACKLIGHT"] = "backlight";
		actions.renames["AO"] = "ao";
		actions.renames["AO_LIGHT_AFFECT"] = "ao_light_affect";
		actions.renames["EMISSION"] = "emission";
		actions.renames["POINT_COORD"] = "point_coord";
		actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
		actions.renames["SCREEN_UV"] = "screen_uv";
		actions.renames["DEPTH"] = "fragment_depth";
		actions.renames["FOG"] = "fog";
		actions.renames["RADIANCE"] = "custom_radiance";
		actions.renames["IRRADIANCE"] = "custom_irradiance";
		actions.renames["BONE_INDICES"] = "bone_attrib";
		actions.renames["BONE_WEIGHTS"] = "weight_attrib";
		actions.renames["CUSTOM0"] = "custom0_attrib";
		actions.renames["CUSTOM1"] = "custom1_attrib";
		actions.renames["CUSTOM2"] = "custom2_attrib";
		actions.renames["CUSTOM3"] = "custom3_attrib";
		actions.renames["LIGHT_VERTEX"] = "light_vertex";

		actions.renames["NODE_POSITION_WORLD"] = "read_model_matrix[3].xyz";
		actions.renames["CAMERA_POSITION_WORLD"] = "inv_view_matrix[3].xyz";
		actions.renames["CAMERA_DIRECTION_WORLD"] = "inv_view_matrix[2].xyz";
		actions.renames["CAMERA_VISIBLE_LAYERS"] = "scene_data.camera_visible_layers";
		actions.renames["NODE_POSITION_VIEW"] = "mul(read_model_matrix, read_view_matrix)[3].xyz";

		actions.renames["IS_MULTIVIEW"] = "OUTPUT_IS_MULTIVIEW";
		actions.renames["VIEW_INDEX"] = "ViewIndex";
		actions.renames["VIEW_MONO_LEFT"] = "0";
		actions.renames["VIEW_RIGHT"] = "1";
		actions.renames["EYE_OFFSET"] = "eye_offset";

		//for light
		actions.renames["VIEW"] = "view_highp";
		actions.renames["SPECULAR_AMOUNT"] = "specular_amount_highp";
		actions.renames["LIGHT_COLOR"] = "light_color_highp";
		actions.renames["LIGHT_IS_DIRECTIONAL"] = "is_directional";
		actions.renames["LIGHT_IS_AREA"] = "is_area";
		actions.renames["LIGHT_AREA_DIFFUSE_MULTIPLIER"] = "area_diffuse";
		actions.renames["LIGHT_AREA_SPECULAR_MULTIPLIER"] = "area_specular";
		actions.renames["LIGHT"] = "light_highp";
		actions.renames["ATTENUATION"] = "attenuation_highp";
		actions.renames["DIFFUSE_LIGHT"] = "diffuse_light_highp";
		actions.renames["SPECULAR_LIGHT"] = "specular_light_highp";

		actions.usage_defines["DEPTH"] = "#define DEPTH_USED\n";
		actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
		actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
		actions.usage_defines["BINORMAL"] = "@TANGENT";
		actions.usage_defines["RIM"] = "#define LIGHT_RIM_USED\n";
		actions.usage_defines["RIM_TINT"] = "@RIM";
		actions.usage_defines["CLEARCOAT"] = "#define LIGHT_CLEARCOAT_USED\n";
		actions.usage_defines["CLEARCOAT_ROUGHNESS"] = "@CLEARCOAT";
		actions.usage_defines["ANISOTROPY"] = "#define LIGHT_ANISOTROPY_USED\n";
		actions.usage_defines["ANISOTROPY_FLOW"] = "@ANISOTROPY";
		actions.usage_defines["AO"] = "#define AO_USED\n";
		actions.usage_defines["AO_LIGHT_AFFECT"] = "#define AO_USED\n";
		actions.usage_defines["UV"] = "#define UV_USED\n";
		actions.usage_defines["UV2"] = "#define UV2_USED\n";
		actions.usage_defines["BONE_INDICES"] = "#define BONES_USED\n";
		actions.usage_defines["BONE_WEIGHTS"] = "#define WEIGHTS_USED\n";
		actions.usage_defines["CUSTOM0"] = "#define CUSTOM0_USED\n";
		actions.usage_defines["CUSTOM1"] = "#define CUSTOM1_USED\n";
		actions.usage_defines["CUSTOM2"] = "#define CUSTOM2_USED\n";
		actions.usage_defines["CUSTOM3"] = "#define CUSTOM3_USED\n";
		actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
		actions.usage_defines["NORMAL_MAP_DEPTH"] = "@NORMAL_MAP";
		actions.usage_defines["BENT_NORMAL_MAP"] = "#define BENT_NORMAL_MAP_USED\n";
		actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
		actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
		actions.usage_defines["POSITION"] = "#define OVERRIDE_POSITION\n";
		actions.usage_defines["LIGHT_VERTEX"] = "#define LIGHT_VERTEX_USED\n";
		actions.usage_defines["Z_CLIP_SCALE"] = "#define Z_CLIP_SCALE_USED\n";
		actions.usage_defines["LIGHT_AREA_DIFFUSE_MULTIPLIER"] = "#define AREA_LIGHT_CODE_USED\n";
		actions.usage_defines["LIGHT_AREA_SPECULAR_MULTIPLIER"] = "@LIGHT_AREA_DIFFUSE_MULTIPLIER";
		actions.usage_defines["LIGHT_IS_AREA"] = "@LIGHT_AREA_DIFFUSE_MULTIPLIER";

		actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
		actions.usage_defines["ALPHA_HASH_SCALE"] = "#define ALPHA_HASH_USED\n";
		actions.usage_defines["ALPHA_ANTIALIASING_EDGE"] = "#define ALPHA_ANTIALIASING_EDGE_USED\n";
		actions.usage_defines["ALPHA_TEXTURE_COORDINATE"] = "@ALPHA_ANTIALIASING_EDGE";
		actions.usage_defines["PREMUL_ALPHA_FACTOR"] = "#define PREMUL_ALPHA_USED\n";

		actions.usage_defines["SSS_STRENGTH"] = "#define ENABLE_SSS\n";
		actions.usage_defines["SSS_TRANSMITTANCE_DEPTH"] = "#define ENABLE_TRANSMITTANCE\n";
		actions.usage_defines["BACKLIGHT"] = "#define LIGHT_BACKLIGHT_USED\n";
		actions.usage_defines["SCREEN_UV"] = "#define SCREEN_UV_USED\n";

		actions.usage_defines["FOG"] = "#define CUSTOM_FOG_USED\n";
		actions.usage_defines["RADIANCE"] = "#define CUSTOM_RADIANCE_USED\n";
		actions.usage_defines["IRRADIANCE"] = "#define CUSTOM_IRRADIANCE_USED\n";

		actions.usage_defines["MODEL_MATRIX"] = "#define MODEL_MATRIX_USED\n";

		actions.usage_defines["POINT_SIZE"] = "#define POINT_SIZE_USED\n";
		actions.usage_defines["POINT_COORD"] = "#define POINT_COORD_USED\n";

		actions.render_mode_defines["skip_vertex_transform"] = "#define SKIP_TRANSFORM_USED\n";
		actions.render_mode_defines["world_vertex_coords"] = "#define VERTEX_WORLD_COORDS_USED\n";
		actions.render_mode_defines["ensure_correct_normals"] = "#define ENSURE_CORRECT_NORMALS\n";
		actions.render_mode_defines["cull_front"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["cull_disabled"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["particle_trails"] = "#define USE_PARTICLE_TRAILS\n";
		actions.render_mode_defines["depth_prepass_alpha"] = "#define USE_OPAQUE_PREPASS\n";

		bool force_lambert = GLOBAL_GET("rendering/shading/overrides/force_lambert_over_burley");

		if (!force_lambert) {
			actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
		}

		actions.render_mode_defines["diffuse_lambert_wrap"] = "#define DIFFUSE_LAMBERT_WRAP\n";
		actions.render_mode_defines["diffuse_toon"] = "#define DIFFUSE_TOON\n";

		actions.render_mode_defines["sss_mode_skin"] = "#define SSS_MODE_SKIN\n";

		actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

		actions.render_mode_defines["specular_toon"] = "#define SPECULAR_TOON\n";
		actions.render_mode_defines["specular_disabled"] = "#define SPECULAR_DISABLED\n";
		actions.render_mode_defines["shadows_disabled"] = "#define SHADOWS_DISABLED\n";
		actions.render_mode_defines["ambient_light_disabled"] = "#define AMBIENT_LIGHT_DISABLED\n";
		actions.render_mode_defines["shadow_to_opacity"] = "#define USE_SHADOW_TO_OPACITY\n";
		actions.render_mode_defines["unshaded"] = "#define MODE_UNSHADED\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (!force_vertex_shading) {
			// If forcing vertex shading, this will be defined already.
			actions.render_mode_defines["vertex_lighting"] = "#define USE_VERTEX_LIGHTING\n";
		}

		actions.render_mode_defines["debug_shadow_splits"] = "#define DEBUG_DRAW_PSSM_SPLITS\n";
		actions.render_mode_defines["fog_disabled"] = "#define FOG_DISABLED\n";

		actions.render_mode_defines["specular_occlusion_disabled"] = "#define SPECULAR_OCCLUSION_DISABLED\n";

		actions.base_texture_binding_index = 1;
		actions.texture_layout_set = RenderForwardClustered::MATERIAL_UNIFORM_SET;
		actions.base_uniform_string = "material.";
		actions.base_varying_index = 16;

		actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
		actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
		actions.global_buffer_array_variable = "global_shader_uniforms";
		actions.instance_uniform_index_variable = "instances[instance_index_interp].instance_uniforms_ofs";

		actions.check_multiview_samplers = true;

		compiler.initialize(actions);
		actions.ray_hit_context = true;
		actions.suppress_varying_io = true;
		actions.check_multiview_samplers = false;
		actions.instance_uniform_index_variable = "rt_instance_uniforms_offset";
		hit_compiler.initialize(actions);
		hit_shader.initialize({ "" }, p_defines);
	}

	{
		//default material and shader
		default_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(default_shader);
		material_storage->shader_set_code(default_shader, R"(
// Default 3D material shader (Forward+).

shader_type spatial;

void vertex() {
	ROUGHNESS = 0.8;
}

void fragment() {
	ALBEDO = vec3(0.6);
	ROUGHNESS = 0.8;
	METALLIC = 0.2;
}
)");
		default_material = material_storage->material_allocate();
		material_storage->material_initialize(default_material);
		material_storage->material_set_shader(default_material, default_shader);

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		default_shader_rd = md->shader_data->get_shader_variant(PIPELINE_VERSION_RTXDI_SURFACE, false);

		default_material_shader_ptr = md->shader_data;
		default_material_uniform_set = md->uniform_set;
	}

	{
		overdraw_material_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(overdraw_material_shader);
		// Use relatively low opacity so that more "layers" of overlapping objects can be distinguished.
		material_storage->shader_set_code(overdraw_material_shader, R"(
// 3D editor Overdraw debug draw mode shader (Forward+).

shader_type spatial;

render_mode blend_add, unshaded, fog_disabled;

void fragment() {
	ALBEDO = vec3(0.4, 0.8, 0.8);
	ALPHA = 0.1;
}
)");
		overdraw_material = material_storage->material_allocate();
		material_storage->material_initialize(overdraw_material);
		material_storage->material_set_shader(overdraw_material, overdraw_material_shader);

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(overdraw_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		overdraw_material_shader_ptr = md->shader_data;
		overdraw_material_uniform_set = md->uniform_set;
	}

	{
		debug_shadow_splits_material_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(debug_shadow_splits_material_shader);
		material_storage->shader_set_code(debug_shadow_splits_material_shader, R"(
// 3D debug shadow splits mode shader (Forward+).

shader_type spatial;

render_mode debug_shadow_splits, fog_disabled;

void fragment() {
	ALBEDO = vec3(1.0, 1.0, 1.0);
}
)");
		debug_shadow_splits_material = material_storage->material_allocate();
		material_storage->material_initialize(debug_shadow_splits_material);
		material_storage->material_set_shader(debug_shadow_splits_material, debug_shadow_splits_material_shader);

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(debug_shadow_splits_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		debug_shadow_splits_material_shader_ptr = md->shader_data;
		debug_shadow_splits_material_uniform_set = md->uniform_set;
	}

	{
		default_vec4_xform_buffer = RD::get_singleton()->storage_buffer_create(256);
		Vector<RD::Uniform> uniforms;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(default_vec4_xform_buffer);
		u.binding = 0;
		uniforms.push_back(u);

		default_vec4_xform_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, default_shader_rd, RenderForwardClustered::TRANSFORMS_UNIFORM_SET);
	}
	{
		RD::SamplerState sampler;
		sampler.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler.min_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_GREATER;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
	}
}

void SceneShaderForwardClustered::set_default_specialization(const ShaderSpecialization &p_specialization) {
	default_specialization = p_specialization;

	for (SelfList<ShaderData> *E = shader_list.first(); E; E = E->next()) {
		E->self()->pipeline_hash_map.clear_pipelines();
	}
}

void SceneShaderForwardClustered::enable_multiview_shader_group() {
	shader.enable_group(SHADER_GROUP_MULTIVIEW);
}

void SceneShaderForwardClustered::enable_advanced_shader_group(bool p_needs_multiview) {
	if (p_needs_multiview || RendererCompositorRD::get_singleton()->is_xr_enabled()) {
		shader.enable_group(SHADER_GROUP_ADVANCED_MULTIVIEW);
	}
	shader.enable_group(SHADER_GROUP_ADVANCED);
}

bool SceneShaderForwardClustered::is_multiview_shader_group_enabled() const {
	return shader.is_group_enabled(SHADER_GROUP_MULTIVIEW);
}

bool SceneShaderForwardClustered::is_advanced_shader_group_enabled(bool p_multiview) const {
	if (p_multiview) {
		return shader.is_group_enabled(SHADER_GROUP_ADVANCED_MULTIVIEW);
	} else {
		return shader.is_group_enabled(SHADER_GROUP_ADVANCED);
	}
}

uint32_t SceneShaderForwardClustered::get_pipeline_compilations(RSE::PipelineSource p_source) {
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	return pipeline_compilations[p_source];
}
