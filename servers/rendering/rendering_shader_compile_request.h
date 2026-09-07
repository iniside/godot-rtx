#pragma once

#include "servers/rendering/rendering_device_commons.h"

struct RenderingShaderCompileRequest {
	enum Language {
		GLSL,
		SLANG,
	};

	Language language = GLSL;
	RenderingDeviceCommons::ShaderLanguageVersion target = RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1;
	RenderingDeviceCommons::ShaderSpirvVersion spirv_version = RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_4;
	String compiler_identity;
	String source_path;
	String entry_points[RenderingDeviceCommons::SHADER_STAGE_MAX];
	HashMap<String, String> includes;
	bool column_major = true;
	bool gl_layout = true;
	bool debug_info = false;
	int optimization_level = 2;

	String get_identity() const;
};
