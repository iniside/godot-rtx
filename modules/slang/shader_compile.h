#pragma once

#include "servers/rendering/rendering_shader_compile_request.h"

Vector<uint8_t> compile_slang_shader(RenderingDeviceCommons::ShaderStage p_stage, const String &p_source, const RenderingShaderCompileRequest &p_request, String *r_error);
String get_slang_shader_compiler_identity();
String get_slang_shader_compiler_filename();
void finalize_slang_shader_compiler();
