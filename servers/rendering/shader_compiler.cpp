/**************************************************************************/
/*  shader_compiler.cpp                                                   */
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

#include "shader_compiler.h"

#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/shader_types.h"

#define SL ShaderLanguage

static String _mktab(int p_level) {
	return String("\t").repeat(p_level);
}

String ShaderCompiler::_typestr(SL::DataType p_type) const {
	if (actions.target == TARGET_SLANG) {
		static const char *types[] = {
			"void", "bool", "bool2", "bool3", "bool4",
			"int", "int2", "int3", "int4", "uint", "uint2", "uint3", "uint4",
			"float", "float2", "float3", "float4", "float2x2", "float3x3", "float4x4",
			"Texture2D<float4>", "Texture2D<int4>", "Texture2D<uint4>",
			"Texture2DArray<float4>", "Texture2DArray<int4>", "Texture2DArray<uint4>",
			"Texture3D<float4>", "Texture3D<int4>", "Texture3D<uint4>",
			"TextureCube<float4>", "TextureCubeArray<float4>", "Texture2D<float4>",
		};
		ERR_FAIL_INDEX_V(p_type, int(sizeof(types) / sizeof(types[0])), String());
		return types[p_type];
	}
	String type = ShaderLanguage::get_datatype_name(p_type);
	if (!RS::get_singleton()->is_low_end() && ShaderLanguage::is_sampler_type(p_type)) {
		type = type.replace("sampler", "texture"); //we use textures instead of samplers in Vulkan GLSL
	}
	return type;
}

static int _get_datatype_alignment(SL::DataType p_type) {
	switch (p_type) {
		case SL::TYPE_VOID:
			return 0;
		case SL::TYPE_BOOL:
			return 4;
		case SL::TYPE_BVEC2:
			return 8;
		case SL::TYPE_BVEC3:
			return 16;
		case SL::TYPE_BVEC4:
			return 16;
		case SL::TYPE_INT:
			return 4;
		case SL::TYPE_IVEC2:
			return 8;
		case SL::TYPE_IVEC3:
			return 16;
		case SL::TYPE_IVEC4:
			return 16;
		case SL::TYPE_UINT:
			return 4;
		case SL::TYPE_UVEC2:
			return 8;
		case SL::TYPE_UVEC3:
			return 16;
		case SL::TYPE_UVEC4:
			return 16;
		case SL::TYPE_FLOAT:
			return 4;
		case SL::TYPE_VEC2:
			return 8;
		case SL::TYPE_VEC3:
			return 16;
		case SL::TYPE_VEC4:
			return 16;
		case SL::TYPE_MAT2:
			return 16;
		case SL::TYPE_MAT3:
			return 16;
		case SL::TYPE_MAT4:
			return 16;
		case SL::TYPE_SAMPLER2D:
			return 16;
		case SL::TYPE_ISAMPLER2D:
			return 16;
		case SL::TYPE_USAMPLER2D:
			return 16;
		case SL::TYPE_SAMPLER2DARRAY:
			return 16;
		case SL::TYPE_ISAMPLER2DARRAY:
			return 16;
		case SL::TYPE_USAMPLER2DARRAY:
			return 16;
		case SL::TYPE_SAMPLER3D:
			return 16;
		case SL::TYPE_ISAMPLER3D:
			return 16;
		case SL::TYPE_USAMPLER3D:
			return 16;
		case SL::TYPE_SAMPLERCUBE:
			return 16;
		case SL::TYPE_SAMPLERCUBEARRAY:
			return 16;
		case SL::TYPE_SAMPLEREXT:
			return 16;
		case SL::TYPE_STRUCT:
			return 0;
		case SL::TYPE_MAX: {
			ERR_FAIL_V(0);
		}
	}

	ERR_FAIL_V(0);
}

static String _interpstr(SL::DataInterpolation p_interp) {
	switch (p_interp) {
		case SL::INTERPOLATION_FLAT:
			return "flat ";
		case SL::INTERPOLATION_SMOOTH:
			return "";
		case SL::INTERPOLATION_DEFAULT:
			return "";
	}
	return "";
}

String ShaderCompiler::_prestr(SL::DataPrecision p_pres, bool p_force_highp) const {
	if (actions.target == TARGET_SLANG) {
		return String();
	}
	switch (p_pres) {
		case SL::PRECISION_LOWP:
			return "lowp ";
		case SL::PRECISION_MEDIUMP:
			return "mediump ";
		case SL::PRECISION_HIGHP:
			return "highp ";
		case SL::PRECISION_DEFAULT:
			return p_force_highp ? "highp " : "";
	}
	return "";
}

static String _constr(bool p_is_const) {
	if (p_is_const) {
		return "const ";
	}
	return "";
}

static String _qualstr(SL::ArgumentQualifier p_qual) {
	switch (p_qual) {
		case SL::ARGUMENT_QUALIFIER_IN:
			return "";
		case SL::ARGUMENT_QUALIFIER_OUT:
			return "out ";
		case SL::ARGUMENT_QUALIFIER_INOUT:
			return "inout ";
	}
	return "";
}

static String _opstr(SL::Operator p_op) {
	return SL::get_operator_text(p_op);
}

static String _mkid(const String &p_id) {
	String id = "m_" + p_id.replace("__", "_dus_");
	return id.replace("__", "_dus_"); //doubleunderscore is reserved in glsl
}

static String f2sp0(float p_float) {
	String num = rtos(p_float);
	if (!num.contains_char('.') && !num.contains_char('e')) {
		num += ".0";
	}
	return num;
}

String ShaderCompiler::_constant_text(SL::DataType p_type, const Vector<SL::Scalar> &p_values) const {
	switch (p_type) {
		case SL::TYPE_BOOL:
			return p_values[0].boolean ? "true" : "false";
		case SL::TYPE_BVEC2:
		case SL::TYPE_BVEC3:
		case SL::TYPE_BVEC4: {
			String text = _typestr(p_type) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += p_values[i].boolean ? "true" : "false";
			}
			text += ")";
			return text;
		}

		case SL::TYPE_INT:
			return itos(p_values[0].sint);
		case SL::TYPE_IVEC2:
		case SL::TYPE_IVEC3:
		case SL::TYPE_IVEC4: {
			String text = _typestr(p_type) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += itos(p_values[i].sint);
			}
			text += ")";
			return text;

		} break;
		case SL::TYPE_UINT:
			return itos(p_values[0].uint) + "u";
		case SL::TYPE_UVEC2:
		case SL::TYPE_UVEC3:
		case SL::TYPE_UVEC4: {
			String text = _typestr(p_type) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += itos(p_values[i].uint) + "u";
			}
			text += ")";
			return text;
		} break;
		case SL::TYPE_FLOAT:
			return f2sp0(p_values[0].real);
		case SL::TYPE_VEC2:
		case SL::TYPE_VEC3:
		case SL::TYPE_VEC4: {
			String text = _typestr(p_type) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += f2sp0(p_values[i].real);
			}
			text += ")";
			return text;

		} break;
		case SL::TYPE_MAT2:
		case SL::TYPE_MAT3:
		case SL::TYPE_MAT4: {
			String text = _typestr(p_type) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += f2sp0(p_values[i].real);
			}
			text += ")";
			return text;

		} break;
		default:
			ERR_FAIL_V(String());
	}
}

String ShaderCompiler::_get_sampler_name(ShaderLanguage::TextureFilter p_filter, ShaderLanguage::TextureRepeat p_repeat) {
	if (p_filter == ShaderLanguage::FILTER_DEFAULT) {
		ERR_FAIL_COND_V(actions.default_filter == ShaderLanguage::FILTER_DEFAULT, String());
		p_filter = actions.default_filter;
	}
	if (p_repeat == ShaderLanguage::REPEAT_DEFAULT) {
		ERR_FAIL_COND_V(actions.default_repeat == ShaderLanguage::REPEAT_DEFAULT, String());
		p_repeat = actions.default_repeat;
	}
	constexpr const char *name_mapping[] = {
		"SAMPLER_NEAREST_CLAMP",
		"SAMPLER_LINEAR_CLAMP",
		"SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP",
		"SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP",
		"SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP",
		"SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP",
		"SAMPLER_NEAREST_REPEAT",
		"SAMPLER_LINEAR_REPEAT",
		"SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT",
		"SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT",
		"SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT",
		"SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT"
	};
	return String(name_mapping[p_filter + (p_repeat == ShaderLanguage::REPEAT_ENABLE ? ShaderLanguage::FILTER_DEFAULT : 0)]);
}

void ShaderCompiler::_dump_function_deps(const SL::ShaderNode *p_node, const StringName &p_for_func, const HashMap<StringName, String> &p_func_code, String &r_to_add, HashSet<StringName> &added) {
	int fidx = -1;

	for (int i = 0; i < p_node->vfunctions.size(); i++) {
		if (p_node->vfunctions[i].name == p_for_func) {
			fidx = i;
			break;
		}
	}

	ERR_FAIL_COND(fidx == -1);

	Vector<StringName> uses_functions;

	for (const StringName &E : p_node->vfunctions[fidx].uses_function) {
		uses_functions.push_back(E);
	}
	uses_functions.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

	for (int k = 0; k < uses_functions.size(); k++) {
		if (added.has(uses_functions[k])) {
			continue; //was added already
		}

		_dump_function_deps(p_node, uses_functions[k], p_func_code, r_to_add, added);

		SL::FunctionNode *fnode = nullptr;

		for (int i = 0; i < p_node->vfunctions.size(); i++) {
			if (p_node->vfunctions[i].name == uses_functions[k]) {
				fnode = p_node->vfunctions[i].function;
				break;
			}
		}

		ERR_FAIL_NULL(fnode);

		r_to_add += "\n";

		String header;
		if (fnode->return_type == SL::TYPE_STRUCT) {
			header = _mkid(fnode->return_struct_name);
		} else {
			header = _typestr(fnode->return_type);
		}

		if (fnode->return_array_size > 0) {
			header += "[";
			header += itos(fnode->return_array_size);
			header += "]";
		}

		header += " ";
		header += _mkid(fnode->rname);
		header += "(";

		for (int i = 0; i < fnode->arguments.size(); i++) {
			if (i > 0) {
				header += ", ";
			}
			header += _constr(fnode->arguments[i].is_const);
			if (fnode->arguments[i].type == SL::TYPE_STRUCT) {
				header += _qualstr(fnode->arguments[i].qualifier) + _mkid(fnode->arguments[i].struct_name) + " " + _mkid(fnode->arguments[i].name);
			} else {
				header += _qualstr(fnode->arguments[i].qualifier) + _prestr(fnode->arguments[i].precision) + _typestr(fnode->arguments[i].type) + " " + _mkid(fnode->arguments[i].name);
			}
			if (fnode->arguments[i].array_size > 0) {
				header += "[";
				header += itos(fnode->arguments[i].array_size);
				header += "]";
			}
		}

		header += ")\n";
		r_to_add += header;
		r_to_add += p_func_code[uses_functions[k]];

		added.insert(uses_functions[k]);
	}
}

String ShaderCompiler::_global_uniform(const String &p_buffer, const String &p_index, ShaderLanguage::DataType p_type) const {
	if (actions.target == TARGET_SLANG) {
		String value = p_buffer + "[" + p_index + "]";
		if (p_type >= SL::TYPE_MAT2 && p_type <= SL::TYPE_MAT4) {
			int size = p_type - SL::TYPE_MAT2 + 2;
			String result = _typestr(p_type) + "(";
			for (int i = 0; i < size; i++) {
				if (i) {
					result += ", ";
				}
				result += p_buffer + "[" + p_index + "+" + itos(i) + "u]." + String("xyzw").substr(0, size);
			}
			return result + ")";
		}
		int size = 1;
		if (p_type >= SL::TYPE_BOOL && p_type <= SL::TYPE_VEC4) {
			size = (p_type - SL::TYPE_BOOL) % 4 + 1;
		}
		value += "." + String("xyzw").substr(0, size);
		if (p_type >= SL::TYPE_BOOL && p_type <= SL::TYPE_BVEC4) {
			return _typestr(p_type) + "(asuint(" + value + "))";
		}
		if (p_type >= SL::TYPE_INT && p_type <= SL::TYPE_IVEC4) {
			return "asint(" + value + ")";
		}
		if (p_type >= SL::TYPE_UINT && p_type <= SL::TYPE_UVEC4) {
			return "asuint(" + value + ")";
		}
		return "(" + value + ")";
	}
	switch (p_type) {
		case ShaderLanguage::TYPE_BOOL: {
			return "bool(floatBitsToUint(" + p_buffer + "[" + p_index + "].x))";
		}
		case ShaderLanguage::TYPE_BVEC2: {
			return "bvec2(floatBitsToUint(" + p_buffer + "[" + p_index + "].xy))";
		}
		case ShaderLanguage::TYPE_BVEC3: {
			return "bvec3(floatBitsToUint(" + p_buffer + "[" + p_index + "].xyz))";
		}
		case ShaderLanguage::TYPE_BVEC4: {
			return "bvec4(floatBitsToUint(" + p_buffer + "[" + p_index + "].xyzw))";
		}
		case ShaderLanguage::TYPE_INT: {
			return "floatBitsToInt(" + p_buffer + "[" + p_index + "].x)";
		}
		case ShaderLanguage::TYPE_IVEC2: {
			return "floatBitsToInt(" + p_buffer + "[" + p_index + "].xy)";
		}
		case ShaderLanguage::TYPE_IVEC3: {
			return "floatBitsToInt(" + p_buffer + "[" + p_index + "].xyz)";
		}
		case ShaderLanguage::TYPE_IVEC4: {
			return "floatBitsToInt(" + p_buffer + "[" + p_index + "].xyzw)";
		}
		case ShaderLanguage::TYPE_UINT: {
			return "floatBitsToUint(" + p_buffer + "[" + p_index + "].x)";
		}
		case ShaderLanguage::TYPE_UVEC2: {
			return "floatBitsToUint(" + p_buffer + "[" + p_index + "].xy)";
		}
		case ShaderLanguage::TYPE_UVEC3: {
			return "floatBitsToUint(" + p_buffer + "[" + p_index + "].xyz)";
		}
		case ShaderLanguage::TYPE_UVEC4: {
			return "floatBitsToUint(" + p_buffer + "[" + p_index + "].xyzw)";
		}
		case ShaderLanguage::TYPE_FLOAT: {
			return "(" + p_buffer + "[" + p_index + "].x)";
		}
		case ShaderLanguage::TYPE_VEC2: {
			return "(" + p_buffer + "[" + p_index + "].xy)";
		}
		case ShaderLanguage::TYPE_VEC3: {
			return "(" + p_buffer + "[" + p_index + "].xyz)";
		}
		case ShaderLanguage::TYPE_VEC4: {
			return "(" + p_buffer + "[" + p_index + "].xyzw)";
		}
		case ShaderLanguage::TYPE_MAT2: {
			return "mat2(" + p_buffer + "[" + p_index + "].xy," + p_buffer + "[" + p_index + "+1u].xy)";
		}
		case ShaderLanguage::TYPE_MAT3: {
			return "mat3(" + p_buffer + "[" + p_index + "].xyz," + p_buffer + "[" + p_index + "+1u].xyz," + p_buffer + "[" + p_index + "+2u].xyz)";
		}
		case ShaderLanguage::TYPE_MAT4: {
			return "mat4(" + p_buffer + "[" + p_index + "].xyzw," + p_buffer + "[" + p_index + "+1u].xyzw," + p_buffer + "[" + p_index + "+2u].xyzw," + p_buffer + "[" + p_index + "+3u].xyzw)";
		}
		default: {
			ERR_FAIL_V("void");
		}
	}
}

String ShaderCompiler::_slang_inverse(SL::DataType p_type) {
	String result_type = _typestr(p_type);
	int size = p_type - SL::TYPE_MAT2 + 2;
	String body = result_type + " result; ";
	for (int row = 0; row < size; row++) {
		for (int column = 0; column < size; column++) {
			Vector<String> minor;
			for (int i = 0; i < size; i++) {
				for (int j = 0; j < size; j++) {
					if (i != column && j != row) {
						minor.push_back("a0[" + itos(i) + "][" + itos(j) + "]");
					}
				}
			}
			String cofactor = size == 2 ? minor[0] : "determinant(float" + itos(size - 1) + "x" + itos(size - 1) + "(" + String(", ").join(minor) + "))";
			body += "result[" + itos(row) + "][" + itos(column) + "] = " + ((row + column) % 2 ? "-" : "") + cofactor + "; ";
		}
	}
	return _slang_helper(result_type, "godot_inverse", { result_type }, body + "return result / determinant(a0);");

}

String ShaderCompiler::_slang_helper(const String &p_return_type, const String &p_name, const Vector<String> &p_argument_types, const String &p_body) {
	String signature = p_return_type + " " + p_name + "(";
	for (int i = 0; i < p_argument_types.size(); i++) {
		if (i) {
			signature += ", ";
		}
		signature += p_argument_types[i] + " a" + itos(i);
	}
	signature += ")";
	if (!slang_helper_signatures.has(signature)) {
		slang_helper_signatures.insert(signature);
		slang_helpers += signature + " { " + p_body + " }\n";
	}
	return p_name;
}

String ShaderCompiler::_dump_slang_call(const SL::OperatorNode *p_node, int p_level, GeneratedCode &r_gen_code, IdentifierActions &p_actions, const DefaultIdentifierActions &p_default_actions, bool p_assigning) {
	const SL::VariableNode *callee = static_cast<const SL::VariableNode *>(p_node->arguments[0]);
	String name = callee->name;
	bool builtin = internal_functions.has(callee->name);
	const SL::FunctionNode *callee_function = nullptr;
	for (const SL::ShaderNode::Function &candidate : shader->vfunctions) {
		if (candidate.name == callee->name) {
			callee_function = candidate.function;
			break;
		}
	}
	if (p_actions.usage_flag_pointers.has(callee->name)) {
		*p_actions.usage_flag_pointers[callee->name] = true;
		used_flag_pointers.insert(callee->name);
	}
	Vector<String> arguments;
	Vector<String> types;
	for (int i = 1; i < p_node->arguments.size(); i++) {
		bool assigning = p_assigning;
		if (builtin) {
			assigning |= SL::is_builtin_func_out_parameter(callee->name, i - 1);
		} else if (callee_function) {
			assigning |= callee_function->arguments[i - 1].qualifier != SL::ARGUMENT_QUALIFIER_IN;
		}
		arguments.push_back(_dump_node_code(p_node->arguments[i], p_level, r_gen_code, p_actions, p_default_actions, assigning));
		types.push_back(p_node->arguments[i]->get_datatype() == SL::TYPE_STRUCT ? String() : _typestr(p_node->arguments[i]->get_datatype()));
	}
	String result_type = p_node->get_datatype() == SL::TYPE_STRUCT ? String() : _typestr(p_node->get_datatype());
	if (p_node->op == SL::OP_CONSTRUCT) {
		name = result_type;
		bool matrix = p_node->get_datatype() >= SL::TYPE_MAT2 && p_node->get_datatype() <= SL::TYPE_MAT4;
		if (matrix && arguments.size() == 1) {
			int size = p_node->get_datatype() - SL::TYPE_MAT2 + 2;
			SL::DataType argument_type = p_node->arguments[1]->get_datatype();
			bool source_matrix = argument_type >= SL::TYPE_MAT2 && argument_type <= SL::TYPE_MAT4;
			int source_size = source_matrix ? argument_type - SL::TYPE_MAT2 + 2 : 0;
			String body = "return " + result_type + "(";
			for (int row = 0; row < size; row++) {
				for (int column = 0; column < size; column++) {
					if (row || column) {
						body += ", ";
					}
					if (source_matrix && row < source_size && column < source_size) {
						body += "a0[" + itos(row) + "][" + itos(column) + "]";
					} else if (row == column) {
						body += source_matrix ? "1.0" : "a0";
					} else {
						body += "0.0";
					}
				}
			}
			name = _slang_helper(result_type, "godot_construct_" + result_type, types, body + ");");
		}
	} else if (p_node->op == SL::OP_STRUCT) {
		return _mkid(callee->name) + "(" + String(", ").join(arguments) + ")";
	} else if (texture_functions.has(callee->name)) {
		StringName texture_name;
		const SL::Node *texture_node = p_node->arguments[1];
		if (texture_node->type == SL::Node::NODE_TYPE_VARIABLE) {
			texture_name = static_cast<const SL::VariableNode *>(texture_node)->name;
		} else if (texture_node->type == SL::Node::NODE_TYPE_ARRAY) {
			texture_name = static_cast<const SL::ArrayNode *>(texture_node)->name;
		}
		String sampler = _get_sampler_name(SL::FILTER_DEFAULT, SL::REPEAT_DEFAULT);
		SL::ShaderNode::Uniform::Hint hint = SL::ShaderNode::Uniform::HINT_NONE;
		if (actions.custom_samplers.has(texture_name)) {
			sampler = actions.custom_samplers[texture_name];
		} else if (shader->uniforms.has(texture_name)) {
			const SL::ShaderNode::Uniform &uniform = shader->uniforms[texture_name];
			sampler = _get_sampler_name(uniform.filter, uniform.repeat);
			hint = uniform.hint;
		} else if (function) {
			for (const SL::FunctionNode::Argument &argument : function->arguments) {
				if (argument.name == texture_name) {
					if (argument.tex_builtin_check && actions.custom_samplers.has(argument.tex_builtin)) {
						sampler = actions.custom_samplers[argument.tex_builtin];
					} else if (argument.tex_argument_check) {
						sampler = _get_sampler_name(argument.tex_argument_filter, argument.tex_argument_repeat);
						hint = argument.tex_hint;
					}
					break;
				}
			}
		}
		bool screen = hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE;
		bool normal = hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE;
		bool multiview = actions.check_multiview_samplers && (screen || normal || hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE);
		bool dimensions = name == "textureSize" || name == "textureQueryLevels";
		if (name.begins_with("textureProj")) {
			int size = p_node->arguments[2]->get_datatype() - SL::TYPE_FLOAT + 1;
			int coordinate_size = texture_node->get_datatype() == SL::TYPE_SAMPLER3D ? 3 : 2;
			String coordinate_type = "float" + itos(coordinate_size);
			String project = _slang_helper(coordinate_type, "godot_project_coordinate", { types[1] }, "return a0." + String("xyz").substr(0, coordinate_size) + " / a0." + String("xyzw").substr(size - 1, 1) + ";");
			arguments.write[1] = project + "(" + arguments[1] + ")";
			types.write[1] = coordinate_type;
			name = name.replace_first("Proj", "");
		}
		String sample_coordinate = arguments.size() > 1 ? arguments[1] : String();
		if (multiview && !dimensions) {
			arguments.write[1] = "multiview_uv(" + arguments[1] + ".xy)";
		}
		String expression;
		if (dimensions) {
			int size = (texture_node->get_datatype() >= SL::TYPE_SAMPLER2DARRAY && texture_node->get_datatype() <= SL::TYPE_USAMPLER3D) || texture_node->get_datatype() == SL::TYPE_SAMPLERCUBEARRAY ? 3 : 2;
			if (multiview) {
				types.write[0] = "GodotMultiviewTexture";
			}
			String body = "uint width, height, levels; ";
			if (size == 3 || multiview) {
				body += "uint depth; ";
			}
			body += "a0.GetDimensions(" + String(name == "textureSize" ? "a1" : "0") + ", width, height, ";
			if (multiview) {
				body += "\n#ifdef USE_MULTIVIEW\ndepth,\n#endif\n";
			} else if (size == 3) {
				body += "depth, ";
			}
			body += "levels); return ";
			body += name == "textureQueryLevels" ? "int(levels);" : (size == 3 ? "int3(width, height, depth);" : "int2(width, height);");
			name = _slang_helper(result_type, "godot_" + name + (multiview ? "_multiview" : ""), types, body);
			expression = name + "(" + String(", ").join(arguments) + ")";
		} else if (name == "textureQueryLod") {
			expression = "float2(" + arguments[0] + ".CalculateLevelOfDetail(" + sampler + ", " + arguments[1] + "), " + arguments[0] + ".CalculateLevelOfDetailUnclamped(" + sampler + ", " + arguments[1] + "))";
		} else if (name == "texelFetch") {
			int size = texture_node->get_datatype() >= SL::TYPE_SAMPLER2DARRAY && texture_node->get_datatype() <= SL::TYPE_USAMPLER3D ? 4 : 3;
			expression = arguments[0] + ".Load(" + (multiview ? "godot_multiview_load_coord" : "int" + itos(size)) + "(" + arguments[1] + ", " + arguments[2] + "))";
		} else {
			String method;
			if (name == "textureGather") {
				String component = arguments.size() > 2 ? arguments[2] : "0";
				String methods[] = { "GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha" };
				for (int i = 3; i >= 0; i--) {
					String gather = arguments[0] + "." + methods[i] + "(" + sampler + ", " + arguments[1] + ")";
					expression = i == 3 ? gather : "(" + component + " == " + itos(i) + " ? " + gather + " : " + expression + ")";
				}
			} else {
				method = name == "textureGrad" ? "SampleGrad" : name == "textureLod" ? "SampleLevel" : arguments.size() > 2 ? "SampleBias" : "Sample";
				if (method == "Sample" || method == "SampleBias") {
					Vector<String> helper_types = types;
					Vector<String> helper_arguments = arguments;
					helper_types.insert(1, "SamplerState");
					helper_arguments.insert(1, sampler);
					String coordinate = "a2";
					if (multiview) {
						helper_types.write[0] = "GodotMultiviewTexture";
						helper_arguments.write[2] = sample_coordinate;
						coordinate = "multiview_uv(a2)";
					}
					String body = "\n#ifdef GODOT_VERTEX_STAGE\nreturn a0.SampleLevel(a1, " + coordinate + ", 0.0);\n#else\nreturn a0." + method + "(a1, " + coordinate;
					if (method == "SampleBias") {
						body += ", a3";
					}
					body += ");\n#endif\n";
					name = _slang_helper(result_type, "godot_" + method + (multiview ? "_multiview" : ""), helper_types, body);
					expression = name + "(" + String(", ").join(helper_arguments) + ")";
				} else {
					expression = arguments[0] + "." + method + "(" + sampler;
					for (int i = 1; i < arguments.size(); i++) {
						expression += ", " + arguments[i];
					}
					expression += ")";
				}
			}
		}
		if (normal && !dimensions && name != "textureQueryLod") {
			expression = "normal_roughness_compatibility(" + expression + ")";
		}
		return expression;
	} else if (builtin) {
		if (name == "length" && arguments.is_empty()) {
			name = "getCount";
		} else if (name == "atan" && arguments.size() == 2) {
			name = "atan2";
		} else if (name == "inverse") {
			name = _slang_inverse(p_node->get_datatype());
		} else if (name == "outerProduct") {
			int size = p_node->get_datatype() - SL::TYPE_MAT2 + 2;
			String body = "return " + result_type + "(";
			for (int i = 0; i < size; i++) {
				if (i) {
					body += ", ";
				}
				body += "a0 * a1[" + itos(i) + "]";
			}
			name = _slang_helper(result_type, "godot_outer_product", types, body + ");");
		} else if (name == "fwidthCoarse" || name == "fwidthFine") {
			String suffix = name == "fwidthCoarse" ? "_coarse" : "_fine";
			name = _slang_helper(result_type, "godot_" + name, types, "return abs(ddx" + suffix + "(a0)) + abs(ddy" + suffix + "(a0));");
		} else if (name == "bitfieldExtract") {
			name = _slang_helper(result_type, "godot_bitfield_extract", types, "return a2 == 0 ? 0 : (a0 << (32 - a1 - a2)) >> (32 - a2);");
		} else if (name == "bitfieldInsert") {
			name = _slang_helper(result_type, "godot_bitfield_insert", types, "uint mask = a3 == 32 ? 0xffffffffu : ((1u << a3) - 1u) << a2; return (a0 & ~mask) | ((a1 << a2) & mask);");
		} else if (name == "packHalf2x16") {
			name = _slang_helper(result_type, "godot_pack_half", types, "uint2 bits = f32tof16(a0); return bits.x | (bits.y << 16);");
		} else if (name == "unpackHalf2x16") {
			name = _slang_helper(result_type, "godot_unpack_half", types, "return f16tof32(uint2(a0 & 0xffffu, a0 >> 16));");
		} else if (name.begins_with("packUnorm") || name.begins_with("packSnorm")) {
			int count = name.ends_with("4x8") ? 4 : 2;
			int width = 32 / count;
			bool sign = name.begins_with("packSnorm");
			String scale = itos((1 << (width - int(sign))) - 1);
			String body = "int" + itos(count) + " bits = int" + itos(count) + "(round(clamp(a0, " + (sign ? "-1.0" : "0.0") + ", 1.0) * " + scale + ".0)); return ";
			for (int i = 0; i < count; i++) {
				if (i) {
					body += " | ";
				}
				body += "((uint(bits[" + itos(i) + "]) & " + itos((1 << width) - 1) + "u) << " + itos(i * width) + ")";
			}
			name = _slang_helper(result_type, "godot_" + name, types, body + ";");
		} else if (name.begins_with("unpackUnorm") || name.begins_with("unpackSnorm")) {
			int count = name.ends_with("4x8") ? 4 : 2;
			int width = 32 / count;
			bool sign = name.begins_with("unpackSnorm");
			String body = "return clamp(" + result_type + "(";
			for (int i = 0; i < count; i++) {
				if (i) {
					body += ", ";
				}
				body += sign ? "(int(a0 << " + itos(32 - (i + 1) * width) + ") >> " + itos(32 - width) + ")" : "((a0 >> " + itos(i * width) + ") & " + itos((1 << width) - 1) + "u)";
			}
			body += ") / " + itos((1 << (width - int(sign))) - 1) + ".0, " + (sign ? "-1.0" : "0.0") + ", 1.0);";
			name = _slang_helper(result_type, "godot_" + name, types, body);
		} else if (name == "mod") {
			name = _slang_helper(result_type, "godot_mod", types, "return a0 - a1 * floor(a0 / a1);");
		} else if (name == "mix") {
			if (p_node->arguments[3]->get_datatype() >= SL::TYPE_BOOL && p_node->arguments[3]->get_datatype() <= SL::TYPE_BVEC4) {
				return "select(" + arguments[2] + ", " + arguments[1] + ", " + arguments[0] + ")";
			}
			name = "lerp";
		} else if (name == "matrixCompMult") {
			return "(" + arguments[0] + " * " + arguments[1] + ")";
		} else if (name == "not") {
			return "(!" + arguments[0] + ")";
		} else {
			static const char *from[] = { "fract", "inversesqrt", "roundEven", "floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat", "dFdx", "dFdy", "dFdxCoarse", "dFdyCoarse", "dFdxFine", "dFdyFine", "bitfieldReverse", "bitCount", "findLSB", "findMSB" };
			static const char *to[] = { "frac", "rsqrt", "round", "asint", "asuint", "asfloat", "asfloat", "ddx", "ddy", "ddx_coarse", "ddy_coarse", "ddx_fine", "ddy_fine", "reversebits", "countbits", "firstbitlow", "firstbithigh" };
			for (uint32_t i = 0; i < sizeof(from) / sizeof(from[0]); i++) {
				if (name == from[i]) {
					name = to[i];
					break;
				}
			}
			static const char *comparisons[] = { "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual" };
			static const char *operators[] = { "<", "<=", ">", ">=", "==", "!=" };
			for (uint32_t i = 0; i < sizeof(comparisons) / sizeof(comparisons[0]); i++) {
				if (name == comparisons[i]) {
					return "(" + arguments[0] + " " + operators[i] + " " + arguments[1] + ")";
				}
			}
		}
	} else if (p_default_actions.renames.has(callee->name)) {
		name = p_default_actions.renames[callee->name];
	} else {
		name = _mkid(callee->rname);
	}
	return name + "(" + String(", ").join(arguments) + ")";
}

String ShaderCompiler::_dump_node_code(const SL::Node *p_node, int p_level, GeneratedCode &r_gen_code, IdentifierActions &p_actions, const DefaultIdentifierActions &p_default_actions, bool p_assigning, bool p_use_scope) {
	String code;

	switch (p_node->type) {
		case SL::Node::NODE_TYPE_SHADER: {
			SL::ShaderNode *pnode = (SL::ShaderNode *)p_node;

			// Render modes.

			for (int i = 0; i < pnode->render_modes.size(); i++) {
				if (p_default_actions.render_mode_defines.has(pnode->render_modes[i]) && !used_rmode_defines.has(pnode->render_modes[i])) {
					r_gen_code.defines.push_back(p_default_actions.render_mode_defines[pnode->render_modes[i]]);
					used_rmode_defines.insert(pnode->render_modes[i]);
				}

				if (p_actions.render_mode_flags.has(pnode->render_modes[i])) {
					*p_actions.render_mode_flags[pnode->render_modes[i]] = true;
				}

				if (p_actions.render_mode_values.has(pnode->render_modes[i])) {
					Pair<int *, int> &p = p_actions.render_mode_values[pnode->render_modes[i]];
					*p.first = p.second;
				}
			}

			// Stencil modes.

			for (int i = 0; i < pnode->stencil_modes.size(); i++) {
				if (p_actions.stencil_mode_values.has(pnode->stencil_modes[i])) {
					Pair<int *, int> &p = p_actions.stencil_mode_values[pnode->stencil_modes[i]];
					*p.first = p.second;
				}
			}

			// Stencil reference value.

			if (p_actions.stencil_reference && pnode->stencil_reference != -1) {
				*p_actions.stencil_reference = pnode->stencil_reference;
			}

			// structs

			for (int i = 0; i < pnode->vstructs.size(); i++) {
				SL::StructNode *st = pnode->vstructs[i].shader_struct;
				String struct_code;

				struct_code += "struct ";
				struct_code += _mkid(pnode->vstructs[i].name);
				struct_code += " ";
				struct_code += "{\n";
				for (SL::MemberNode *m : st->members) {
					if (m->datatype == SL::TYPE_STRUCT) {
						struct_code += _mkid(m->struct_name);
					} else {
						struct_code += _prestr(m->precision);
						struct_code += _typestr(m->datatype);
					}
					struct_code += " ";
					struct_code += _mkid(m->name);
					if (m->array_size > 0) {
						struct_code += "[";
						struct_code += itos(m->array_size);
						struct_code += "]";
					}
					struct_code += ";\n";
				}
				struct_code += "}";
				struct_code += ";\n";

				for (int j = 0; j < STAGE_MAX; j++) {
					r_gen_code.stage_globals[j] += struct_code;
				}
			}

			int max_texture_uniforms = 0;
			int max_uniforms = 0;

			for (const KeyValue<StringName, SL::ShaderNode::Uniform> &E : pnode->uniforms) {
				if (SL::is_sampler_type(E.value.type)) {
					if (E.value.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
						continue; // Don't create uniforms in the generated code for these.
					}
					max_texture_uniforms++;
				} else {
					if (E.value.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
						continue; // Instances are indexed directly, don't need index uniforms.
					}

					max_uniforms++;
				}
			}

			r_gen_code.texture_uniforms.resize(max_texture_uniforms);

			Vector<int> uniform_sizes;
			Vector<int> uniform_alignments;
			Vector<StringName> uniform_defines;
			uniform_sizes.resize(max_uniforms);
			uniform_alignments.resize(max_uniforms);
			uniform_defines.resize(max_uniforms);
			bool uses_uniforms = false;

			Vector<StringName> uniform_names;

			for (const KeyValue<StringName, SL::ShaderNode::Uniform> &E : pnode->uniforms) {
				uniform_names.push_back(E.key);
			}

			uniform_names.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

			for (int k = 0; k < uniform_names.size(); k++) {
				const StringName &uniform_name = uniform_names[k];
				const SL::ShaderNode::Uniform &uniform = pnode->uniforms[uniform_name];

				String ucode;

				if (uniform.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
					//insert, but don't generate any code.
					p_actions.uniforms->insert(uniform_name, uniform);
					continue; // Instances are indexed directly, don't need index uniforms.
				}

				if (uniform.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
					continue; // Don't create uniforms in the generated code for these.
				}

				if (SL::is_sampler_type(uniform.type)) {
					// Texture layouts are different for OpenGL GLSL and Vulkan GLSL
					if (actions.target == TARGET_SLANG) {
						ucode = "[[vk::binding(" + itos(actions.base_texture_binding_index + uniform.texture_binding) + ", " + itos(actions.texture_layout_set) + ")]] ";
					} else if (!RS::get_singleton()->is_low_end()) {
						ucode = "layout(set = " + itos(actions.texture_layout_set) + ", binding = " + itos(actions.base_texture_binding_index + uniform.texture_binding) + ") ";
					}
					if (actions.target == TARGET_GLSL) {
						ucode += "uniform ";
					}
				}

				bool is_buffer_global = !SL::is_sampler_type(uniform.type) && uniform.scope == SL::ShaderNode::Uniform::SCOPE_GLOBAL;

				if (is_buffer_global) {
					//this is an integer to index the global table
					ucode += _typestr(ShaderLanguage::TYPE_UINT);
				} else {
					ucode += _prestr(uniform.precision, ShaderLanguage::is_float_type(uniform.type));
					if (actions.target == TARGET_SLANG && uniform.type >= SL::TYPE_MAT2 && uniform.type <= SL::TYPE_MAT4) {
						ucode += "row_major ";
					}
					ucode += _typestr(uniform.type);
				}

				ucode += " " + _mkid(uniform_name);
				if (uniform.array_size > 0) {
					ucode += "[";
					ucode += itos(uniform.array_size);
					ucode += "]";
				}
				ucode += ";\n";
				if (SL::is_sampler_type(uniform.type)) {
					for (int j = 0; j < STAGE_MAX; j++) {
						r_gen_code.stage_globals[j] += ucode;
					}

					GeneratedCode::Texture texture;
					texture.name = uniform_name;
					texture.hint = uniform.hint;
					texture.type = uniform.type;
					texture.use_color = uniform.use_color;
					texture.filter = uniform.filter;
					texture.repeat = uniform.repeat;
					texture.global = uniform.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL;
					texture.array_size = uniform.array_size;
					if (texture.global) {
						r_gen_code.uses_global_textures = true;
					}

					r_gen_code.texture_uniforms.write[uniform.texture_order] = texture;
				} else {
					if (!uses_uniforms) {
						uses_uniforms = true;
					}
					uniform_defines.write[uniform.order] = ucode;
					if (is_buffer_global) {
						//globals are indices into the global table
						uniform_sizes.write[uniform.order] = ShaderLanguage::get_datatype_size(ShaderLanguage::TYPE_UINT);
						uniform_alignments.write[uniform.order] = _get_datatype_alignment(ShaderLanguage::TYPE_UINT);
					} else {
						// The following code enforces a 16-byte alignment of uniform arrays.
						if (uniform.array_size > 0) {
							int size = ShaderLanguage::get_datatype_size(uniform.type) * uniform.array_size;
							int m = (16 * uniform.array_size);
							if ((size % m) != 0) {
								size += m - (size % m);
							}
							uniform_sizes.write[uniform.order] = size;
							uniform_alignments.write[uniform.order] = 16;
						} else {
							uniform_sizes.write[uniform.order] = ShaderLanguage::get_datatype_size(uniform.type);
							uniform_alignments.write[uniform.order] = _get_datatype_alignment(uniform.type);
						}
					}
				}

				p_actions.uniforms->insert(uniform_name, uniform);
			}


			// add up
			int offset = 0;
			for (int i = 0; i < uniform_sizes.size(); i++) {
				int align = offset % uniform_alignments[i];

				if (align != 0) {
					offset += uniform_alignments[i] - align;
				}

				r_gen_code.uniform_offsets.push_back(offset);
				if (actions.target == TARGET_SLANG) {
					r_gen_code.uniforms += "[[vk::offset(" + itos(offset) + ")]] ";
				}
				r_gen_code.uniforms += uniform_defines[i];

				offset += uniform_sizes[i];
			}

			r_gen_code.uniform_total_size = offset;

			if (r_gen_code.uniform_total_size % 16 != 0) { //UBO sizes must be multiples of 16
				r_gen_code.uniform_total_size += 16 - (r_gen_code.uniform_total_size % 16);
			}

			uint32_t index = p_default_actions.base_varying_index;

			List<Pair<StringName, SL::ShaderNode::Varying>> var_frag_to_light;

			Vector<StringName> varying_names;

			for (const KeyValue<StringName, SL::ShaderNode::Varying> &E : pnode->varyings) {
				varying_names.push_back(E.key);
			}

			varying_names.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

			for (int k = 0; k < varying_names.size(); k++) {
				const StringName &varying_name = varying_names[k];
				const SL::ShaderNode::Varying &varying = pnode->varyings[varying_name];

				if (varying.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT && !p_default_actions.suppress_varying_io) {
					var_frag_to_light.push_back(Pair<StringName, SL::ShaderNode::Varying>(varying_name, varying));
					fragment_varyings.insert(varying_name);
					continue;
				}
				if (varying.type < SL::TYPE_INT) {
					continue; // Ignore boolean types to prevent crashing (if varying is just declared).
				}

				String type_str = _prestr(varying.precision, ShaderLanguage::is_float_type(varying.type)) + _typestr(varying.type);
				String name_str = _mkid(varying_name);
				uint32_t inc = varying.get_size();

				if (actions.target == TARGET_SLANG && !p_default_actions.suppress_varying_io) {
					String declaration = type_str + " " + name_str;
					if (varying.array_size > 0) {
						declaration += "[" + itos(varying.array_size) + "]";
					}
					declaration += ";\n";
					String interpolation = varying.interpolation == SL::INTERPOLATION_FLAT ? "nointerpolation " : "";
					r_gen_code.code["varyings"] += "[[vk::location(" + itos(index) + ")]] " + interpolation + declaration;
					r_gen_code.stage_globals[STAGE_VERTEX] += "static " + declaration;
					r_gen_code.stage_globals[STAGE_FRAGMENT] += "static " + declaration;
					r_gen_code.code["varyings_vertex"] += "stage_output." + name_str + " = " + name_str + ";\n";
					r_gen_code.code["varyings_fragment"] += name_str + " = stage_input." + name_str + ";\n";
				} else if (p_default_actions.suppress_varying_io) {
					// No vertex stage (e.g. RT shaders): emit zero-initialized
					// globals instead of in/out IO declarations.
					String decl = type_str + " " + name_str;
					if (varying.array_size > 0) {
						decl += "[" + itos(varying.array_size) + "];\n";
					} else {
						decl += " = " + _typestr(varying.type) + "(0);\n";
					}
					r_gen_code.stage_globals[STAGE_FRAGMENT] += decl;
				} else {
					String vcode;
					String interp_mode = _interpstr(varying.interpolation);
					vcode += type_str;
					vcode += " " + name_str;

					if (varying.array_size > 0) {
						vcode += "[";
						vcode += itos(varying.array_size);
						vcode += "]";
					}

					vcode += ";\n";
					// GLSL ES 3.0 does not allow layout qualifiers for varyings
					if (!RS::get_singleton()->is_low_end()) {
						r_gen_code.stage_globals[STAGE_VERTEX] += "layout(location=" + itos(index) + ") ";
						r_gen_code.stage_globals[STAGE_FRAGMENT] += "layout(location=" + itos(index) + ") ";
					}
					r_gen_code.stage_globals[STAGE_VERTEX] += interp_mode + "out " + vcode;
					r_gen_code.stage_globals[STAGE_FRAGMENT] += interp_mode + "in " + vcode;
				}

				index += inc;
			}

			if (!p_default_actions.suppress_varying_io && var_frag_to_light.size() > 0) {
				String gcode = actions.target == TARGET_SLANG ? "\nstruct GodotFragmentVaryings {\n" : "\n\nstruct {\n";
				for (const Pair<StringName, SL::ShaderNode::Varying> &E : var_frag_to_light) {
					gcode += "\t" + _prestr(E.second.precision) + _typestr(E.second.type) + " " + _mkid(E.first);
					if (E.second.array_size > 0) {
						gcode += "[";
						gcode += itos(E.second.array_size);
						gcode += "]";
					}
					gcode += ";\n";
				}
				gcode += actions.target == TARGET_SLANG ? "};\nstatic GodotFragmentVaryings frag_to_light;\n" : "} frag_to_light;\n";
				r_gen_code.stage_globals[STAGE_FRAGMENT] += gcode;
			}

			for (int i = 0; i < pnode->vconstants.size(); i++) {
				const SL::ShaderNode::Constant &cnode = pnode->vconstants[i];
				String gcode;
				if (actions.target == TARGET_SLANG) {
					gcode += "static ";
				}
				gcode += _constr(true);
				gcode += _prestr(cnode.precision, ShaderLanguage::is_float_type(cnode.type));
				if (cnode.type == SL::TYPE_STRUCT) {
					gcode += _mkid(cnode.struct_name);
				} else {
					gcode += _typestr(cnode.type);
				}
				gcode += " " + _mkid(String(cnode.name));
				if (cnode.array_size > 0) {
					gcode += "[";
					gcode += itos(cnode.array_size);
					gcode += "]";
				}
				gcode += "=";
				gcode += _dump_node_code(cnode.initializer, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				gcode += ";\n";
				for (int j = 0; j < STAGE_MAX; j++) {
					r_gen_code.stage_globals[j] += gcode;
				}
			}

			HashMap<StringName, String> function_code;

			//code for functions
			for (int i = 0; i < pnode->vfunctions.size(); i++) {
				SL::FunctionNode *fnode = pnode->vfunctions[i].function;
				function = fnode;
				current_func_name = fnode->name;
				function_code[fnode->name] = _dump_node_code(fnode->body, p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				function = nullptr;
			}

			//place functions in actual code

			HashSet<StringName> added_funcs_per_stage[STAGE_MAX];

			for (int i = 0; i < pnode->vfunctions.size(); i++) {
				SL::FunctionNode *fnode = pnode->vfunctions[i].function;

				function = fnode;

				current_func_name = fnode->name;

				if (p_actions.entry_point_stages.has(fnode->name)) {
					Stage stage = p_actions.entry_point_stages[fnode->name];
					Stage dep_stage = (p_default_actions.suppress_varying_io && stage == STAGE_VERTEX) ? STAGE_FRAGMENT : stage;
					_dump_function_deps(pnode, fnode->name, function_code, r_gen_code.stage_globals[dep_stage], added_funcs_per_stage[dep_stage]);
					r_gen_code.code[fnode->name] = function_code[fnode->name];
				}

				function = nullptr;
			}

			//code+=dump_node_code(pnode->body,p_level);
		} break;
		case SL::Node::NODE_TYPE_STRUCT: {
		} break;
		case SL::Node::NODE_TYPE_FUNCTION: {
		} break;
		case SL::Node::NODE_TYPE_BLOCK: {
			SL::BlockNode *bnode = (SL::BlockNode *)p_node;

			//variables
			if (!bnode->single_statement) {
				code += _mktab(p_level - 1) + "{\n";
			}

			int i = 0;
			for (List<ShaderLanguage::Node *>::ConstIterator itr = bnode->statements.begin(); itr != bnode->statements.end(); ++itr, ++i) {
				String scode = _dump_node_code(*itr, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);

				if ((*itr)->type == SL::Node::NODE_TYPE_CONTROL_FLOW || bnode->single_statement) {
					code += scode; //use directly
					if (bnode->use_comma_between_statements && i + 1 < bnode->statements.size()) {
						code += ",";
					}
				} else {
					code += _mktab(p_level) + scode + ";\n";
				}
			}
			if (!bnode->single_statement) {
				code += _mktab(p_level - 1) + "}\n";
			}

		} break;
		case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
			SL::VariableDeclarationNode *vdnode = (SL::VariableDeclarationNode *)p_node;

			String declaration;
			declaration += _constr(vdnode->is_const);
			if (vdnode->datatype == SL::TYPE_STRUCT) {
				declaration += _mkid(vdnode->struct_name);
			} else {
				declaration += _prestr(vdnode->precision) + _typestr(vdnode->datatype);
			}
			declaration += " ";
			for (int i = 0; i < vdnode->declarations.size(); i++) {
				bool is_array = vdnode->declarations[i].size > 0;
				if (i > 0) {
					declaration += ",";
				}
				declaration += _mkid(vdnode->declarations[i].name);
				if (is_array) {
					declaration += "[";
					if (vdnode->declarations[i].size_expression != nullptr) {
						declaration += _dump_node_code(vdnode->declarations[i].size_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					} else {
						declaration += itos(vdnode->declarations[i].size);
					}
					declaration += "]";
				}

				if (!is_array || vdnode->declarations[i].single_expression) {
					if (!vdnode->declarations[i].initializer.is_empty()) {
						declaration += "=";
						declaration += _dump_node_code(vdnode->declarations[i].initializer[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					}
				} else {
					int size = vdnode->declarations[i].initializer.size();
					if (size > 0) {
						declaration += "=";
						if (vdnode->datatype == SL::TYPE_STRUCT) {
							declaration += _mkid(vdnode->struct_name);
						} else {
							declaration += _typestr(vdnode->datatype);
						}
						declaration += "[";
						declaration += itos(size);
						declaration += "]";
						declaration += "(";
						for (int j = 0; j < size; j++) {
							if (j > 0) {
								declaration += ",";
							}
							declaration += _dump_node_code(vdnode->declarations[i].initializer[j], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
						}
						declaration += ")";
					}
				}
			}

			code += declaration;
		} break;
		case SL::Node::NODE_TYPE_VARIABLE: {
			SL::VariableNode *vnode = (SL::VariableNode *)p_node;
			bool use_fragment_varying = false;

			if (!vnode->is_local && !(p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX)) {
				if (p_assigning) {
					if (shader->varyings.has(vnode->name)) {
						use_fragment_varying = true;
					}
				} else {
					if (fragment_varyings.has(vnode->name)) {
						use_fragment_varying = true;
					}
				}
			}

			if (p_assigning && p_actions.write_flag_pointers.has(vnode->name)) {
				*p_actions.write_flag_pointers[vnode->name] = true;
			}

			if (p_default_actions.usage_defines.has(vnode->name) && !used_name_defines.has(vnode->name)) {
				String define = p_default_actions.usage_defines[vnode->name];
				if (define.begins_with("@")) {
					define = p_default_actions.usage_defines[define.substr(1)];
				}
				r_gen_code.defines.push_back(define);
				used_name_defines.insert(vnode->name);
			}

			if (p_actions.usage_flag_pointers.has(vnode->name) && !used_flag_pointers.has(vnode->name)) {
				*p_actions.usage_flag_pointers[vnode->name] = true;
				used_flag_pointers.insert(vnode->name);
			}

			if (p_default_actions.renames.has(vnode->name)) {
				code = p_default_actions.renames[vnode->name];
			} else {
				bool param_found = false;
				if (function) {
					for (const SL::FunctionNode::Argument &argument : function->arguments) {
						if (argument.name == vnode->name) {
							param_found = true;
							break;
						}
					}
				}
				if (!param_found && shader->uniforms.has(vnode->name)) {
					//its a uniform!
					const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[vnode->name];
					if (u.is_texture()) {
						StringName name;
						if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
							name = "color_buffer";
							if (u.filter >= ShaderLanguage::FILTER_NEAREST_MIPMAP) {
								r_gen_code.uses_screen_texture_mipmaps = true;
							}
							r_gen_code.uses_screen_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
							name = "normal_roughness_buffer";
							r_gen_code.uses_normal_roughness_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
							name = "depth_buffer";
							r_gen_code.uses_depth_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE0) {
							name = "source0";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE1) {
							name = "source1";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE2) {
							name = "source2";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
							name = "source3";
						} else {
							name = _mkid(vnode->name); //texture, use as is
						}

						code = name;
					} else {
						//a scalar or vector
						if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
							code = actions.base_uniform_string + _mkid(vnode->name); //texture, use as is
							//global variable, this means the code points to an index to the global table
							code = _global_uniform(p_default_actions.global_buffer_array_variable, code, u.type);
						} else if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
							//instance variable, index it as such
							code = "(" + p_default_actions.instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
							code = _global_uniform(p_default_actions.global_buffer_array_variable, code, u.type);
						} else {
							//regular uniform, index from UBO
							code = actions.base_uniform_string + _mkid(vnode->name);
						}
					}

				} else {
					if (use_fragment_varying) {
						code = "frag_to_light.";
					}
					code += _mkid(vnode->name); //its something else (local var most likely) use as is
				}
			}

			if (vnode->name == time_name) {
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX) {
					r_gen_code.uses_vertex_time = true;
				}
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_FRAGMENT) {
					r_gen_code.uses_fragment_time = true;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
			SL::ArrayConstructNode *acnode = (SL::ArrayConstructNode *)p_node;
			int sz = acnode->initializer.size();
			if (acnode->datatype == SL::TYPE_STRUCT) {
				code += _mkid(acnode->struct_name);
			} else {
				code += _typestr(acnode->datatype);
			}
			code += "[";
			code += itos(acnode->initializer.size());
			code += "]";
			code += "(";
			for (int i = 0; i < sz; i++) {
				code += _dump_node_code(acnode->initializer[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				if (i != sz - 1) {
					code += ", ";
				}
			}
			code += ")";
		} break;
		case SL::Node::NODE_TYPE_ARRAY: {
			SL::ArrayNode *anode = (SL::ArrayNode *)p_node;
			bool use_fragment_varying = false;

			if (!anode->is_local && !(p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX)) {
				if (anode->assign_expression != nullptr && shader->varyings.has(anode->name)) {
					use_fragment_varying = true;
				} else {
					if (p_assigning) {
						if (shader->varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					} else {
						if (fragment_varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					}
				}
			}

			if (p_assigning && p_actions.write_flag_pointers.has(anode->name)) {
				*p_actions.write_flag_pointers[anode->name] = true;
			}

			if (p_default_actions.usage_defines.has(anode->name) && !used_name_defines.has(anode->name)) {
				String define = p_default_actions.usage_defines[anode->name];
				if (define.begins_with("@")) {
					define = p_default_actions.usage_defines[define.substr(1)];
				}
				r_gen_code.defines.push_back(define);
				used_name_defines.insert(anode->name);
			}

			if (p_actions.usage_flag_pointers.has(anode->name) && !used_flag_pointers.has(anode->name)) {
				*p_actions.usage_flag_pointers[anode->name] = true;
				used_flag_pointers.insert(anode->name);
			}

			if (p_default_actions.renames.has(anode->name)) {
				code = p_default_actions.renames[anode->name];
			} else {
				bool param_found = false;
				if (function) {
					for (const SL::FunctionNode::Argument &argument : function->arguments) {
						if (argument.name == anode->name) {
							param_found = true;
							break;
						}
					}
				}
				if (!param_found && shader->uniforms.has(anode->name)) {
					//its a uniform!
					const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[anode->name];
					if (u.is_texture()) {
						code = _mkid(anode->name); //texture, use as is
					} else {
						//a scalar or vector
						if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
							code = actions.base_uniform_string + _mkid(anode->name); //texture, use as is
							//global variable, this means the code points to an index to the global table
							code = _global_uniform(p_default_actions.global_buffer_array_variable, code, u.type);
						} else if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
							//instance variable, index it as such
							code = "(" + p_default_actions.instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
							code = _global_uniform(p_default_actions.global_buffer_array_variable, code, u.type);
						} else {
							//regular uniform, index from UBO
							code = actions.base_uniform_string + _mkid(anode->name);
						}
					}
				} else {
					if (use_fragment_varying) {
						code = "frag_to_light.";
					}
					code += _mkid(anode->name);
				}
			}

			if (anode->call_expression != nullptr) {
				code += ".";
				code += _dump_node_code(anode->call_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning, false);
			} else if (anode->index_expression != nullptr) {
				code += "[";
				code += _dump_node_code(anode->index_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += "]";
			} else if (anode->assign_expression != nullptr) {
				code += "=";
				code += _dump_node_code(anode->assign_expression, p_level, r_gen_code, p_actions, p_default_actions, true, false);
			}

			if (anode->name == time_name) {
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX) {
					r_gen_code.uses_vertex_time = true;
				}
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_FRAGMENT) {
					r_gen_code.uses_fragment_time = true;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_CONSTANT: {
			SL::ConstantNode *cnode = (SL::ConstantNode *)p_node;

			if (cnode->array_size == 0) {
				return _constant_text(cnode->datatype, cnode->values);
			} else {
				if (cnode->get_datatype() == SL::TYPE_STRUCT) {
					code += _mkid(cnode->struct_name);
				} else {
					code += _typestr(cnode->datatype);
				}
				code += "[";
				code += itos(cnode->array_size);
				code += "]";
				code += "(";
				for (int i = 0; i < cnode->array_size; i++) {
					if (i > 0) {
						code += ",";
					} else {
						code += "";
					}
					code += _dump_node_code(cnode->array_declarations[0].initializer[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				}
				code += ")";
			}

		} break;
		case SL::Node::NODE_TYPE_OPERATOR: {
			SL::OperatorNode *onode = (SL::OperatorNode *)p_node;
			if (actions.target == TARGET_SLANG && (onode->op == SL::OP_CALL || onode->op == SL::OP_CONSTRUCT || onode->op == SL::OP_STRUCT)) {
				return _dump_slang_call(onode, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
			}
			if (actions.target == TARGET_SLANG && (onode->op == SL::OP_EQUAL || onode->op == SL::OP_NOT_EQUAL) && onode->get_datatype() == SL::TYPE_BOOL) {
				SL::DataType type = onode->arguments[0]->get_datatype();
				bool matrix = type >= SL::TYPE_MAT2 && type <= SL::TYPE_MAT4;
				bool vector = type >= SL::TYPE_BOOL && type <= SL::TYPE_VEC4 && (type - SL::TYPE_BOOL) % 4 != 0;
				if (matrix || vector) {
					String left = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, false);
					String right = _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, false);
					String result;
					if (matrix) {
						String body = "return ";
						for (int i = 0; i < type - SL::TYPE_MAT2 + 2; i++) {
							if (i) { body += " && "; }
							body += "all(a0[" + itos(i) + "] == a1[" + itos(i) + "])";
						}
						String helper = _slang_helper("bool", "godot_matrix_equal", { _typestr(type), _typestr(type) }, body + ";");
						result = helper + "(" + left + ", " + right + ")";
					} else {
						result = "all(" + left + " == " + right + ")";
					}
					return onode->op == SL::OP_NOT_EQUAL ? "(!" + result + ")" : result;
				}
			}
			if (actions.target == TARGET_SLANG && (onode->op == SL::OP_MUL || onode->op == SL::OP_ASSIGN_MUL)) {
				SL::DataType left_type = onode->arguments[0]->get_datatype();
				SL::DataType right_type = onode->arguments[1]->get_datatype();
				bool left_matrix = left_type >= SL::TYPE_MAT2 && left_type <= SL::TYPE_MAT4;
				bool right_matrix = right_type >= SL::TYPE_MAT2 && right_type <= SL::TYPE_MAT4;
				if ((left_matrix || right_matrix) && left_type != SL::TYPE_FLOAT && right_type != SL::TYPE_FLOAT) {
					String left = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, onode->op == SL::OP_ASSIGN_MUL || p_assigning);
					String right = _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					if (onode->op == SL::OP_ASSIGN_MUL) {
						String helper = _slang_helper(_typestr(left_type), "godot_matrix_assign_mul", { "inout " + _typestr(left_type), _typestr(right_type) }, "a0 = mul(a1, a0); return a0;");
						return helper + "(" + left + ", " + right + ")";
					}
					return "mul(" + right + ", " + left + ")";
				}
			}

			switch (onode->op) {
				case SL::OP_ASSIGN:
				case SL::OP_ASSIGN_ADD:
				case SL::OP_ASSIGN_SUB:
				case SL::OP_ASSIGN_MUL:
				case SL::OP_ASSIGN_DIV:
				case SL::OP_ASSIGN_SHIFT_LEFT:
				case SL::OP_ASSIGN_SHIFT_RIGHT:
				case SL::OP_ASSIGN_MOD:
				case SL::OP_ASSIGN_BIT_AND:
				case SL::OP_ASSIGN_BIT_OR:
				case SL::OP_ASSIGN_BIT_XOR:
					code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, true) + _opstr(onode->op) + _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					break;
				case SL::OP_BIT_INVERT:
				case SL::OP_NEGATE:
				case SL::OP_NOT:
				case SL::OP_DECREMENT:
				case SL::OP_INCREMENT: {
					const String node_code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);

					if (onode->op == SL::OP_NEGATE && node_code.begins_with("-")) { // To prevent writing unary minus twice.
						code = node_code;
					} else {
						code = _opstr(onode->op) + node_code;
					}

				} break;
				case SL::OP_POST_DECREMENT:
				case SL::OP_POST_INCREMENT:
					code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + _opstr(onode->op);
					break;
				case SL::OP_CALL:
				case SL::OP_STRUCT:
				case SL::OP_CONSTRUCT: {
					ERR_FAIL_COND_V(onode->arguments[0]->type != SL::Node::NODE_TYPE_VARIABLE, String());
					const SL::VariableNode *vnode = static_cast<const SL::VariableNode *>(onode->arguments[0]);
					const SL::FunctionNode *func = nullptr;
					const bool is_internal_func = internal_functions.has(vnode->name);

					if (!is_internal_func) {
						for (int i = 0; i < shader->vfunctions.size(); i++) {
							if (shader->vfunctions[i].name == vnode->name) {
								func = shader->vfunctions[i].function;
								break;
							}
						}
					}

					bool is_texture_func = false;
					bool is_screen_texture = false;
					bool is_radiance_texture = false;
					bool texture_func_no_uv = false;
					bool texture_func_returns_data = false;
					bool texture_func_simple = false;

					if (onode->op == SL::OP_STRUCT) {
						code += _mkid(vnode->name);
					} else if (onode->op == SL::OP_CONSTRUCT) {
						code += String(vnode->name);
					} else {
						if (p_actions.usage_flag_pointers.has(vnode->name) && !used_flag_pointers.has(vnode->name)) {
							*p_actions.usage_flag_pointers[vnode->name] = true;
							used_flag_pointers.insert(vnode->name);
						}

						if (is_internal_func) {
							code += vnode->name;
							is_texture_func = texture_functions.has(vnode->name);
							texture_func_no_uv = (vnode->name == "textureSize" || vnode->name == "textureQueryLevels");
							texture_func_returns_data = texture_func_no_uv || vnode->name == "textureQueryLod";
							texture_func_simple = vnode->name == "texture";
						} else if (p_default_actions.renames.has(vnode->name)) {
							code += p_default_actions.renames[vnode->name];
						} else {
							code += _mkid(vnode->rname);
						}
					}

					code += "(";

					// if color backbuffer, depth backbuffer or normal roughness texture is used,
					// we will add logic to automatically switch between
					// sampler2D and sampler2D array and vec2 UV and vec3 UV.
					bool multiview_uv_needed = false;
					bool is_normal_roughness_texture = false;

					for (int i = 1; i < onode->arguments.size(); i++) {
						if (i > 1) {
							code += ", ";
						}

						bool is_out_qualifier = false;
						if (is_internal_func) {
							is_out_qualifier = SL::is_builtin_func_out_parameter(vnode->name, i - 1);
						} else if (func != nullptr) {
							const SL::ArgumentQualifier qualifier = func->arguments[i - 1].qualifier;
							is_out_qualifier = qualifier == SL::ARGUMENT_QUALIFIER_OUT || qualifier == SL::ARGUMENT_QUALIFIER_INOUT;
						}

						if (is_out_qualifier) {
							StringName name;
							bool found = false;
							{
								const SL::Node *node = onode->arguments[i];

								bool done = false;
								do {
									switch (node->type) {
										case SL::Node::NODE_TYPE_VARIABLE: {
											name = static_cast<const SL::VariableNode *>(node)->name;
											done = true;
											found = true;
										} break;
										case SL::Node::NODE_TYPE_MEMBER: {
											node = static_cast<const SL::MemberNode *>(node)->owner;
										} break;
										default: {
											done = true;
										} break;
									}
								} while (!done);
							}

							if (found && p_actions.write_flag_pointers.has(name)) {
								*p_actions.write_flag_pointers[name] = true;
							}
						}

						String node_code = _dump_node_code(onode->arguments[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
						if (is_texture_func && i == 1) {
							// If we're doing a texture lookup we need to check our texture argument
							StringName texture_uniform;
							bool correct_texture_uniform = false;

							switch (onode->arguments[i]->type) {
								case SL::Node::NODE_TYPE_VARIABLE: {
									const SL::VariableNode *varnode = static_cast<const SL::VariableNode *>(onode->arguments[i]);
									texture_uniform = varnode->name;
									correct_texture_uniform = true;
								} break;
								case SL::Node::NODE_TYPE_ARRAY: {
									const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(onode->arguments[i]);
									texture_uniform = anode->name;
									correct_texture_uniform = true;
								} break;
								default:
									break;
							}

							if (correct_texture_uniform && !RS::get_singleton()->is_low_end()) {
								// Need to map from texture to sampler in order to sample when using Vulkan GLSL.
								String sampler_name;
								bool is_depth_texture = false;

								if (actions.custom_samplers.has(texture_uniform)) {
									sampler_name = actions.custom_samplers[texture_uniform];
								} else {
									if (shader->uniforms.has(texture_uniform)) {
										const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[texture_uniform];
										if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											is_screen_texture = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											is_depth_texture = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											is_normal_roughness_texture = true;
										}
										sampler_name = _get_sampler_name(u.filter, u.repeat);
									} else {
										bool found = false;

										for (int j = 0; j < function->arguments.size(); j++) {
											if (function->arguments[j].name == texture_uniform) {
												if (function->arguments[j].tex_builtin_check) {
													ERR_CONTINUE(!actions.custom_samplers.has(function->arguments[j].tex_builtin));
													sampler_name = actions.custom_samplers[function->arguments[j].tex_builtin];
													found = true;
													break;
												}
												if (function->arguments[j].tex_argument_check) {
													if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
														is_screen_texture = true;
													} else if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
														is_depth_texture = true;
													} else if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
														is_normal_roughness_texture = true;
													}
													sampler_name = _get_sampler_name(function->arguments[j].tex_argument_filter, function->arguments[j].tex_argument_repeat);
													found = true;
													break;
												}
											}
										}
										if (!found) {
											//function was most likely unused, so use anything (compiler will remove it anyway)
											sampler_name = _get_sampler_name(ShaderLanguage::FILTER_DEFAULT, ShaderLanguage::REPEAT_DEFAULT);
										}
									}
								}

								if (texture_uniform == SNAME("RADIANCE")) {
									is_radiance_texture = true;
								}

								String data_type_name = "";
								if (actions.check_multiview_samplers && (is_screen_texture || is_depth_texture || is_normal_roughness_texture)) {
									data_type_name = "multiviewSampler";
									multiview_uv_needed = true;
								} else if (is_radiance_texture) {
									// We need to use an explicit level of detail to avoid mip mapping artifacts caused by the octahedral discontinuity.
									if (texture_func_simple) {
										code = code.replace("texture(", "textureLod(");
									}
									data_type_name = "sampler2D";
								} else {
									data_type_name = ShaderLanguage::get_datatype_name(onode->arguments[i]->get_datatype());
								}

								code += data_type_name + "(" + node_code + ", " + sampler_name + ")";
							} else if (correct_texture_uniform && RS::get_singleton()->is_low_end()) {
								// Texture function on low end hardware (i.e. OpenGL).

								if (shader->uniforms.has(texture_uniform)) {
									const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[texture_uniform];
									if (actions.check_multiview_samplers) {
										if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											multiview_uv_needed = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											multiview_uv_needed = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											multiview_uv_needed = true;
										}
									}
									if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
										is_screen_texture = true;
									}
								}

								code += node_code;
							} else {
								code += node_code;
							}
						} else if (multiview_uv_needed && !texture_func_no_uv && i == 2) {
							// UV coordinate after using color, depth or normal roughness texture.
							node_code = "multiview_uv(" + node_code + ".xy)";

							code += node_code;
						} else if (is_radiance_texture && !texture_func_no_uv && i == 2) {
							node_code = "vec3_to_oct_with_border(" + node_code + ", params.border_size)";

							// Need an explicit level of detail if one isn't provided by the user.
							if (texture_func_simple && onode->arguments.size() == 3) {
								node_code += ", 0.0";
							}

							code += node_code;
						} else {
							code += node_code;
						}
					}
					code += ")";
					if (is_screen_texture && !texture_func_returns_data && actions.apply_luminance_multiplier) {
						if (RS::get_singleton()->is_low_end()) {
							code = "(" + code + " / vec4(vec3(scene_data_block.data.luminance_multiplier), 1.0))";
						} else {
							code = "(" + code + " * vec4(vec3(sc_luminance_multiplier()), 1.0))";
						}
					}
					if (is_normal_roughness_texture && !texture_func_returns_data) {
						code = "normal_roughness_compatibility(" + code + ")";
					}
				} break;
				case SL::OP_INDEX: {
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "[";
					code += _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "]";

				} break;
				case SL::OP_SELECT_IF: {
					code += "(";
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "?";
					code += _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += ":";
					code += _dump_node_code(onode->arguments[2], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += ")";

				} break;
				case SL::OP_EMPTY: {
					// Semicolon (or empty statement) - ignored.
				} break;

				default: {
					if (p_use_scope) {
						code += "(";
					}
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + " " + _opstr(onode->op) + " " + _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					if (p_use_scope) {
						code += ")";
					}
					break;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_CONTROL_FLOW: {
			SL::ControlFlowNode *cfnode = (SL::ControlFlowNode *)p_node;
			if (cfnode->flow_op == SL::FLOW_OP_IF) {
				code += _mktab(p_level) + "if (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				if (cfnode->blocks.size() == 2) {
					code += _mktab(p_level) + "else\n";
					code += _dump_node_code(cfnode->blocks[1], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				}
			} else if (cfnode->flow_op == SL::FLOW_OP_SWITCH) {
				code += _mktab(p_level) + "switch (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_CASE) {
				code += _mktab(p_level) + "case " + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ":\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_DEFAULT) {
				code += _mktab(p_level) + "default:\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_DO) {
				code += _mktab(p_level) + "do";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += _mktab(p_level) + "while (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ");";
			} else if (cfnode->flow_op == SL::FLOW_OP_WHILE) {
				code += _mktab(p_level) + "while (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_FOR) {
				String left = _dump_node_code(cfnode->blocks[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				String middle = _dump_node_code(cfnode->blocks[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				String right = _dump_node_code(cfnode->blocks[2], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += _mktab(p_level) + "for (" + left + ";" + middle + ";" + right + ")\n";
				code += _dump_node_code(cfnode->blocks[3], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);

			} else if (cfnode->flow_op == SL::FLOW_OP_RETURN) {
				if (cfnode->expressions.size()) {
					code = "return " + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ";";
				} else {
					code = "return;";
				}
			} else if (cfnode->flow_op == SL::FLOW_OP_DISCARD) {
				if (p_actions.usage_flag_pointers.has("DISCARD") && !used_flag_pointers.has("DISCARD")) {
					*p_actions.usage_flag_pointers["DISCARD"] = true;
					used_flag_pointers.insert("DISCARD");
				}

				code = "discard;";
			} else if (cfnode->flow_op == SL::FLOW_OP_CONTINUE) {
				code = "continue;";
			} else if (cfnode->flow_op == SL::FLOW_OP_BREAK) {
				code = "break;";
			}

		} break;
		case SL::Node::NODE_TYPE_MEMBER: {
			SL::MemberNode *mnode = (SL::MemberNode *)p_node;
			String name;
			if (mnode->basetype == SL::TYPE_STRUCT) {
				name = _mkid(mnode->name);
			} else {
				name = mnode->name;
			}
			code = _dump_node_code(mnode->owner, p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + "." + name;
			if (mnode->index_expression != nullptr) {
				code += "[";
				code += _dump_node_code(mnode->index_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += "]";
			} else if (mnode->assign_expression != nullptr) {
				code += "=";
				code += _dump_node_code(mnode->assign_expression, p_level, r_gen_code, p_actions, p_default_actions, true, false);
			} else if (mnode->call_expression != nullptr) {
				code += ".";
				code += _dump_node_code(mnode->call_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning, false);
			}
		} break;
	}

	return code;
}

ShaderLanguage::DataType ShaderCompiler::_get_global_shader_uniform_type(const StringName &p_name) {
	RSE::GlobalShaderParameterType gvt = RSG::material_storage->global_shader_parameter_get_type(p_name);
	return (ShaderLanguage::DataType)RS::global_shader_uniform_type_get_shader_datatype(gvt);
}

Error ShaderCompiler::compile(RSE::ShaderMode p_mode, const String &p_code, IdentifierActions *p_actions, const String &p_path, GeneratedCode &r_gen_code) {
	SL::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(p_mode);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(p_mode);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(p_mode);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	info.global_shader_uniform_type_func = _get_global_shader_uniform_type;
	info.base_varying_index = actions.base_varying_index;

	Error err = parser.compile(p_code, info);

	if (err != OK) {
		Vector<ShaderLanguage::FilePosition> include_positions = parser.get_include_positions();

		String current;
		HashMap<String, Vector<String>> includes;
		includes[""] = Vector<String>();
		Vector<String> include_stack;
		Vector<String> shader_lines = p_code.split("\n");

		// Reconstruct the files.
		for (int i = 0; i < shader_lines.size(); i++) {
			String l = shader_lines[i];
			if (l.begins_with("@@>")) {
				String inc_path = l.replace_first("@@>", "");

				l = "#include \"" + inc_path + "\"";
				includes[current].append("#include \"" + inc_path + "\""); // Restore the include directive
				include_stack.push_back(current);
				current = inc_path;
				includes[inc_path] = Vector<String>();

			} else if (l.begins_with("@@<")) {
				if (include_stack.size()) {
					current = include_stack[include_stack.size() - 1];
					include_stack.resize(include_stack.size() - 1);
				}
			} else {
				includes[current].push_back(l);
			}
		}

		// Print the files.
		for (const KeyValue<String, Vector<String>> &E : includes) {
			int err_line = -1;
			for (const ShaderLanguage::FilePosition &include_position : include_positions) {
				if (include_position.file == E.key) {
					err_line = include_position.line;
				}
			}
			if (err_line < 0) {
				// Skip files that don't contain errors.
				continue;
			}

			if (E.key.is_empty()) {
				if (p_path == "") {
					print_line("--Main Shader--");
				} else {
					print_line("--" + p_path + "--");
				}
			} else {
				print_line("--" + E.key + "--");
			}
			const Vector<String> &V = E.value;
			for (int i = 0; i < V.size(); i++) {
				if (i == err_line - 1) {
					// Mark the error line to be visible without having to look at
					// the trace at the end.
					print_line(vformat("E%4d-> %s", i + 1, V[i]));
				} else if ((i == err_line - 3) || (i == err_line - 2) || (i == err_line) || (i == err_line + 1)) {
					// Print 4 lines around the error line.
					print_line(vformat("%5d | %s", i + 1, V[i]));
				}
			}
		}

		String file;
		int line;
		if (include_positions.size() > 1) {
			file = include_positions[include_positions.size() - 1].file;
			line = include_positions[include_positions.size() - 1].line;
		} else {
			file = p_path;
			line = parser.get_error_line();
		}

		_err_print_error(nullptr, file.utf8().get_data(), line, parser.get_error_text().utf8().get_data(), false, ERR_HANDLER_SHADER);
		return err;
	}

	r_gen_code.defines.clear();
	r_gen_code.code.clear();
	for (int i = 0; i < STAGE_MAX; i++) {
		r_gen_code.stage_globals[i] = String();
	}
	r_gen_code.uses_fragment_time = false;
	r_gen_code.uses_vertex_time = false;
	r_gen_code.uses_global_textures = false;
	r_gen_code.uses_screen_texture_mipmaps = false;
	r_gen_code.uses_screen_texture = false;
	r_gen_code.uses_depth_texture = false;
	r_gen_code.uses_normal_roughness_texture = false;

	slang_helper_signatures.clear();
	slang_helpers = String();
	if (actions.target == TARGET_SLANG) {
		_ALLOW_DISCARD_ _slang_inverse(SL::TYPE_MAT2);
		_ALLOW_DISCARD_ _slang_inverse(SL::TYPE_MAT3);
		_ALLOW_DISCARD_ _slang_inverse(SL::TYPE_MAT4);
	}
	used_name_defines.clear();
	used_rmode_defines.clear();
	used_flag_pointers.clear();
	fragment_varyings.clear();

	shader = parser.get_shader();
	function = nullptr;
	// Return value only relevant within nested calls.
	_ALLOW_DISCARD_ _dump_node_code(shader, 1, r_gen_code, *p_actions, actions, false);
	if (actions.target == TARGET_SLANG) {
		for (int i = 0; i < STAGE_MAX; i++) {
			r_gen_code.stage_globals[i] = slang_helpers + r_gen_code.stage_globals[i];
		}
	}

	return OK;
}

void ShaderCompiler::initialize(DefaultIdentifierActions p_actions) {
	actions = p_actions;

	time_name = "TIME";

	List<String> func_list;

	ShaderLanguage::get_builtin_funcs(&func_list);

	for (const String &E : func_list) {
		internal_functions.insert(E);
	}
	texture_functions.insert("texture");
	texture_functions.insert("textureProj");
	texture_functions.insert("textureLod");
	texture_functions.insert("textureProjLod");
	texture_functions.insert("textureGrad");
	texture_functions.insert("textureProjGrad");
	texture_functions.insert("textureGather");
	texture_functions.insert("textureSize");
	texture_functions.insert("textureQueryLod");
	texture_functions.insert("textureQueryLevels");
	texture_functions.insert("texelFetch");
}

ShaderCompiler::ShaderCompiler() {
}
