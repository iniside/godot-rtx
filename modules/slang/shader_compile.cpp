#include "shader_compile.h"

#include "core/io/file_access.h"
#include "core/os/mutex.h"
#include "core/os/os.h"

#include <slang-com-ptr.h>
#include <slang.h>
#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

static Mutex compiler_mutex;
static void *compiler_library = nullptr;
static Slang::ComPtr<slang::IGlobalSession> global_session;

class EmbeddedShaderBlob : public ISlangBlob {
	uint32_t references = 1;
	CharString source;

public:
	explicit EmbeddedShaderBlob(const String &p_source) : source(p_source.utf8()) {}
	SlangResult SLANG_MCALL queryInterface(const SlangUUID &p_uuid, void **r_object) noexcept override {
		if (p_uuid == ISlangUnknown::getTypeGuid() || p_uuid == ISlangBlob::getTypeGuid()) {
			*r_object = static_cast<ISlangBlob *>(this);
			addRef();
			return SLANG_OK;
		}
		*r_object = nullptr;
		return SLANG_E_NO_INTERFACE;
	}
	uint32_t SLANG_MCALL addRef() noexcept override { return ++references; }
	uint32_t SLANG_MCALL release() noexcept override {
		uint32_t remaining = --references;
		if (remaining == 0) {
			memdelete(this);
		}
		return remaining;
	}
	const void *SLANG_MCALL getBufferPointer() noexcept override { return source.get_data(); }
	size_t SLANG_MCALL getBufferSize() noexcept override { return source.length(); }
};

class EmbeddedShaderFileSystem : public ISlangFileSystem {
	uint32_t references = 1;
	HashMap<String, String> includes;

public:
	explicit EmbeddedShaderFileSystem(const HashMap<String, String> &p_includes) : includes(p_includes) {}
	void *SLANG_MCALL castAs(const SlangUUID &p_uuid) noexcept override {
		if (p_uuid == ISlangUnknown::getTypeGuid() || p_uuid == ISlangCastable::getTypeGuid() || p_uuid == ISlangFileSystem::getTypeGuid()) {
			return static_cast<ISlangFileSystem *>(this);
		}
		return nullptr;
	}
	SlangResult SLANG_MCALL queryInterface(const SlangUUID &p_uuid, void **r_object) noexcept override {
		*r_object = castAs(p_uuid);
		if (*r_object) {
			addRef();
			return SLANG_OK;
		}
		*r_object = nullptr;
		return SLANG_E_NO_INTERFACE;
	}

	uint32_t SLANG_MCALL addRef() noexcept override { return ++references; }
	uint32_t SLANG_MCALL release() noexcept override {
		uint32_t remaining = --references;
		if (remaining == 0) {
			memdelete(this);
		}
		return remaining;
	}
	SlangResult SLANG_MCALL loadFile(const char *p_path, ISlangBlob **r_blob) noexcept override {
		const String *source = includes.getptr(String::utf8(p_path).simplify_path());
		if (source) {
			*r_blob = memnew(EmbeddedShaderBlob(*source));
			return SLANG_OK;
		}
		*r_blob = nullptr;
		return SLANG_E_NOT_FOUND;
	}
};

String get_slang_shader_compiler_identity() {
	return "slang/" GODOT_SLANG_VERSION "/" GODOT_SLANG_ARCHIVE_SHA256 "/embedded-spirv-v1";
}

String get_slang_shader_compiler_filename() {
	return "slang-compiler." GODOT_SLANG_VERSION ".x86_64.dll";
}

static bool initialize_compiler(String *r_error) {
	if (global_session) {
		return true;
	}
	String library_path = OS::get_singleton()->get_executable_path().get_base_dir().path_join(get_slang_shader_compiler_filename());
	if (FileAccess::get_sha256(library_path) != GODOT_SLANG_DLL_SHA256) {
		if (r_error) {
			*r_error = "Missing or mismatched pinned Slang compiler: " + library_path;
		}
		return false;
	}
	if (OS::get_singleton()->open_dynamic_library(library_path, compiler_library) != OK) {
		if (r_error) {
			*r_error = "Unable to load Slang compiler: " + library_path;
		}
		return false;
	}
	void *symbol = nullptr;
	Error error = OS::get_singleton()->get_dynamic_library_symbol_handle(compiler_library, "slang_createGlobalSession2", symbol);
	using CreateSession = SlangResult (*)(const SlangGlobalSessionDesc *, slang::IGlobalSession **);
	SlangGlobalSessionDesc descriptor;
	if (error != OK || SLANG_FAILED(reinterpret_cast<CreateSession>(symbol)(&descriptor, global_session.writeRef()))) {
		if (r_error) {
			*r_error = "Unable to initialize the pinned Slang compiler API.";
		}
		global_session.setNull();
		OS::get_singleton()->close_dynamic_library(compiler_library);
		compiler_library = nullptr;
		return false;
	}
	return true;
}

static bool compilation_succeeded(SlangResult p_result, const Slang::ComPtr<slang::IBlob> &p_diagnostics, String *r_error) {
	if (p_diagnostics && r_error) {
		*r_error += String::utf8(static_cast<const char *>(p_diagnostics->getBufferPointer()), p_diagnostics->getBufferSize());
	}
	if (SLANG_FAILED(p_result) && r_error && r_error->is_empty()) {
		*r_error = "Slang compilation failed: " + itos(p_result);
	}
	return SLANG_SUCCEEDED(p_result);
}

Vector<uint8_t> compile_slang_shader(RenderingDeviceCommons::ShaderStage p_stage, const String &p_source, const RenderingShaderCompileRequest &p_request, String *r_error) {
	MutexLock lock(compiler_mutex);
	ERR_FAIL_INDEX_V(p_stage, RenderingDeviceCommons::SHADER_STAGE_MAX, Vector<uint8_t>());
	if (r_error) {
		r_error->clear();
	}
	if (p_request.target != RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_3 || p_request.spirv_version != RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_6 || p_request.compiler_identity != get_slang_shader_compiler_identity() || p_request.entry_points[p_stage].is_empty()) {
		if (r_error) {
			*r_error = "Invalid internal Slang compiler request.";
		}
		return {};
	}
	if (!initialize_compiler(r_error)) {
		return {};
	}

	slang::CompilerOptionEntry options[] = {
		{ slang::CompilerOptionName::EmitSpirvDirectly, { slang::CompilerOptionValueKind::Int, 1 } },
		{ slang::CompilerOptionName::VulkanUseGLLayout, { slang::CompilerOptionValueKind::Int, p_request.gl_layout } },
		{ slang::CompilerOptionName::VulkanUseEntryPointName, { slang::CompilerOptionValueKind::Int, 0 } },
		{ slang::CompilerOptionName::PreserveParameters, { slang::CompilerOptionValueKind::Int, p_request.preserve_parameters } },
		{ slang::CompilerOptionName::FloatingPointMode, { slang::CompilerOptionValueKind::Int, int32_t(p_request.precise_float ? SLANG_FLOATING_POINT_MODE_PRECISE : SLANG_FLOATING_POINT_MODE_DEFAULT) } },
		{ slang::CompilerOptionName::Optimization, { slang::CompilerOptionValueKind::Int, p_request.optimization_level } },
		{ slang::CompilerOptionName::DebugInformation, { slang::CompilerOptionValueKind::Int, int32_t(p_request.debug_info ? SLANG_DEBUG_INFO_LEVEL_STANDARD : SLANG_DEBUG_INFO_LEVEL_NONE) } },
	};
	slang::TargetDesc target;
	target.format = SLANG_SPIRV;
	target.profile = global_session->findProfile("spirv_1_6");
	target.floatingPointMode = p_request.precise_float ? SLANG_FLOATING_POINT_MODE_PRECISE : SLANG_FLOATING_POINT_MODE_DEFAULT;
	Slang::ComPtr<ISlangFileSystem> filesystem;
	filesystem.attach(memnew(EmbeddedShaderFileSystem(p_request.includes)));
	slang::SessionDesc descriptor;
	descriptor.targets = &target;
	descriptor.targetCount = 1;
	descriptor.compilerOptionEntries = options;
	descriptor.compilerOptionEntryCount = sizeof(options) / sizeof(options[0]);
	descriptor.defaultMatrixLayoutMode = p_request.column_major ? SLANG_MATRIX_LAYOUT_COLUMN_MAJOR : SLANG_MATRIX_LAYOUT_ROW_MAJOR;
	descriptor.fileSystem = filesystem;
	Slang::ComPtr<slang::ISession> session;
	Slang::ComPtr<slang::IBlob> diagnostics;
	if (!compilation_succeeded(global_session->createSession(descriptor, session.writeRef()), diagnostics, r_error)) {
		return {};
	}
	CharString source = p_source.utf8();
	CharString source_path = p_request.source_path.utf8();
	slang::IModule *module = session->loadModuleFromSourceString("godot_shader", source_path.get_data(), source.get_data(), diagnostics.writeRef());
	if (!compilation_succeeded(module ? SLANG_OK : SLANG_FAIL, diagnostics, r_error)) {
		return {};
	}
	const SlangStage stages[] = { SLANG_STAGE_VERTEX, SLANG_STAGE_FRAGMENT, SLANG_STAGE_HULL, SLANG_STAGE_DOMAIN, SLANG_STAGE_COMPUTE, SLANG_STAGE_RAY_GENERATION, SLANG_STAGE_ANY_HIT, SLANG_STAGE_CLOSEST_HIT, SLANG_STAGE_MISS, SLANG_STAGE_INTERSECTION, SLANG_STAGE_MESH };
	Slang::ComPtr<slang::IEntryPoint> entry;
	CharString entry_name = p_request.entry_points[p_stage].utf8();
	diagnostics.setNull();
	if (!compilation_succeeded(module->findAndCheckEntryPoint(entry_name.get_data(), stages[p_stage], entry.writeRef(), diagnostics.writeRef()), diagnostics, r_error)) {
		return {};
	}
	slang::IComponentType *components[] = { module, entry };
	Slang::ComPtr<slang::IComponentType> program;
	diagnostics.setNull();
	if (!compilation_succeeded(session->createCompositeComponentType(components, 2, program.writeRef(), diagnostics.writeRef()), diagnostics, r_error)) {
		return {};
	}
	Slang::ComPtr<slang::IComponentType> linked_program;
	diagnostics.setNull();
	if (!compilation_succeeded(program->link(linked_program.writeRef(), diagnostics.writeRef()), diagnostics, r_error)) {
		return {};
	}
	Slang::ComPtr<slang::IBlob> code;
	diagnostics.setNull();
	if (!compilation_succeeded(linked_program->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef()), diagnostics, r_error)) {
		return {};
	}
	Vector<uint8_t> result;
	result.resize(code->getBufferSize());
	memcpy(result.ptrw(), code->getBufferPointer(), result.size());
	if ((p_stage == RenderingDeviceCommons::SHADER_STAGE_VERTEX || p_stage == RenderingDeviceCommons::SHADER_STAGE_MESH) && p_request.invariant_position) {
		const uint32_t *words = reinterpret_cast<const uint32_t *>(result.ptr());
		size_t word_count = result.size() / sizeof(uint32_t);
		size_t position_offset = 0;
		uint32_t position_id = 0;
		uint32_t position_member = 0;
		bool member_decoration = false;
		if (result.size() % sizeof(uint32_t) != 0 || word_count < 5 || words[0] != SpvMagicNumber) {
			if (r_error) {
				*r_error = "Invalid Slang pre-raster SPIR-V header.";
			}
			return {};
		}
		for (size_t offset = 5; offset < word_count;) {
			uint32_t count = words[offset] >> 16;
			uint32_t opcode = words[offset] & 0xffffu;
			if (count == 0 || count > word_count - offset) {
				if (r_error) {
					*r_error = "Invalid Slang pre-raster SPIR-V instruction length.";
				}
				return {};
			}
			if (opcode == SpvOpDecorate && count == 4 && words[offset + 2] == SpvDecorationBuiltIn && words[offset + 3] == SpvBuiltInPosition) {
				position_offset = offset;
				position_id = words[offset + 1];
				member_decoration = false;
			} else if (opcode == SpvOpMemberDecorate && count == 5 && words[offset + 3] == SpvDecorationBuiltIn && words[offset + 4] == SpvBuiltInPosition) {
				position_offset = offset;
				position_id = words[offset + 1];
				position_member = words[offset + 2];
				member_decoration = true;
			}
			offset += count;
		}
		if (position_id == 0) {
			if (r_error) {
				*r_error = "Slang pre-raster output has no BuiltIn Position decoration for the required invariant contract.";
			}
			return {};
		}
		for (size_t offset = 5; offset < word_count; offset += words[offset] >> 16) {
			if (!member_decoration && words[offset] == ((3u << 16) | SpvOpDecorate) && words[offset + 1] == position_id && words[offset + 2] == SpvDecorationInvariant) {
				return result;
			}
			if (member_decoration && words[offset] == ((4u << 16) | SpvOpMemberDecorate) && words[offset + 1] == position_id && words[offset + 2] == position_member && words[offset + 3] == SpvDecorationInvariant) {
				return result;
			}
		}
		uint32_t decoration_size = member_decoration ? 4 : 3;
		result.resize(result.size() + decoration_size * sizeof(uint32_t));
		uint32_t *output = reinterpret_cast<uint32_t *>(result.ptrw());
		memmove(output + position_offset + decoration_size, output + position_offset, (word_count - position_offset) * sizeof(uint32_t));
		output[position_offset] = (decoration_size << 16) | (member_decoration ? SpvOpMemberDecorate : SpvOpDecorate);
		output[position_offset + 1] = position_id;
		if (member_decoration) {
			output[position_offset + 2] = position_member;
		}
		output[position_offset + decoration_size - 1] = SpvDecorationInvariant;
	}
	return result;
}

void finalize_slang_shader_compiler() {
	MutexLock lock(compiler_mutex);
	global_session.setNull();
	if (compiler_library) {
		OS::get_singleton()->close_dynamic_library(compiler_library);
		compiler_library = nullptr;
	}
}
