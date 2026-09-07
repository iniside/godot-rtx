"""Functions used to generate source files during build time"""

import os.path
import re

from methods import generated_wrapper, print_error, to_raw_cstring


RD_HEADER_INCLUDE_ROOTS = {
    "Rtxdi/": "thirdparty/rtxdi/Include",
}


class RDHeaderStruct:
    def __init__(self):
        self.vertex_lines = []
        self.fragment_lines = []
        self.compute_lines = []
        self.raygen_lines = []
        self.any_hit_lines = []
        self.closest_hit_lines = []
        self.miss_lines = []
        self.intersection_lines = []

        self.vertex_included_files = []
        self.fragment_included_files = []
        self.compute_included_files = []
        self.raygen_included_files = []
        self.any_hit_included_files = []
        self.closest_hit_included_files = []
        self.miss_included_files = []
        self.intersection_included_files = []

        self.reading = ""
        self.line_offset = 0
        self.vertex_offset = 0
        self.fragment_offset = 0
        self.compute_offset = 0
        self.raygen_offset = 0
        self.any_hit_offset = 0
        self.closest_hit_offset = 0
        self.miss_offset = 0
        self.intersection_offset = 0


def include_file_in_rd_header(filename: str, header_data: RDHeaderStruct, depth: int) -> RDHeaderStruct:
    with open(filename, "r", encoding="utf-8") as fs:
        line = fs.readline()

        while line:
            index = line.find("//")
            if index != -1:
                line = line[:index]

            if line.find("#[vertex]") != -1:
                header_data.reading = "vertex"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.vertex_offset = header_data.line_offset
                continue

            if line.find("#[fragment]") != -1:
                header_data.reading = "fragment"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.fragment_offset = header_data.line_offset
                continue

            if line.find("#[compute]") != -1:
                header_data.reading = "compute"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.compute_offset = header_data.line_offset
                continue

            if line.find("#[raygen]") != -1:
                header_data.reading = "raygen"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.raygen_offset = header_data.line_offset
                continue

            if line.find("#[any_hit]") != -1:
                header_data.reading = "any_hit"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.any_hit_offset = header_data.line_offset
                continue

            if line.find("#[closest_hit]") != -1:
                header_data.reading = "closest_hit"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.closest_hit_offset = header_data.line_offset
                continue

            if line.find("#[miss]") != -1:
                header_data.reading = "miss"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.miss_offset = header_data.line_offset
                continue

            if line.find("#[intersection]") != -1:
                header_data.reading = "intersection"
                line = fs.readline()
                header_data.line_offset += 1
                header_data.intersection_offset = header_data.line_offset
                continue

            while line.find("#include ") != -1:
                includeline = line.replace("#include ", "").strip()[1:-1]

                include_root = next(
                    (root for prefix, root in RD_HEADER_INCLUDE_ROOTS.items() if includeline.startswith(prefix)), None
                )

                if include_root is not None:
                    included_file = os.path.relpath(os.path.join(include_root, includeline))
                elif includeline.startswith("thirdparty/"):
                    included_file = os.path.relpath(includeline)

                else:
                    included_file = os.path.relpath(os.path.dirname(filename) + "/" + includeline)

                if included_file not in header_data.vertex_included_files and header_data.reading == "vertex":
                    header_data.vertex_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif included_file not in header_data.fragment_included_files and header_data.reading == "fragment":
                    header_data.fragment_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif included_file not in header_data.compute_included_files and header_data.reading == "compute":
                    header_data.compute_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif included_file not in header_data.raygen_included_files and header_data.reading == "raygen":
                    header_data.raygen_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif included_file not in header_data.any_hit_included_files and header_data.reading == "any_hit":
                    header_data.any_hit_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif (
                    included_file not in header_data.closest_hit_included_files and header_data.reading == "closest_hit"
                ):
                    header_data.closest_hit_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif included_file not in header_data.miss_included_files and header_data.reading == "miss":
                    header_data.miss_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')
                elif (
                    included_file not in header_data.intersection_included_files
                    and header_data.reading == "intersection"
                ):
                    header_data.intersection_included_files += [included_file]
                    if include_file_in_rd_header(included_file, header_data, depth + 1) is None:
                        print_error(f'In file "{filename}": #include "{includeline}" could not be found!"')

                line = fs.readline()

            line = line.replace("\r", "").replace("\n", "")

            if header_data.reading == "vertex":
                header_data.vertex_lines += [line]
            if header_data.reading == "fragment":
                header_data.fragment_lines += [line]
            if header_data.reading == "compute":
                header_data.compute_lines += [line]
            if header_data.reading == "raygen":
                header_data.raygen_lines += [line]
            if header_data.reading == "any_hit":
                header_data.any_hit_lines += [line]
            if header_data.reading == "closest_hit":
                header_data.closest_hit_lines += [line]
            if header_data.reading == "miss":
                header_data.miss_lines += [line]
            if header_data.reading == "intersection":
                header_data.intersection_lines += [line]

            line = fs.readline()
            header_data.line_offset += 1

    return header_data


def build_rd_header_lines_for_raytracing_stage(lines, stage: str):
    if lines:
        return f"""\
		static const char _{stage}_code[] = {{
{to_raw_cstring(lines)}
		}};
"""
    else:
        return f"""\
		static const char *_{stage}_code = nullptr;
"""


def build_rd_header(filename: str, shader: str) -> None:
    shader_path = os.path.normpath(shader).replace(os.sep, "/")
    header_data = RDHeaderStruct()
    slang_includes = {}
    if shader.endswith(".slang"):
        lines, slang_includes = read_slang_sources(shader)
        stage = ""
        for line_number, line in enumerate(lines, 1):
            match = re.fullmatch(r"#\[(\w+)\]", line.strip())
            if match:
                stage = match[1]
                getattr(header_data, stage + "_lines").append(f'#line {line_number + 1} "/godot/{shader_path}"')
            elif stage:
                getattr(header_data, stage + "_lines").append(line)
    else:
        include_file_in_rd_header(shader, header_data, 0)
    class_name = os.path.splitext(os.path.basename(shader))[0].title().replace("_", "").replace(".", "") + "ShaderRD"

    with generated_wrapper(filename) as file:
        file.write(f"""\
#include "servers/rendering/renderer_rd/shader_rd.h"

class {class_name} : public ShaderRD {{
public:
	{class_name}() {{
""")

        if (
            header_data.raygen_lines
            or header_data.any_hit_lines
            or header_data.closest_hit_lines
            or header_data.miss_lines
            or header_data.intersection_lines
        ):
            file.write(build_rd_header_lines_for_raytracing_stage(header_data.raygen_lines, "raygen"))
            file.write(build_rd_header_lines_for_raytracing_stage(header_data.any_hit_lines, "any_hit"))
            file.write(build_rd_header_lines_for_raytracing_stage(header_data.closest_hit_lines, "closest_hit"))
            file.write(build_rd_header_lines_for_raytracing_stage(header_data.miss_lines, "miss"))
            file.write(build_rd_header_lines_for_raytracing_stage(header_data.intersection_lines, "intersection"))
            file.write(f"""\
		setup_raytracing(_raygen_code, _any_hit_code, _closest_hit_code, _miss_code, _intersection_code, "{class_name}");
""")
        elif header_data.compute_lines:
            file.write(f"""\
		static const char *_vertex_code = nullptr;
		static const char *_fragment_code = nullptr;
		static const char _compute_code[] = {{
{to_raw_cstring(header_data.compute_lines)}
		}};
		setup(_vertex_code, _fragment_code, _compute_code, "{class_name}");
""")
        else:
            file.write(f"""\
		static const char _vertex_code[] = {{
{to_raw_cstring(header_data.vertex_lines)}
		}};
		static const char _fragment_code[] = {{
{to_raw_cstring(header_data.fragment_lines)}
		}};
		static const char *_compute_code = nullptr;
		setup(_vertex_code, _fragment_code, _compute_code, "{class_name}");
""")

        if shader.endswith(".slang"):
            file.write(f'\t\tsetup_slang("/godot/{shader_path}");\n')
            for index, (path, contents) in enumerate(sorted(slang_includes.items())):
                file.write(f'\t\tstatic const char _include_{index}[] = {{\n{to_raw_cstring(contents)}\n\t\t}};\n')
                file.write(f'\t\tsetup_slang_include("/godot/{path}", _include_{index});\n')

        file.write("""\
	}
};
""")


def build_rd_headers(target, source, env):
    env.NoCache(target)
    for src in source:
        build_rd_header(f"{src}.gen.h", str(src))


def read_slang_sources(shader):
    includes = {}

    def read_file(path):
        path = os.path.normpath(path).replace(os.sep, "/")
        with open(path, encoding="utf-8") as source:
            lines = source.read().splitlines()
        for index, line in enumerate(lines):
            match = re.match(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
            if not match:
                continue
            name = match[1]
            root = next((root for prefix, root in RD_HEADER_INCLUDE_ROOTS.items() if name.startswith(prefix)), None)
            included = os.path.normpath(os.path.join(root or os.path.dirname(path), name)).replace(os.sep, "/")
            if name.startswith("thirdparty/"):
                included = name
            lines[index] = f'#include "/godot/{included}"'
            if included not in includes:
                includes[included] = []
                includes[included] = read_file(included)
        return lines

    return read_file(shader), includes


def rd_slang_dependencies(target, source, env):
    for shader in source:
        _, includes = read_slang_sources(shader.abspath)
        env.Depends(target, [env.File(os.path.abspath(path)) for path in includes] + [env.File("#glsl_builders.py")])
    return target, source


class RAWHeaderStruct:
    def __init__(self):
        self.code = ""


def include_file_in_raw_header(filename: str, header_data: RAWHeaderStruct, depth: int) -> None:
    with open(filename, "r", encoding="utf-8") as fs:
        line = fs.readline()

        while line:
            while line.find("#include ") != -1:
                includeline = line.replace("#include ", "").strip()[1:-1]

                included_file = os.path.relpath(os.path.dirname(filename) + "/" + includeline)
                include_file_in_raw_header(included_file, header_data, depth + 1)

                line = fs.readline()

            header_data.code += line
            line = fs.readline()


def build_raw_header(filename: str, shader: str) -> None:
    include_file_in_raw_header(shader, header_data := RAWHeaderStruct(), 0)

    with generated_wrapper(filename) as file:
        file.write(f"""\
static const char {os.path.basename(shader).replace(".glsl", "_shader_glsl")}[] = {{
{to_raw_cstring(header_data.code)}
}};
""")


def build_raw_headers(target, source, env):
    env.NoCache(target)
    for src in source:
        build_raw_header(f"{src}.gen.h", str(src))
