/**************************************************************************/
/*  mesh_editor_plugin.cpp                                                */
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

#include "mesh_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

void MeshEditor::gui_input(const Ref<InputEvent> &p_event) {
	ERR_FAIL_COND(p_event.is_null());

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && (mm->get_button_mask().has_flag(MouseButtonMask::LEFT))) {
		rot_x -= mm->get_relative().y * 0.01;
		rot_y -= mm->get_relative().x * 0.01;

		rot_x = CLAMP(rot_x, -Math::PI / 2, Math::PI / 2);
		_update_rotation();
	}
}

void MeshEditor::_update_theme_item_cache() {
	SubViewportContainer::_update_theme_item_cache();

	theme_cache.light_1_icon = get_editor_theme_icon(SNAME("MaterialPreviewLight1"));
	theme_cache.light_2_icon = get_editor_theme_icon(SNAME("MaterialPreviewLight2"));
}

void MeshEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POST_ENTER_TREE: {
			RS::get_singleton()->viewport_set_scenario(viewport->get_viewport_rid(), scenario);
			RS::get_singleton()->viewport_attach_camera(viewport->get_viewport_rid(), camera);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			light_1_switch->set_button_icon(theme_cache.light_1_icon);
			light_2_switch->set_button_icon(theme_cache.light_2_icon);
		} break;
	}
}

void MeshEditor::_update_rotation() {
	Transform3D t;
	t.basis.rotate(Vector3(0, 1, 0), -rot_y);
	t.basis.rotate(Vector3(1, 0, 0), -rot_x);
	mesh_instance.transform = t * mesh_transform;
	mesh_instance.publish();
}

void MeshEditor::edit(Ref<Mesh> p_mesh) {
	mesh = p_mesh;
	mesh_instance.base = mesh->get_rid();
	mesh_instance.base_asset = mesh;
	mesh_transform = Transform3D();

	rot_x = Math::deg_to_rad(-15.0);
	rot_y = Math::deg_to_rad(30.0);

	AABB aabb = mesh->get_aabb();
	Vector3 ofs = aabb.get_center();
	float m = aabb.get_longest_axis_size();
	if (m != 0) {
		m = 1.0 / m;
		m *= 0.5;
		Transform3D xform;
		xform.basis.scale(Vector3(m, m, m));
		xform.origin = -xform.basis.xform(ofs); //-ofs*m;
		//xform.origin.z -= aabb.get_longest_axis_size() * 2;
		mesh_transform = xform;
	}
	_update_rotation();
}

void MeshEditor::_on_light_1_switch_pressed() {
	light1.visible = light_1_switch->is_pressed();
	light1.publish();
}

void MeshEditor::_on_light_2_switch_pressed() {
	light2.visible = light_2_switch->is_pressed();
	light2.publish();
}

MeshEditor::MeshEditor() {
	viewport = memnew(SubViewport);
	add_child(viewport);
	viewport->set_disable_input(true);
	viewport->set_msaa_3d(Viewport::MSAA_4X);
	set_stretch(true);
	RenderingServer *rs = RS::get_singleton();
	scenario = rs->scenario_create();
	camera = rs->camera_create();
	rs->camera_set_transform(camera, Transform3D(Basis(), Vector3(0, 0, 1.1)));
	rs->camera_set_perspective(camera, 45, 0.1, 10);
	if (GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units")) {
		camera_attributes.instantiate();
		rs->camera_set_camera_attributes(camera, camera_attributes->get_rid());
	}
	light1 = ToolRenderData::create(rs->directional_light_create(), scenario);
	light1.transform = Transform3D().looking_at(Vector3(-1, -1, -1), Vector3(0, 1, 0));
	rs->light_set_param(light1.base, RSE::LIGHT_PARAM_INTENSITY, 100000.0);
	light1.publish();
	light2 = ToolRenderData::create(rs->directional_light_create(), scenario);
	light2.transform = Transform3D().looking_at(Vector3(0, 1, 0), Vector3(0, 0, 1));
	rs->light_set_color(light2.base, Color(0.7, 0.7, 0.7));
	rs->light_set_param(light2.base, RSE::LIGHT_PARAM_INTENSITY, 100000.0);
	light2.publish();
	mesh_instance = ToolRenderData::create(RID(), scenario);

	set_custom_minimum_size(Size2(1, 150) * EDSCALE);

	HBoxContainer *hb = memnew(HBoxContainer);
	add_child(hb);
	hb->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT, Control::PRESET_MODE_MINSIZE, 2);

	hb->add_spacer();

	VBoxContainer *vb_light = memnew(VBoxContainer);
	hb->add_child(vb_light);

	light_1_switch = memnew(Button);
	light_1_switch->set_theme_type_variation("PreviewLightButton");
	light_1_switch->set_toggle_mode(true);
	light_1_switch->set_pressed(true);
	light_1_switch->set_accessibility_name(TTRC("First Light"));
	vb_light->add_child(light_1_switch);
	light_1_switch->connect(SceneStringName(pressed), callable_mp(this, &MeshEditor::_on_light_1_switch_pressed));

	light_2_switch = memnew(Button);
	light_2_switch->set_theme_type_variation("PreviewLightButton");
	light_2_switch->set_toggle_mode(true);
	light_2_switch->set_pressed(true);
	light_2_switch->set_accessibility_name(TTRC("Second Light"));
	vb_light->add_child(light_2_switch);
	light_2_switch->connect(SceneStringName(pressed), callable_mp(this, &MeshEditor::_on_light_2_switch_pressed));

	rot_x = 0;
	rot_y = 0;

	EditorNode::get_singleton()->register_hdr_viewport(viewport);
}

MeshEditor::~MeshEditor() {
	RID light1_base = light1.base;
	RID light2_base = light2.base;
	mesh_instance.clear();
	light1.clear();
	light2.clear();
	RS::get_singleton()->free_rid(light1_base);
	RS::get_singleton()->free_rid(light2_base);
	RS::get_singleton()->free_rid(camera);
	RS::get_singleton()->free_rid(scenario);
}

///////////////////////

bool EditorInspectorPluginMesh::can_handle(Object *p_object) {
	return Object::cast_to<Mesh>(p_object) != nullptr;
}

void EditorInspectorPluginMesh::parse_begin(Object *p_object) {
	Mesh *mesh = Object::cast_to<Mesh>(p_object);
	if (!mesh) {
		return;
	}
	Ref<Mesh> m(mesh);

	MeshEditor *editor = memnew(MeshEditor);
	editor->edit(m);
	add_custom_control(editor);
}

MeshEditorPlugin::MeshEditorPlugin() {
	Ref<EditorInspectorPluginMesh> plugin;
	plugin.instantiate();
	add_inspector_plugin(plugin);
}
