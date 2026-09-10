/**************************************************************************/
/*  skeleton_3d_editor_plugin.cpp                                         */
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

#include "skeleton_3d_editor_plugin.h"

#include "editor/editor_string_names.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/resources/material.h"
#include "scene/resources/surface_tool.h"

Ref<ArrayMesh> create_skeleton_rest_mesh(Skeleton3D *p_skeleton) {
	Color bone_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/skeleton");
	real_t bone_axis_length = EDITOR_GET("editors/3d_gizmos/gizmo_settings/bone_axis_length");
	int bone_shape = EDITOR_GET("editors/3d_gizmos/gizmo_settings/bone_shape");

	LocalVector<Color> axis_colors;
	axis_colors.push_back(Node3DEditor::get_singleton()->get_theme_color(SNAME("axis_x_color"), EditorStringName(Editor)));
	axis_colors.push_back(Node3DEditor::get_singleton()->get_theme_color(SNAME("axis_y_color"), EditorStringName(Editor)));
	axis_colors.push_back(Node3DEditor::get_singleton()->get_theme_color(SNAME("axis_z_color"), EditorStringName(Editor)));

	Ref<SurfaceTool> surface_tool(memnew(SurfaceTool));
	surface_tool->begin(Mesh::PRIMITIVE_LINES);

	Ref<ShaderMaterial> material;
	material.instantiate();
	Ref<Shader> selected_sh = Ref<Shader>(memnew(Shader));
	selected_sh->set_code(R"(

shader_type spatial;
render_mode unshaded, shadows_disabled;

void vertex() {
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)), COLOR.rgb * (1.0 / 12.92), lessThan(COLOR.rgb,vec3(0.04045)));
	}
	VERTEX = VERTEX;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * MODEL_MATRIX * vec4(VERTEX.xyz, 1.0);
	POSITION.z = mix(POSITION.z, POSITION.w, 0.998);
}

void fragment() {
	ALBEDO = COLOR.rgb;
	ALPHA = COLOR.a;
}
)");
	material->set_shader(selected_sh);
	surface_tool->set_material(material);

	int current_bone_index = 0;
	Vector<int> bones_to_process = p_skeleton->get_parentless_bones();

	while (bones_to_process.size() > current_bone_index) {
		int current_bone_idx = bones_to_process[current_bone_index];
		current_bone_index++;

		Color current_bone_color = bone_color;

		Vector<int> child_bones_vector;
		child_bones_vector = p_skeleton->get_bone_children(current_bone_idx);
		int child_bones_size = child_bones_vector.size();

		for (int i = 0; i < child_bones_size; i++) {
			if (child_bones_vector[i] < 0) {
				continue;
			}

			int child_bone_idx = child_bones_vector[i];

			Vector3 v0 = p_skeleton->get_bone_global_rest(current_bone_idx).origin;
			Vector3 v1 = p_skeleton->get_bone_global_rest(child_bone_idx).origin;
			Vector3 d = (v1 - v0).normalized();
			real_t dist = v0.distance_to(v1);
			int closest = -1;
			real_t closest_d = 0.0;
			for (int j = 0; j < 3; j++) {
				real_t dp = Math::abs(p_skeleton->get_bone_global_rest(current_bone_idx).basis[j].normalized().dot(d));
				if (j == 0 || dp > closest_d) {
					closest = j;
				}
			}
			switch (bone_shape) {
				case 0: { // Wire shape.
					surface_tool->set_color(current_bone_color);
					surface_tool->add_vertex(v0);
					surface_tool->add_vertex(v1);
				} break;

				case 1: { // Octahedron shape.
					Vector3 first;
					Vector3 points[6];
					int point_idx = 0;
					for (int j = 0; j < 3; j++) {
						Vector3 axis;
						if (first == Vector3()) {
							axis = d.cross(d.cross(p_skeleton->get_bone_global_rest(current_bone_idx).basis[j])).normalized();
							first = axis;
						} else {
							axis = d.cross(first).normalized();
						}

						surface_tool->set_color(current_bone_color);
						for (int k = 0; k < 2; k++) {
							if (k == 1) {
								axis = -axis;
							}
							Vector3 point = v0 + d * dist * 0.2;
							point += axis * dist * 0.1;
							surface_tool->add_vertex(v0);
							surface_tool->add_vertex(point);
							surface_tool->add_vertex(point);
							surface_tool->add_vertex(v1);
							points[point_idx++] = point;
						}
					}
					surface_tool->set_color(current_bone_color);
					SWAP(points[1], points[2]);
					for (int j = 0; j < 6; j++) {
						surface_tool->add_vertex(points[j]);
						surface_tool->add_vertex(points[(j + 1) % 6]);
					}
				} break;
			}
			for (int j = 0; j < 3; j++) {
				surface_tool->set_color(axis_colors[j]);
				surface_tool->add_vertex(v0);
				surface_tool->add_vertex(v0 + (p_skeleton->get_bone_global_rest(current_bone_idx).basis.inverse())[j].normalized() * dist * bone_axis_length);

				if (j == closest) {
					continue;
				}
			}
			if (i == child_bones_size - 1) {
				for (int j = 0; j < 3; j++) {
					surface_tool->set_color(axis_colors[j]);
					surface_tool->add_vertex(v1);
					surface_tool->add_vertex(v1 + (p_skeleton->get_bone_global_rest(child_bone_idx).basis.inverse())[j].normalized() * dist * bone_axis_length);

					if (j == closest) {
						continue;
					}
				}
			}
			bones_to_process.push_back(child_bones_vector[i]);
		}
	}

	return surface_tool->commit();
}
