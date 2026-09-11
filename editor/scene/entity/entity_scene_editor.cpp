#include "entity_scene_editor.h"

#include "core/input/input.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/docks/inspector_dock.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_resource_picker.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/foldable_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/tree.h"
#include "scene/main/viewport.h"

EntitySceneEditor *EntitySceneEditor::singleton = nullptr;

void EntitySceneEditor::_refresh_catalog() {
	entities.clear();
	names.clear();
	if (document.is_valid()) {
		for (EntityId id : document->get_catalog().get_ids()) {
			EntityResolution target = document->resolve(id);
			if (target.state == EntityReferenceState::DELETED || target.state == EntityReferenceState::MISSING) {
				continue;
			}
			const EntityName *name = target.state == EntityReferenceState::RESIDENT ? document->get_world()->get<EntityName>(target.handle) : nullptr;
			names[id] = name && !name->name.is_empty() ? name->name : id.to_string();
			entities.push_back(id);
		}
		struct EntityOrder {
			bool operator()(const EntityId &p_a, const EntityId &p_b) const {
				return p_a.high == p_b.high ? p_a.low < p_b.low : p_a.high < p_b.high;
			}
		};
		entities.sort_custom<EntityOrder>();
	}
	_filter_changed(filter->get_text());
}

void EntitySceneEditor::_filter_changed(const String &p_text) {
	filtered.clear();
	for (EntityId id : entities) {
		if (p_text.is_empty() || names[id].containsn(p_text) || id.to_string().containsn(p_text)) {
			filtered.push_back(id);
		}
	}
	page = MAX(0, filtered.find(selected) / PAGE_SIZE);
	_refresh_page();
}

void EntitySceneEditor::_page(int p_delta) {
	page = CLAMP(page + p_delta, 0, MAX(0, (filtered.size() - 1) / PAGE_SIZE));
	_refresh_page();
}

void EntitySceneEditor::_refresh_page() {
	rebuilding = true;
	tree->clear();
	TreeItem *root = tree->create_item();
	const int begin = page * PAGE_SIZE;
	const int end = MIN(begin + PAGE_SIZE, filtered.size());
	for (int i = begin; i < end; i++) {
		TreeItem *item = tree->create_item(root);
		item->set_text(0, names[filtered[i]]);
		item->set_metadata(0, filtered[i].to_string());
		item->set_tooltip_text(0, filtered[i].to_string());
		if (filtered[i] == selected) {
			item->select(0);
		}
	}
	page_label->set_text(vformat("%d–%d / %d", filtered.is_empty() ? 0 : begin + 1, end, filtered.size()));
	previous->set_disabled(page == 0);
	next->set_disabled(end >= filtered.size());
	rebuilding = false;
}

void EntitySceneEditor::_selected() {
	if (rebuilding || !tree->get_selected()) {
		return;
	}
	callable_mp(this, &EntitySceneEditor::_select_deferred).bind(String(tree->get_selected()->get_metadata(0)), document).call_deferred();
}

void EntitySceneEditor::_select_deferred(const String &p_id, Ref<EntityScene> p_document) {
	if (document != p_document || EditorNode::get_editor_data().get_scene_document() != p_document) {
		return;
	}
	EntityId id;
	if (EntityId::parse(p_id, id) == OK) {
		select(id);
	}
}

void EntitySceneEditor::select(EntityId p_entity) {
	commit_pending_edits();
	field_error = String();
	if (document.is_null()) {
		return;
	}
	if (p_entity.is_valid() && document->pin(Vector<EntityId>{ p_entity }) != OK) {
		return;
	}
	if (selected.is_valid()) {
		document->unpin(Vector<EntityId>{ selected });
	}
	selected = p_entity;
	int index = filtered.find(selected);
	if (selected.is_valid() && index < 0) {
		filter->set_text(String());
		_filter_changed(String());
		index = filtered.find(selected);
	}
	if (index >= 0) {
		page = index / PAGE_SIZE;
	}
	_refresh_page();
	if (tree->get_selected()) {
		tree->scroll_to_item(tree->get_selected());
	}
	_inspect();
}

void EntitySceneEditor::_add_field(VBoxContainer *p_parent, const String &p_label, const Variant &p_value, uint64_t p_component, uint64_t p_field, const Array &p_path, const EntityFieldSchema *p_schema) {
	if (p_value.get_type() == Variant::DICTIONARY && p_schema && p_schema->nested_type_id) {
		const EntityComponentSchema *nested = document->get_world()->get_schemas().find(p_schema->nested_type_id);
		if (nested) {
			FoldableContainer *group = memnew(FoldableContainer(p_label.capitalize()));
			p_parent->add_child(group);
			VBoxContainer *children = memnew(VBoxContainer);
			group->add_child(children);
			Dictionary values = p_value;
			for (const EntityFieldSchema &field : nested->fields) {
				String key = String::num_uint64(field.id, 16);
				if (field.editable && values.has(key)) {
					Array path = p_path.duplicate();
					path.push_back(key);
					_add_field(children, String(field.name).capitalize(), values[key], p_component, p_field, path, &field);
				}
			}
			return;
		}
	}
	if (p_value.get_type() == Variant::ARRAY) {
		HBoxContainer *row = memnew(HBoxContainer);
		p_parent->add_child(row);
		Label *label = memnew(Label(p_label));
		label->set_h_size_flags(SIZE_EXPAND_FILL);
		row->add_child(label);
		Variant default_value;
		bool can_add = p_schema && p_schema->make_array_element && p_schema->make_array_element(default_value) == OK;
		can_add = can_add && default_value.get_type() != Variant::NIL && (default_value.get_type() != Variant::DICTIONARY || p_schema->nested_type_id != 0);
		Button *add = memnew(Button("Add"));
		row->add_child(add);
		add->set_disabled(!can_add);
		if (!can_add) {
			add->set_tooltip_text("The declared element type has no native field editor.");
		}
		add->connect("pressed", callable_mp(this, &EntitySceneEditor::_resize_array).bind(p_component, p_field, p_path, default_value, 1));
		Button *remove = memnew(Button("Remove last"));
		row->add_child(remove);
		remove->set_disabled(Array(p_value).is_empty());
		remove->connect("pressed", callable_mp(this, &EntitySceneEditor::_resize_array).bind(p_component, p_field, p_path, default_value, -1));
		if (Array(p_value).is_empty()) {
			return;
		}
	}
	int count = 0;
	switch (p_value.get_type()) {
		case Variant::VECTOR2:
			count = 2;
			break;
		case Variant::VECTOR3:
		case Variant::BASIS:
			count = 3;
			break;
		case Variant::VECTOR4:
		case Variant::COLOR:
			count = 4;
			break;
		case Variant::ARRAY:
			count = Array(p_value).size();
			break;
		default:
			break;
	}
	if (count > 0) {
		for (int i = 0; i < count; i++) {
			Array path = p_path.duplicate();
			path.push_back(i);
			const String axes[] = { "X", "Y", "Z", "W" };
			const String channels[] = { "R", "G", "B", "A" };
			String name = p_value.get_type() == Variant::ARRAY ? "[" + itos(i) + "]" : (p_value.get_type() == Variant::COLOR ? channels[i] : axes[i]);
			_add_field(p_parent, p_label + " / " + name, p_value.get(i), p_component, p_field, path, p_schema);
		}
		return;
	}
	if (p_value.get_type() == Variant::AABB || p_value.get_type() == Variant::RECT2) {
		for (const String &key : { String("position"), String("size") }) {
			Array path = p_path.duplicate();
			path.push_back(key);
			_add_field(p_parent, p_label + " / " + key.capitalize(), p_value.get(key), p_component, p_field, path);
		}
		return;
	}
	HBoxContainer *row = memnew(HBoxContainer);
	p_parent->add_child(row);
	Label *label = memnew(Label(p_label));
	label->set_h_size_flags(SIZE_EXPAND_FILL);
	label->set_clip_text(true);
	label->set_tooltip_text(p_label);
	row->add_child(label);
	if (p_value.get_type() == Variant::BOOL) {
		CheckBox *value = memnew(CheckBox);
		value->set_pressed(p_value);
		row->add_child(value);
		value->connect("toggled", callable_mp(this, &EntitySceneEditor::_bool_changed).bind(p_component, p_field, p_path));
	} else if (p_value.is_num()) {
		SpinBox *value = memnew(SpinBox);
		value->set_custom_minimum_size(Vector2(110, 0));
		value->set_step(p_value.get_type() == Variant::INT ? 1.0 : 0.001);
		value->set_allow_greater(true);
		value->set_allow_lesser(true);
		if (p_schema && p_schema->has_range) {
			value->set_min(p_schema->minimum);
			value->set_max(p_schema->maximum);
			value->set_allow_greater(false);
			value->set_allow_lesser(false);
		}
		value->set_value(p_value);
		row->add_child(value);
		value->connect("value_changed", callable_mp(this, &EntitySceneEditor::_number_changed).bind(p_component, p_field, p_path, p_value.get_type() == Variant::INT));
	} else if (p_value.get_type() == Variant::STRING && p_schema && p_schema->asset_reference) {
		EditorResourcePicker *picker = memnew(EditorResourcePicker);
		String native_type = p_schema->native_type;
		int begin = native_type.find("Ref<");
		int end = native_type.find(">", begin);
		picker->set_base_type(begin >= 0 && end > begin ? native_type.substr(begin + 4, end - begin - 4) : "Resource");
		picker->set_h_size_flags(SIZE_EXPAND_FILL);
		picker->set_custom_minimum_size(Vector2(130, 0));
		Ref<Resource> resource;
		entity_decode_asset(p_value, resource);
		picker->set_edited_resource(resource);
		row->add_child(picker);
		picker->connect("resource_changed", callable_mp(this, &EntitySceneEditor::_resource_changed).bind(p_component, p_field, p_path, picker));
		picker->connect("resource_selected", callable_mp(this, &EntitySceneEditor::_resource_selected));
	} else if (p_value.get_type() == Variant::STRING) {
		LineEdit *value = memnew(LineEdit);
		value->set_custom_minimum_size(Vector2(130, 0));
		value->set_h_size_flags(SIZE_EXPAND_FILL);
		value->set_text(p_value);
		row->add_child(value);
		value->connect("text_changed", callable_mp(this, &EntitySceneEditor::_text_changed).bind(p_component, p_field, p_path, document, selected.to_string()));
		value->connect("text_submitted", callable_mp(this, &EntitySceneEditor::_flush_text).unbind(1));
		value->connect("focus_exited", callable_mp(this, &EntitySceneEditor::_flush_text), CONNECT_DEFERRED);
	} else {
		row->add_child(memnew(Label(p_value.get_type() == Variant::ARRAY ? "Empty array" : "Unsupported field type")));
	}
}

void EntitySceneEditor::_inspect() {
	if (!inspector) {
		inspector = memnew(ScrollContainer);
		inspector->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
		inspector->set_v_size_flags(SIZE_EXPAND_FILL);
		fields = memnew(VBoxContainer);
		fields->set_h_size_flags(SIZE_EXPAND_FILL);
		inspector->add_child(fields);
		InspectorDock::get_singleton()->set_native_editor(inspector);
	}
	for (int i = fields->get_child_count() - 1; i >= 0; i--) {
		Node *child = fields->get_child(i);
		fields->remove_child(child);
		child->queue_free();
	}
	error_label = memnew(Label(field_error));
	error_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	fields->add_child(error_label);
	EntityResolution target = document->resolve(selected);
	if (target.state != EntityReferenceState::RESIDENT) {
		error_label->set_text("Select an entity in the scene list or viewport.");
		InspectorDock::get_singleton()->show_native_editor();
		return;
	}
	fields->add_child(memnew(Label(names.has(selected) ? names[selected] : selected.to_string())));
	EntityWorld *world = document->get_world();
	Vector<uint64_t> components;
	for (const KeyValue<uint64_t, EntityComponentSchema> &entry : world->get_schemas().get_types()) {
		if (entry.value.is_component && world->has_component(target.handle, entry.key)) {
			components.push_back(entry.key);
		}
	}
	components.sort();
	for (uint64_t id : components) {
		const EntityComponentSchema *schema = world->get_schemas().find(id);
		FoldableContainer *group = memnew(FoldableContainer(String(schema->name).trim_prefix("Entity").capitalize()));
		group->set_folded(folded_components.has(id) ? folded_components[id] : id != EntityComponentTraits<EntityName>::id && id != EntityComponentTraits<EntityTransform>::id);
		group->connect("folding_changed", callable_mp(this, &EntitySceneEditor::_component_folded).bind(id));
		fields->add_child(group);
		VBoxContainer *children = memnew(VBoxContainer);
		group->add_child(children);
		for (const EntityFieldSchema &field : schema->fields) {
			if (!field.editable) {
				continue;
			}
			Variant value;
			if (world->read_field(target.handle, id, field.id, value) == OK) {
				_add_field(children, String(field.name).capitalize(), value, id, field.id, Array(), &field);
			}
		}
	}
	InspectorDock::get_singleton()->show_native_editor();
}

static bool set_nested_value(Variant &r_value, const Array &p_path, int p_index, const Variant &p_value) {
	if (p_index == p_path.size()) {
		r_value = p_value;
		return true;
	}
	bool valid = false;
	Variant nested = r_value.get(p_path[p_index], &valid);
	if (!valid || !set_nested_value(nested, p_path, p_index + 1, p_value)) {
		return false;
	}
	r_value.set(p_path[p_index], nested, &valid);
	return valid;
}

void EntitySceneEditor::_change(const Variant &p_value, uint64_t p_component, uint64_t p_field, const Array &p_path, Ref<EntityScene> p_document, EntityId p_entity) {
	Ref<EntityScene> target_document = p_document.is_valid() ? p_document : document;
	EntityId target_entity = p_entity.is_valid() ? p_entity : selected;
	if (target_document.is_null()) {
		return;
	}
	EntityResolution target = target_document->resolve(target_entity);
	if (target.state != EntityReferenceState::RESIDENT) {
		return;
	}
	EntitySceneCommands::Command command;
	command.document = target_document->get_document_id();
	command.entity = target_entity;
	command.component = p_component;
	command.field = p_field;
	Error error = target_document->get_world()->read_field(target.handle, p_component, p_field, command.before);
	command.after = command.before.duplicate(true);
	if (error != OK || !set_nested_value(command.after, p_path, 0, p_value) || command.before == command.after) {
		return;
	}
	EditorData &editor_data = EditorNode::get_editor_data();
	int history_id = EditorUndoRedoManager::INVALID_HISTORY;
	for (int i = 0; i < editor_data.get_edited_scene_count(); i++) {
		if (editor_data.get_scene_document(i) == target_document) {
			history_id = editor_data.get_scene_history_id(i);
			break;
		}
	}
	ERR_FAIL_COND(history_id == EditorUndoRedoManager::INVALID_HISTORY);
	EntitySceneCommands::Transaction transaction;
	error = target_document->get_commands().execute("Edit entity field", Vector<EntitySceneCommands::Command>{ command }, nullptr, &transaction);
	if (error != OK) {
		field_error = vformat("Edit rejected (%d): %s", error, target_document->get_last_error());
		error_label->set_text(field_error);
		callable_mp(this, &EntitySceneEditor::_inspect).call_deferred();
		return;
	}
	field_error = String();
	error_label->set_text(String());
	revision = target_document->get_revision();
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	manager->create_action_for_history("Edit entity field", history_id);
	manager->set_native_action(callable_mp(this, &EntitySceneEditor::_restore).bind(target_document, transaction.before, transaction.prefabs_before), callable_mp(this, &EntitySceneEditor::_restore).bind(target_document, transaction.after, transaction.prefabs_after));
	UndoRedo *history = manager->get_history_undo_redo(history_id);
	history->add_do_method(callable_mp(this, &EntitySceneEditor::_history_changed));
	history->add_undo_method(callable_mp(this, &EntitySceneEditor::_history_changed));
	manager->commit_action(false);
	if (target_document == document && p_component == EntityComponentTraits<EntityName>::id) {
		callable_mp(this, &EntitySceneEditor::_refresh_catalog).call_deferred();
	}
}

void EntitySceneEditor::_number_changed(double p_value, uint64_t p_component, uint64_t p_field, Array p_path, bool p_integer) {
	if (Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
		pending_number = { document, selected, p_integer ? Variant(int64_t(p_value)) : Variant(p_value), p_component, p_field, p_path, true };
	} else {
		_change(p_integer ? Variant(int64_t(p_value)) : Variant(p_value), p_component, p_field, p_path);
	}
}

void EntitySceneEditor::_text_changed(const String &p_value, uint64_t p_component, uint64_t p_field, Array p_path, Ref<EntityScene> p_document, const String &p_entity) {
	EntityId entity;
	if (EntityId::parse(p_entity, entity) != OK) {
		return;
	}
	if (pending_text.active && (pending_text.document != p_document || pending_text.entity != entity || pending_text.component != p_component || pending_text.field != p_field || pending_text.path != p_path)) {
		_flush_text();
	}
	pending_text = { p_document, entity, p_value, p_component, p_field, p_path, true };
}

void EntitySceneEditor::_bool_changed(bool p_value, uint64_t p_component, uint64_t p_field, Array p_path) {
	_change(p_value, p_component, p_field, p_path);
}

void EntitySceneEditor::_resource_changed(Ref<Resource> p_resource, uint64_t p_component, uint64_t p_field, Array p_path, EditorResourcePicker *p_picker) {
	Variant encoded;
	if (entity_encode_asset(p_resource, encoded) != OK) {
		field_error = "Save the resource to the project before assigning it to an entity.";
		error_label->set_text(field_error);
		Variant previous_value;
		EntityResolution target = document->resolve(selected);
		if (target.state == EntityReferenceState::RESIDENT && document->get_world()->read_field(target.handle, p_component, p_field, previous_value) == OK) {
			for (int i = 0; i < p_path.size(); i++) {
				previous_value = previous_value.get(p_path[i]);
			}
			Ref<Resource> previous_resource;
			entity_decode_asset(previous_value, previous_resource);
			p_picker->set_edited_resource(previous_resource);
		}
		return;
	}
	_change(encoded, p_component, p_field, p_path);
}

void EntitySceneEditor::_resource_selected(Ref<Resource> p_resource, bool p_inspect) {
	if (p_resource.is_valid()) {
		InspectorDock::get_singleton()->edit_resource(p_resource);
	}
}

void EntitySceneEditor::_resize_array(uint64_t p_component, uint64_t p_field, Array p_path, Variant p_default, int p_delta) {
	EntityResolution target = document->resolve(selected);
	Variant value;
	if (target.state != EntityReferenceState::RESIDENT || document->get_world()->read_field(target.handle, p_component, p_field, value) != OK) {
		return;
	}
	for (int i = 0; i < p_path.size(); i++) {
		value = value.get(p_path[i]);
	}
	Array array = Array(value).duplicate(true);
	if (p_delta > 0) {
		array.push_back(p_default);
	} else if (!array.is_empty()) {
		array.resize(array.size() - 1);
	}
	_change(array, p_component, p_field, p_path);
	callable_mp(this, &EntitySceneEditor::_inspect).call_deferred();
}

void EntitySceneEditor::_flush_number() {
	if (pending_number.active) {
		PendingField pending = pending_number;
		pending_number = PendingField();
		_change(pending.value, pending.component, pending.field, pending.path, pending.document, pending.entity);
	}
}

void EntitySceneEditor::_flush_text() {
	if (pending_text.active) {
		PendingField pending = pending_text;
		pending_text = PendingField();
		_change(pending.value, pending.component, pending.field, pending.path, pending.document, pending.entity);
	}
}

void EntitySceneEditor::commit_pending_edits() {
	_flush_number();
	_flush_text();
	Control *focus = fields && fields->is_inside_tree() ? fields->get_viewport()->gui_get_focus_owner() : nullptr;
	SpinBox *spin_box = focus ? Object::cast_to<SpinBox>(focus->get_parent()) : nullptr;
	if (spin_box && fields->is_ancestor_of(spin_box)) {
		spin_box->apply();
		_flush_number();
	}
}

void EntitySceneEditor::_component_folded(bool p_folded, uint64_t p_component) {
	folded_components[p_component] = p_folded;
}

Error EntitySceneEditor::_restore(Ref<EntityScene> p_document, Dictionary p_records, Dictionary p_prefabs) {
	EntitySceneCommands::Transaction transaction;
	transaction.before = p_records;
	transaction.prefabs_before = p_prefabs;
	return p_document->get_commands().restore_transaction(transaction, false);
}

void EntitySceneEditor::_history_changed() {
	callable_mp(this, &EntitySceneEditor::_document_changed).call_deferred();
}

void EntitySceneEditor::_document_changed() {
	if (document.is_valid()) {
		revision = document->get_revision();
		const uint64_t catalog_begin = OS::get_singleton()->get_ticks_usec();
		_refresh_catalog();
		const uint64_t catalog_end = OS::get_singleton()->get_ticks_usec();
		_inspect();
		const uint64_t inspect_end = OS::get_singleton()->get_ticks_usec();
		if (OS::get_singleton()->is_use_benchmark_set()) {
			const double to_ms = 1.0 / 1000.0;
			print_line(vformat("EntitySceneEditor document changed: entities=%d refresh_catalog=%.2fms inspect=%.2fms",
					entities.size(),
					double(catalog_end - catalog_begin) * to_ms,
					double(inspect_end - catalog_end) * to_ms));
		}
	}
}

void EntitySceneEditor::_notification(int p_what) {
	if (p_what != NOTIFICATION_PROCESS) {
		return;
	}
	if (!Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
		_flush_number();
	}
	EditorData &editor_data = EditorNode::get_editor_data();
	if (editor_data.get_edited_scene_count() == 0) {
		return;
	}
	Ref<EntityScene> next_document = editor_data.get_scene_document();
	if (next_document == document) {
		return;
	}
	commit_pending_edits();
	if (document.is_valid() && selected.is_valid()) {
		document->unpin(Vector<EntityId>{ selected });
	}
	document = next_document;
	selected = EntityId();
	if (document.is_valid()) {
		_document_changed();
	}
}

void EntitySceneEditor::pick(Camera3D *p_camera, const Vector2 &p_position) {
	if (document.is_null()) {
		return;
	}
	Vector3 origin = p_camera->project_position(p_position, p_camera->get_near());
	Vector3 segment = p_camera->project_position(p_position, p_camera->get_far()) - origin;
	real_t distance = segment.length();
	if (Math::is_zero_approx(distance)) {
		return;
	}
	Vector3 direction = segment / distance;
	EntityId nearest;
	EntityWorld *world = document->get_world();
	world->query<EntityMesh, EntityTransform>().each([&](flecs::entity p_entity, const EntityMesh &p_mesh, const EntityTransform &p_transform) {
		EntityHandle handle = world->get_handle(p_entity);
		const EntityVisibility *visibility = world->get<EntityVisibility>(handle);
		if (!p_mesh.visible || p_mesh.mesh.is_null() || !(p_mesh.layers & p_camera->get_cull_mask()) || (visibility && !visibility->effective)) {
			return;
		}
		const EntityPose &pose = p_transform.current;
		if (Math::is_zero_approx(pose.basis.determinant())) {
			return;
		}
		Basis inverse = pose.basis.inverse();
		Vector3 local_origin = inverse.xform(Vector3(double(origin.x) - pose.translation.x, double(origin.y) - pose.translation.y, double(origin.z) - pose.translation.z));
		Vector3 local_direction = inverse.xform(direction);
		Vector3 local_end = local_origin + local_direction * distance;
		if (!p_mesh.mesh->get_aabb().intersects_segment(local_origin, local_end)) {
			return;
		}
		Ref<TriangleMesh> triangles = p_mesh.mesh->generate_triangle_mesh();
		Vector3 point;
		Vector3 normal;
		if (triangles.is_valid() && triangles->intersect_segment(local_origin, local_end, point, normal)) {
			real_t hit_distance = pose.basis.xform(point - local_origin).dot(direction);
			if (hit_distance >= 0.0 && hit_distance <= distance && world->is_alive(handle)) {
				distance = hit_distance;
				nearest = world->get_id(handle);
			}
		}
	});
	select(nearest);
}

EntitySceneEditor::EntitySceneEditor() {
	singleton = this;
	set_v_size_flags(SIZE_EXPAND_FILL);
	filter = memnew(LineEdit);
	filter->set_placeholder("Filter entities");
	add_child(filter);
	filter->connect("text_changed", callable_mp(this, &EntitySceneEditor::_filter_changed));
	tree = memnew(Tree);
	tree->set_hide_root(true);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(tree);
	tree->connect("item_selected", callable_mp(this, &EntitySceneEditor::_selected));
	HBoxContainer *paging = memnew(HBoxContainer);
	add_child(paging);
	previous = memnew(Button("Previous"));
	paging->add_child(previous);
	previous->connect("pressed", callable_mp(this, &EntitySceneEditor::_page).bind(-1));
	page_label = memnew(Label);
	page_label->set_h_size_flags(SIZE_EXPAND_FILL);
	paging->add_child(page_label);
	next = memnew(Button("Next"));
	paging->add_child(next);
	next->connect("pressed", callable_mp(this, &EntitySceneEditor::_page).bind(1));
	set_process(true);
}

EntitySceneEditor::~EntitySceneEditor() {
	if (document.is_valid() && selected.is_valid()) {
		document->unpin(Vector<EntityId>{ selected });
	}
	singleton = nullptr;
}
