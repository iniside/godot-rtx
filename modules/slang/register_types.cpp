#include "register_types.h"

#include "shader_compile.h"

void initialize_slang_module(ModuleInitializationLevel p_level) {
}

void uninitialize_slang_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_CORE) {
		finalize_slang_shader_compiler();
	}
}
