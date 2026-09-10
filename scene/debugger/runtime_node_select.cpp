/**************************************************************************/
/*  runtime_node_select.cpp                                               */
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

#ifdef DEBUG_ENABLED

#include "runtime_node_select.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/math/geometry_3d.h"
#include "core/object/callable_mp.h"
#include "scene/2d/camera_2d.h"
#include "scene/debugger/scene_debugger_object.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/view_panner.h"
#include "scene/main/canvas_layer.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/mesh.h"
#include "scene/theme/theme_db.h"
#include "servers/rendering/rendering_server.h"

#ifndef PHYSICS_2D_DISABLED
#include "scene/2d/physics/collision_object_2d.h"
#include "scene/2d/physics/collision_polygon_2d.h"
#include "scene/2d/physics/collision_shape_2d.h"
#endif // PHYSICS_2D_DISABLED


RuntimeNodeSelect *RuntimeNodeSelect::get_singleton() {
	return singleton;
}

RuntimeNodeSelect::~RuntimeNodeSelect() {
	if (selection_list && !selection_list->is_visible()) {
		memdelete(selection_list);
	}

	if (draw_canvas.is_valid()) {
		RS::get_singleton()->free_rid(sel_drag_ci);
		RS::get_singleton()->free_rid(srect_ci);
		RS::get_singleton()->free_rid(draw_canvas);
	}
}

void RuntimeNodeSelect::_setup(const Dictionary &p_settings) {
	Window *root = SceneTree::get_singleton()->get_root();
	ERR_FAIL_COND(root->is_connected(SceneStringName(window_input), callable_mp(this, &RuntimeNodeSelect::_root_window_input)));

	root->connect(SceneStringName(window_input), callable_mp(this, &RuntimeNodeSelect::_root_window_input));
	root->connect("size_changed", callable_mp(this, &RuntimeNodeSelect::_queue_selection_update), CONNECT_DEFERRED);

	max_selection = p_settings.get("debugger/max_node_selection", 1);

	// Panner Setup

	panner.instantiate();
	panner->set_callbacks(callable_mp(this, &RuntimeNodeSelect::_pan_callback), callable_mp(this, &RuntimeNodeSelect::_zoom_callback));

	ViewPanner::ControlScheme panning_scheme = (ViewPanner::ControlScheme)p_settings.get("editors/panning/2d_editor_panning_scheme", 0).operator int();
	bool simple_panning = p_settings.get("editors/panning/simple_panning", false);
	int pan_speed = p_settings.get("editors/panning/2d_editor_pan_speed", 20);
	Array keys = p_settings.get("canvas_item_editor/pan_view", Array()).operator Array();
	panner->setup(panning_scheme, DebuggerMarshalls::deserialize_key_shortcut(keys), simple_panning);
	panner->setup_warped_panning(root, p_settings.get("editors/panning/warped_mouse_panning", true));
	panner->set_scroll_speed(pan_speed);

	sel_2d_grab_dist = p_settings.get("editors/polygon_editor/point_grab_radius", 0);
	sel_2d_scale = MAX(1, Math::ceil(2.0 / (float)GLOBAL_GET("display/window/stretch/scale")));

	selection_area_fill = p_settings.get("box_selection_fill_color", Color());
	selection_area_outline = p_settings.get("box_selection_stroke_color", Color());

	draw_canvas = RS::get_singleton()->canvas_create();
	sel_drag_ci = RS::get_singleton()->canvas_item_create();

	/// 2D Selection Rectangle Generation

	srect_color = p_settings.get("editors/2d/selection_rectangle_color", Color());

	srect_ci = RS::get_singleton()->canvas_item_create();
	RS::get_singleton()->viewport_attach_canvas(root->get_viewport_rid(), draw_canvas);
	RS::get_singleton()->canvas_item_set_parent(sel_drag_ci, draw_canvas);
	RS::get_singleton()->canvas_item_set_parent(srect_ci, draw_canvas);


	SceneTree::get_singleton()->connect("process_frame", callable_mp(this, &RuntimeNodeSelect::_process_frame));
	SceneTree::get_singleton()->connect("physics_frame", callable_mp(this, &RuntimeNodeSelect::_physics_frame));

	// This function will be called before the root enters the tree at first when the Game view is passing its settings to
	// the debugger, so queue the update for after it enters.
	root->connect(SceneStringName(tree_entered), callable_mp(this, &RuntimeNodeSelect::_update_input_state), Object::CONNECT_ONE_SHOT);
}

void RuntimeNodeSelect::_node_set_type(NodeType p_type) {
	ERR_FAIL_COND_MSG(p_type == NODE_TYPE_3D, "Node-based 3D runtime selection is unavailable for native entity worlds.");
	node_select_type = p_type;
	_update_input_state();
}

void RuntimeNodeSelect::_select_set_mode(SelectMode p_mode) {
	node_select_mode = p_mode;
}

void RuntimeNodeSelect::_set_camera_override_enabled(bool p_enabled) {
	camera_override = p_enabled;

	if (camera_first_override) {
		_reset_camera_2d();

		camera_first_override = false;
	} else if (p_enabled) {
		_update_view_2d();

	}
}

void RuntimeNodeSelect::_root_window_input(const Ref<InputEvent> &p_event) {
	Window *root = SceneTree::get_singleton()->get_root();
	if (node_select_type == NODE_TYPE_NONE || (selection_list && selection_list->is_visible())) {
		// Workaround for platforms that don't allow subwindows.
		if (selection_list && selection_list->is_visible() && selection_list->is_embedded()) {
			root->set_disable_input_override(false);
			selection_list->push_input(p_event);
			callable_mp(root->get_viewport(), &Viewport::set_disable_input_override).call_deferred(true);
		}

		return;
	}

	Ref<InputEventMouseButton> b = p_event;

	if (b.is_valid() && b->is_pressed()) {
		list_shortcut_pressed = node_select_mode == SELECT_MODE_SINGLE && b->get_button_index() == MouseButton::RIGHT && b->is_alt_pressed();
	}

	bool is_dragging_camera = false;
	if (camera_override && !list_shortcut_pressed) {
		if (node_select_type == NODE_TYPE_2D) {
			is_dragging_camera = panner->gui_input(p_event, Rect2(Vector2(), root->get_visible_rect().get_size()));
		}
	}

	if (selection_drag_state == SELECTION_DRAG_MOVE) {
		Ref<InputEventMouseMotion> m = p_event;
		if (m.is_valid()) {
			_update_selection_drag(root->get_screen_transform().affine_inverse().xform(m->get_position()));
			return;
		} else if (b.is_valid()) {
			// Account for actions like zooming.
			_update_selection_drag(root->get_screen_transform().affine_inverse().xform(b->get_position()));
		}
	}

	if (b.is_null()) {
		return;
	}

	// Ignore mouse wheel inputs.
	if (b->get_button_index() != MouseButton::LEFT && b->get_button_index() != MouseButton::RIGHT) {
		return;
	}

	if (selection_drag_state == SELECTION_DRAG_MOVE && !b->is_pressed() && b->get_button_index() == MouseButton::LEFT) {
		selection_drag_state = SELECTION_DRAG_END;
		selection_drag_area = selection_drag_area.abs();
		_update_selection_drag();

		// Trigger a selection in the position on release.
		if (multi_shortcut_pressed) {
			selection_position = root->get_screen_transform().affine_inverse().xform(b->get_position());
		}
	}

	if (!is_dragging_camera && b->is_pressed()) {
		multi_shortcut_pressed = b->is_shift_pressed();
		if (list_shortcut_pressed || b->get_button_index() == MouseButton::LEFT) {
			selection_position = root->get_screen_transform().affine_inverse().xform(b->get_position());
		}
	}
}

void RuntimeNodeSelect::_items_popup_index_pressed(int p_index, PopupMenu *p_popup) {
	Object *obj = p_popup->get_item_metadata(p_index).get_validated_object();
	if (obj) {
		Vector<Node *> node;
		node.append(Object::cast_to<Node>(obj));
		_send_ids(node);
	}
}

void RuntimeNodeSelect::_update_input_state() {
	SceneTree *scene_tree = SceneTree::get_singleton();
	// This function can be called at the very beginning, when the root hasn't entered the tree yet.
	// So check first to avoid a crash.
	if (!scene_tree->get_root()->is_inside_tree()) {
		return;
	}

	bool disable_input = scene_tree->is_suspended() || node_select_type != RuntimeNodeSelect::NODE_TYPE_NONE;
	Input::get_singleton()->set_disable_input(disable_input);
	Input::get_singleton()->set_mouse_mode_override_enabled(disable_input);
	scene_tree->get_root()->set_disable_input_override(disable_input);
}

void RuntimeNodeSelect::_process_frame() {

	if (selection_update_queued || !SceneTree::get_singleton()->is_suspended()) {
		selection_update_queued = false;
		if (has_selection) {
			_update_selection();
		}
	}
}

void RuntimeNodeSelect::_physics_frame() {
	if (selection_drag_state != SELECTION_DRAG_END && (selection_drag_state == SELECTION_DRAG_MOVE || !selection_position.is_finite())) {
		return;
	}

	Window *root = SceneTree::get_singleton()->get_root();
	bool selection_drag_valid = selection_drag_state == SELECTION_DRAG_END && selection_drag_area.get_area() > SELECTION_MIN_AREA;
	Vector<SelectResult> items;

	if (node_select_type == NODE_TYPE_2D) {
		if (selection_drag_valid) {
			for (int i = 0; i < root->get_child_count(); i++) {
				_find_canvas_items_at_rect(selection_drag_area, root->get_child(i), items);
			}
		} else if (selection_position.is_finite()) {
			for (int i = 0; i < root->get_child_count(); i++) {
				_find_canvas_items_at_pos(selection_position, root->get_child(i), items);
			}
		}

	}

	if ((prefer_group_selection || avoid_locked_nodes) && !list_shortcut_pressed && node_select_mode == SELECT_MODE_SINGLE) {
		for (int i = 0; i < items.size(); i++) {
			Node *node = items[i].item;
			Node *final_node = node;
			real_t order = items[i].order;

			// Replace the node by the group if grouped.
			if (prefer_group_selection) {
				while (node && node != root) {
					if (node->has_meta("_edit_group_")) {
						final_node = node;

						if (Object::cast_to<CanvasItem>(final_node)) {
							CanvasItem *ci_tmp = Object::cast_to<CanvasItem>(final_node);
							order = ci_tmp->get_effective_z_index() + ci_tmp->get_canvas_layer();
						}
					}
					node = node->get_parent();
				}
			}

			// Filter out locked nodes.
			if (avoid_locked_nodes && final_node->get_meta("_edit_lock_", false)) {
				items.remove_at(i);
				i--;
				continue;
			}

			items.write[i].item = final_node;
			items.write[i].order = order;
		}
	}

	// Remove possible duplicates.
	for (int i = 0; i < items.size(); i++) {
		Node *item = items[i].item;
		for (int j = 0; j < i; j++) {
			if (items[j].item == item) {
				items.remove_at(i);
				i--;

				break;
			}
		}
	}

	items.sort();

	switch (selection_drag_state) {
		case SELECTION_DRAG_END: {
			selection_position = Point2(Math::INF, Math::INF);
			selection_drag_state = SELECTION_DRAG_NONE;

			if (selection_drag_area.get_area() > SELECTION_MIN_AREA) {
				if (!items.is_empty()) {
					Vector<Node *> nodes;
					for (const SelectResult item : items) {
						nodes.append(item.item);
					}
					_send_ids(nodes, false);
				}

				_update_selection_drag();
				return;
			}

			_update_selection_drag();
		} break;

		case SELECTION_DRAG_NONE: {
			if (list_shortcut_pressed || node_select_mode == SELECT_MODE_LIST) {
				break;
			}

			if (multi_shortcut_pressed) {
				// Allow forcing box selection when an item was clicked.
				selection_drag_state = SELECTION_DRAG_MOVE;
			} else if (items.is_empty()) {
				if (!selected_ci_nodes.is_empty()) {
					EngineDebugger::get_singleton()->send_message("remote_nothing_selected", Array());
					_clear_selection();
				}

				selection_drag_state = SELECTION_DRAG_MOVE;
			} else {
				break;
			}

			[[fallthrough]];
		}

		case SELECTION_DRAG_MOVE: {
			selection_drag_area.position = selection_position;

			// Stop selection on click, so it can happen on release if the selection area doesn't pass the threshold.
			if (multi_shortcut_pressed) {
				return;
			}
		}
	}

	if (items.is_empty()) {
		selection_position = Point2(Math::INF, Math::INF);
		return;
	}
	if ((!list_shortcut_pressed && node_select_mode == SELECT_MODE_SINGLE) || items.size() == 1) {
		selection_position = Point2(Math::INF, Math::INF);

		Vector<Node *> node;
		node.append(items[0].item);
		_send_ids(node);

		return;
	}

	if (!selection_list && (list_shortcut_pressed || node_select_mode == SELECT_MODE_LIST)) {
		_open_selection_list(items, selection_position);
	}

	selection_position = Point2(Math::INF, Math::INF);
}

void RuntimeNodeSelect::_send_ids(const Vector<Node *> &p_picked_nodes, bool p_invert_new_selections) {
	ERR_FAIL_COND(p_picked_nodes.is_empty());

	Vector<Node *> picked_nodes = p_picked_nodes;
	Array message;

	if (!multi_shortcut_pressed) {
		if (picked_nodes.size() > max_selection) {
			picked_nodes.resize(max_selection);
			EngineDebugger::get_singleton()->send_message("show_selection_limit_warning", Array());
		}

		for (const Node *node : picked_nodes) {
			SceneDebuggerObject obj(node->get_instance_id());
			Array arr;
			obj.serialize(arr);
			message.append(arr);
		}

		EngineDebugger::get_singleton()->send_message("remote_objects_selected", message);
		_set_selected_nodes(picked_nodes);

		return;
	}

	LocalVector<Node *> nodes;
	LocalVector<ObjectID> ids;
	for (Node *node : picked_nodes) {
		ObjectID id = node->get_instance_id();
		if (CanvasItem *ci = Object::cast_to<CanvasItem>(node)) {
			if (selected_ci_nodes.has(id)) {
				if (p_invert_new_selections) {
					selected_ci_nodes.erase(id);
				}
			} else {
				ids.push_back(id);
				nodes.push_back(ci);
			}
		} else {
		}
	}

	uint32_t limit = max_selection - selected_ci_nodes.size();
	if (ids.size() > limit) {
		ids.resize(limit);
		nodes.resize(limit);
		EngineDebugger::get_singleton()->send_message("show_selection_limit_warning", Array());
	}

	for (ObjectID id : selected_ci_nodes) {
		ids.push_back(id);
		nodes.push_back(ObjectDB::get_instance<Node>(id));
	}

	if (ids.is_empty()) {
		EngineDebugger::get_singleton()->send_message("remote_nothing_selected", message);
	} else {
		for (const ObjectID &id : ids) {
			SceneDebuggerObject obj(id);
			Array arr;
			obj.serialize(arr);
			message.append(arr);
		}

		EngineDebugger::get_singleton()->send_message("remote_objects_selected", message);
	}

	_set_selected_nodes(Vector<Node *>(nodes));
}

void RuntimeNodeSelect::_set_selected_nodes(const Vector<Node *> &p_nodes) {
	if (p_nodes.is_empty()) {
		_clear_selection();
		return;
	}

	bool changed = false;
	LocalVector<ObjectID> nodes_ci;

	for (Node *node : p_nodes) {
		ObjectID id = node->get_instance_id();
		if (Object::cast_to<CanvasItem>(node)) {
			if (!changed || !selected_ci_nodes.has(id)) {
				changed = true;
			}

			nodes_ci.push_back(id);
		} else {
		}
	}

	if (!changed && nodes_ci.size() == selected_ci_nodes.size()) {
		return;
	}

	_clear_selection();
	selected_ci_nodes = nodes_ci;
	has_selection = !nodes_ci.is_empty();


	_queue_selection_update();
}

void RuntimeNodeSelect::_queue_selection_update() {
	if (has_selection && selection_visible) {
		if (SceneTree::get_singleton()->is_suspended()) {
			_update_selection();
		} else {
			selection_update_queued = true;
		}
	}
}

void RuntimeNodeSelect::_update_selection() {
	RS::get_singleton()->canvas_item_clear(srect_ci);
	RS::get_singleton()->canvas_item_set_visible(srect_ci, selection_visible);

	for (LocalVector<ObjectID>::Iterator E = selected_ci_nodes.begin(); E != selected_ci_nodes.end(); ++E) {
		ObjectID id = *E;
		CanvasItem *ci = ObjectDB::get_instance<CanvasItem>(id);
		if (!ci) {
			selected_ci_nodes.erase(id);
			--E;
			continue;
		}

		if (!ci->is_inside_tree()) {
			continue;
		}

		Transform2D xform = ci->get_global_transform_with_canvas();

		// Fallback.
		Rect2 rect = Rect2(Vector2(), Vector2(10, 10));

		if (ci->_edit_use_rect()) {
			rect = ci->_edit_get_rect();
		} else {
#ifndef PHYSICS_2D_DISABLED
			CollisionShape2D *collision_shape = Object::cast_to<CollisionShape2D>(ci);
			if (collision_shape) {
				Ref<Shape2D> shape = collision_shape->get_shape();
				if (shape.is_valid()) {
					rect = shape->get_rect();
				}
			}
#endif // PHYSICS_2D_DISABLED
		}

		const Vector2 endpoints[4] = {
			xform.xform(rect.position),
			xform.xform(rect.position + Point2(rect.size.x, 0)),
			xform.xform(rect.position + rect.size),
			xform.xform(rect.position + Point2(0, rect.size.y))
		};

		for (int i = 0; i < 4; i++) {
			RS::get_singleton()->canvas_item_add_line(srect_ci, endpoints[i], endpoints[(i + 1) % 4], srect_color, sel_2d_scale);
		}
	}

}

void RuntimeNodeSelect::_clear_selection() {
	selected_ci_nodes.clear();
	if (draw_canvas.is_valid()) {
		RS::get_singleton()->canvas_item_clear(srect_ci);
	}


	has_selection = false;
}

void RuntimeNodeSelect::_update_selection_drag(const Point2 &p_end_pos) {
	RS::get_singleton()->canvas_item_clear(sel_drag_ci);

	if (selection_drag_state != SELECTION_DRAG_MOVE) {
		return;
	}

	selection_drag_area.size = p_end_pos - selection_drag_area.position;

	if (selection_drag_state == SELECTION_DRAG_END) {
		return;
	}

	Rect2 selection_drawing = selection_drag_area.abs();
	int thickness = 1;

	const Vector2 endpoints[4] = {
		selection_drawing.position,
		selection_drawing.position + Point2(selection_drawing.size.x, 0),
		selection_drawing.position + selection_drawing.size,
		selection_drawing.position + Point2(0, selection_drawing.size.y)
	};

	// Draw fill.
	RS::get_singleton()->canvas_item_add_rect(sel_drag_ci, selection_drawing, selection_area_fill);
	// Draw outline.
	for (int i = 0; i < 4; i++) {
		RS::get_singleton()->canvas_item_add_line(sel_drag_ci, endpoints[i], endpoints[(i + 1) % 4], selection_area_outline, thickness);
	}
}

void RuntimeNodeSelect::_open_selection_list(const Vector<SelectResult> &p_items, const Point2 &p_pos) {
	Window *root = SceneTree::get_singleton()->get_root();

	selection_list = memnew(PopupMenu);
	selection_list->set_theme(ThemeDB::get_singleton()->get_default_theme());
	selection_list->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_DISABLED);
	selection_list->set_force_native(true);
	selection_list->connect("index_pressed", callable_mp(this, &RuntimeNodeSelect::_items_popup_index_pressed).bind(selection_list));
	selection_list->connect("popup_hide", callable_mp(this, &RuntimeNodeSelect::_close_selection_list));

	root->add_child(selection_list);

	for (const SelectResult &I : p_items) {
		int locked = 0;
		if (I.item->get_meta("_edit_lock_", false)) {
			locked = 1;
		} else {
			Node *scene = SceneTree::get_singleton()->get_root();
			Node *node = I.item;

			while (node && node != scene->get_parent()) {
				if (node->has_meta("_edit_group_")) {
					locked = 2;
				}
				node = node->get_parent();
			}
		}

		String suffix;
		if (locked == 1) {
			suffix = " (" + RTR("Locked") + ")";
		} else if (locked == 2) {
			suffix = " (" + RTR("Grouped") + ")";
		}

		selection_list->add_item((String)I.item->get_name() + suffix);
		selection_list->set_item_metadata(-1, I.item);
	}

	selection_list->set_position(selection_list->is_embedded() ? p_pos : (Input::get_singleton()->get_mouse_position() + root->get_position()));
	selection_list->reset_size();
	selection_list->popup();

	selection_list->set_content_scale_factor(1);
	selection_list->set_min_size(selection_list->get_contents_minimum_size());
	selection_list->reset_size();

	// FIXME: Ugly hack that stops the popup from hiding when the button is released.
	selection_list->call_deferred(SNAME("set_position"), selection_list->get_position() + Point2(1, 0));
}

void RuntimeNodeSelect::_close_selection_list() {
	selection_list->queue_free();
	selection_list = nullptr;
}

void RuntimeNodeSelect::_set_selection_visible(bool p_visible) {
	selection_visible = p_visible;

	if (has_selection) {
		_update_selection();
	}
}

void RuntimeNodeSelect::_set_avoid_locked(bool p_enabled) {
	avoid_locked_nodes = p_enabled;
}

void RuntimeNodeSelect::_set_prefer_group(bool p_enabled) {
	prefer_group_selection = p_enabled;
}

// Copied and trimmed from the CanvasItemEditor implementation.
void RuntimeNodeSelect::_find_canvas_items_at_pos(const Point2 &p_pos, Node *p_node, Vector<SelectResult> &r_items, const Transform2D &p_parent_xform, const Transform2D &p_canvas_xform) {
	if (!p_node || Object::cast_to<Viewport>(p_node)) {
		return;
	}

	CanvasItem *ci = Object::cast_to<CanvasItem>(p_node);
	for (int i = p_node->get_child_count() - 1; i >= 0; i--) {
		if (ci) {
			if (!ci->is_set_as_top_level()) {
				_find_canvas_items_at_pos(p_pos, p_node->get_child(i), r_items, p_parent_xform * ci->get_transform(), p_canvas_xform);
			} else {
				_find_canvas_items_at_pos(p_pos, p_node->get_child(i), r_items, ci->get_transform(), p_canvas_xform);
			}
		} else {
			CanvasLayer *cl = Object::cast_to<CanvasLayer>(p_node);
			_find_canvas_items_at_pos(p_pos, p_node->get_child(i), r_items, Transform2D(), cl ? cl->get_transform() : p_canvas_xform);
		}
	}

	if (!ci || !ci->is_visible_in_tree()) {
		return;
	}

	Transform2D xform = p_canvas_xform;
	if (!ci->is_set_as_top_level()) {
		xform *= p_parent_xform;
	}

	Window *root = SceneTree::get_singleton()->get_root();
	Point2 pos;

	// Cameras don't affect `CanvasLayer`s.
	if (!ci->get_canvas_layer_node() || ci->get_canvas_layer_node()->is_following_viewport()) {
		pos = root->get_canvas_transform().affine_inverse().xform(p_pos);
	} else {
		pos = p_pos;
	}

	xform = (xform * ci->get_transform()).affine_inverse();
	const real_t local_grab_distance = xform.basis_xform(Vector2(sel_2d_grab_dist, 0)).length() / view_2d_zoom;
	if (ci->_edit_is_selected_on_click(xform.xform(pos), local_grab_distance)) {
		SelectResult res;
		res.item = ci;
		res.order = ci->get_effective_z_index() + ci->get_canvas_layer();
		r_items.push_back(res);

#ifndef PHYSICS_2D_DISABLED
		// If it's a shape, get the collision object it's from.
		// FIXME: If the collision object has multiple shapes, only the topmost will be above it in the list.
		if (Object::cast_to<CollisionShape2D>(ci) || Object::cast_to<CollisionPolygon2D>(ci)) {
			CollisionObject2D *collision_object = Object::cast_to<CollisionObject2D>(ci->get_parent());
			if (collision_object) {
				SelectResult res_col;
				res_col.item = ci->get_parent();
				res_col.order = collision_object->get_z_index() + ci->get_canvas_layer();
				r_items.push_back(res_col);
			}
		}
#endif // PHYSICS_2D_DISABLED
	}
}

// Copied and trimmed from the CanvasItemEditor implementation.
void RuntimeNodeSelect::_find_canvas_items_at_rect(const Rect2 &p_rect, Node *p_node, Vector<SelectResult> &r_items, const Transform2D &p_parent_xform, const Transform2D &p_canvas_xform) {
	if (!p_node || Object::cast_to<Viewport>(p_node)) {
		return;
	}

	CanvasItem *ci = Object::cast_to<CanvasItem>(p_node);
	for (int i = p_node->get_child_count() - 1; i >= 0; i--) {
		if (ci) {
			if (!ci->is_set_as_top_level()) {
				_find_canvas_items_at_rect(p_rect, p_node->get_child(i), r_items, p_parent_xform * ci->get_transform(), p_canvas_xform);
			} else {
				_find_canvas_items_at_rect(p_rect, p_node->get_child(i), r_items, ci->get_transform(), p_canvas_xform);
			}
		} else {
			CanvasLayer *cl = Object::cast_to<CanvasLayer>(p_node);
			_find_canvas_items_at_rect(p_rect, p_node->get_child(i), r_items, Transform2D(), cl ? cl->get_transform() : p_canvas_xform);
		}
	}

	if (!ci || !ci->is_visible_in_tree()) {
		return;
	}

	Transform2D xform = p_canvas_xform;
	if (!ci->is_set_as_top_level()) {
		xform *= p_parent_xform;
	}

	Window *root = SceneTree::get_singleton()->get_root();
	Rect2 rect;
	// Cameras don't affect `CanvasLayer`s.
	if (!ci->get_canvas_layer_node() || ci->get_canvas_layer_node()->is_following_viewport()) {
		rect = root->get_canvas_transform().affine_inverse().xform(p_rect);
	} else {
		rect = p_rect;
	}
	rect = (xform * ci->get_transform()).affine_inverse().xform(rect);

	bool selected = false;
	if (ci->_edit_use_rect()) {
		Rect2 ci_rect = ci->_edit_get_rect();
		if (rect.has_point(ci_rect.position) &&
				rect.has_point(ci_rect.position + Vector2(ci_rect.size.x, 0)) &&
				rect.has_point(ci_rect.position + Vector2(ci_rect.size.x, ci_rect.size.y)) &&
				rect.has_point(ci_rect.position + Vector2(0, ci_rect.size.y))) {
			selected = true;
		}
	} else if (rect.has_point(Point2())) {
		selected = true;
	}

	if (selected) {
		SelectResult res;
		res.item = ci;
		res.order = ci->get_effective_z_index() + ci->get_canvas_layer();
		r_items.push_back(res);
	}
}

void RuntimeNodeSelect::_pan_callback(Vector2 p_scroll_vec, Ref<InputEvent> p_event) {
	Vector2 scroll = SceneTree::get_singleton()->get_root()->get_screen_transform().affine_inverse().xform(p_scroll_vec);
	view_2d_offset.x -= scroll.x / view_2d_zoom;
	view_2d_offset.y -= scroll.y / view_2d_zoom;

	_update_view_2d();
}

// A very shallow copy of the same function inside CanvasItemEditor.
void RuntimeNodeSelect::_zoom_callback(float p_zoom_factor, Vector2 p_origin, Ref<InputEvent> p_event) {
	real_t prev_zoom = view_2d_zoom;
	view_2d_zoom = CLAMP(view_2d_zoom * p_zoom_factor, VIEW_2D_MIN_ZOOM, VIEW_2D_MAX_ZOOM);

	Vector2 pos = SceneTree::get_singleton()->get_root()->get_screen_transform().affine_inverse().xform(p_origin);
	view_2d_offset += pos / prev_zoom - pos / view_2d_zoom;

	// We want to align in-scene pixels to screen pixels, this prevents blurry rendering
	// of small details (texts, lines).
	// This correction adds a jitter movement when zooming, so we correct only when the
	// zoom factor is an integer. (in the other cases, all pixels won't be aligned anyway)
	const real_t closest_zoom_factor = Math::round(view_2d_zoom);
	if (Math::is_zero_approx(view_2d_zoom - closest_zoom_factor)) {
		// Make sure scene pixel at view_offset is aligned on a screen pixel.
		Vector2 view_offset_int = view_2d_offset.floor();
		Vector2 view_offset_frac = view_2d_offset - view_offset_int;
		view_2d_offset = view_offset_int + (view_offset_frac * closest_zoom_factor).round() / closest_zoom_factor;
	}

	_update_view_2d();
}

void RuntimeNodeSelect::_reset_camera_2d() {
	camera_first_override = true;
	Window *root = SceneTree::get_singleton()->get_root();
	Camera2D *game_camera = root->is_camera_2d_override_enabled() ? root->get_overridden_camera_2d() : root->get_camera_2d();
	if (game_camera) {
		// Ideally we should be using Camera2D::get_camera_transform() but it's not so this hack will have to do for now.
		view_2d_offset = game_camera->get_camera_screen_center() - (0.5 * root->get_visible_rect().size);
	} else {
		view_2d_offset = Vector2();
	}

	view_2d_zoom = 1;

	if (root->is_camera_2d_override_enabled()) {
		_update_view_2d();
	}
}

void RuntimeNodeSelect::_update_view_2d() {
	Window *root = SceneTree::get_singleton()->get_root();
	ERR_FAIL_COND(!root->is_camera_2d_override_enabled());

	Camera2D *override_camera = root->get_override_camera_2d();
	ERR_FAIL_NULL(override_camera);
	override_camera->set_anchor_mode(Camera2D::ANCHOR_MODE_FIXED_TOP_LEFT);
	override_camera->set_zoom(Vector2(view_2d_zoom, view_2d_zoom));
	override_camera->set_offset(view_2d_offset);

	_queue_selection_update();
}


#endif // DEBUG_ENABLED
