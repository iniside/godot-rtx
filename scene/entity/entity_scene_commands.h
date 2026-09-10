#pragma once

#include "scene/resources/entity_scene.h"

class EntitySceneCommands {
	friend class EntityScene;
	friend class EntitySceneIO;
public:
	enum Kind {
		CREATE,
		DELETE,
		DUPLICATE,
		REPARENT,
		SET_FIELD,
		ADD_COMPONENT,
		REMOVE_COMPONENT,
		SET_ORDER,
	};
	struct Command {
		EntityId document;
		EntityId entity;
		Kind kind = SET_FIELD;
		uint64_t component = 0;
		uint64_t field = 0;
		Variant before;
		Variant after;
		EntityId parent;
		EntityWorld::ReparentMode reparent_mode = EntityWorld::KEEP_WORLD;
	};

	struct Transaction {
		String name;
		Dictionary before;
		Dictionary after;
		Dictionary prefabs_before;
		Dictionary prefabs_after;
	};
private:
	using History = Transaction;
	EntityScene &document;
	Vector<History> history;
	int cursor = 0;

	Error _snapshot(EntityScene &p_scene, const Vector<EntityId> &p_ids, Dictionary &r_records, bool p_prefer_stored = false);
	Error _restore(const Dictionary &p_records, const Dictionary &p_prefabs);
	Error _apply(EntityScene &p_scene, const Command &p_command, Vector<EntityId> &r_changed, Dictionary &r_remap);
	Error _remap_record(Dictionary &r_record, const Dictionary &p_remap);
	Error _remap_fields(uint64_t p_type, Dictionary &r_fields, const Dictionary &p_remap);
	Error _refresh_instance(EntityScene &p_target, EntityId p_instance, EntityScene &p_source, Vector<EntityId> &r_changed, EntityScene *p_source_changes = nullptr);
	Error _prefab_record(EntityId p_id, Dictionary &r_record, bool &r_found);
	Error _reconcile_prefab_catalog();
	Error _override_record(Dictionary &r_record, const Dictionary &p_instance, const String &p_source);
	void _push(History p_history);

public:
	explicit EntitySceneCommands(EntityScene &p_document) : document(p_document) {}
	Error execute(const String &p_name, const Vector<Command> &p_commands, Dictionary *r_remap = nullptr, Transaction *r_transaction = nullptr);
	Error restore_transaction(const Transaction &p_transaction, bool p_forward);
	Error undo();
	Error redo();
	bool can_undo() const { return cursor > 0; }
	bool can_redo() const { return cursor < history.size(); }
	void clear();
	Error instantiate_prefab(const Ref<EntityScene> &p_prefab, EntityId &r_instance);
	Error refresh_prefab(EntityId p_instance, const Ref<EntityScene> &p_prefab);
	Error revert_override(EntityId p_instance, int p_override, const Ref<EntityScene> &p_prefab);
	Error apply_overrides(EntityId p_instance, const Ref<EntityScene> &p_prefab, const Vector<Ref<EntityScene>> &p_users);
	Dictionary get_prefab_instances() const { return document.prefab_instances.duplicate(true); }
};
