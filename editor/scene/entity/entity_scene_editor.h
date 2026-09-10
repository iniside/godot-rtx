#pragma once

#include "scene/entity/entity_scene_commands.h"
#include "scene/gui/box_container.h"

class Tree;
class LineEdit;
class Label;
class Button;
class ScrollContainer;
class Camera3D;
class EditorResourcePicker;

class EntitySceneEditor : public VBoxContainer {
	GDCLASS(EntitySceneEditor, VBoxContainer);
	static EntitySceneEditor *singleton;
	Ref<EntityScene> document;
	EntityId selected;
	uint64_t revision = 0;
	Vector<EntityId> entities;
	Vector<EntityId> filtered;
	HashMap<EntityId, String, EntityIdHasher> names;
	Tree *tree = nullptr;
	LineEdit *filter = nullptr;
	Label *page_label = nullptr;
	Button *previous = nullptr;
	Button *next = nullptr;
	ScrollContainer *inspector = nullptr;
	VBoxContainer *fields = nullptr;
	Label *error_label = nullptr;
	String field_error;
	int page = 0;
	bool rebuilding = false;
	struct PendingField {
		Ref<EntityScene> document;
		EntityId entity;
		Variant value;
		uint64_t component = 0;
		uint64_t field = 0;
		Array path;
		bool active = false;
	};
	PendingField pending_number;
	PendingField pending_text;
	HashMap<uint64_t, bool> folded_components;
	static constexpr int PAGE_SIZE = 256;

	void _refresh_catalog();
	void _filter_changed(const String &p_text);
	void _page(int p_delta);
	void _refresh_page();
	void _selected();
	void _select_deferred(const String &p_id, Ref<EntityScene> p_document);
	void _inspect();
	void _add_field(VBoxContainer *p_parent, const String &p_label, const Variant &p_value, uint64_t p_component, uint64_t p_field, const Array &p_path, const EntityFieldSchema *p_schema = nullptr);
	void _change(const Variant &p_value, uint64_t p_component, uint64_t p_field, const Array &p_path, Ref<EntityScene> p_document = Ref<EntityScene>(), EntityId p_entity = EntityId());
	void _number_changed(double p_value, uint64_t p_component, uint64_t p_field, Array p_path, bool p_integer);
	void _text_changed(const String &p_value, uint64_t p_component, uint64_t p_field, Array p_path, Ref<EntityScene> p_document, const String &p_entity);
	void _bool_changed(bool p_value, uint64_t p_component, uint64_t p_field, Array p_path);
	void _resource_changed(Ref<Resource> p_resource, uint64_t p_component, uint64_t p_field, Array p_path, EditorResourcePicker *p_picker);
	void _resource_selected(Ref<Resource> p_resource, bool p_inspect);
	void _resize_array(uint64_t p_component, uint64_t p_field, Array p_path, Variant p_default, int p_delta);
	void _flush_number();
	void _flush_text();
	void _component_folded(bool p_folded, uint64_t p_component);
	Error _restore(Ref<EntityScene> p_document, Dictionary p_records, Dictionary p_prefabs);
	void _history_changed();
	void _document_changed();

protected:
	void _notification(int p_what);

public:
	static EntitySceneEditor *get_singleton() { return singleton; }
	void commit_pending_edits();
	void select(EntityId p_entity);
	void pick(Camera3D *p_camera, const Vector2 &p_position);
	EntitySceneEditor();
	~EntitySceneEditor();
};
