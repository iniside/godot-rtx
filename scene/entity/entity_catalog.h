#pragma once

#include "entity_id.h"

#include "core/templates/a_hash_map.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class EntityWorld;

class EntityCatalog {
	friend class EntityWorld;
	friend class EntityScene;
	friend class EntitySceneCommands;
	friend class EntitySceneIO;
	struct Section {
		String path;
		bool cluster = false;
		String name;
		Dictionary record;
		Array components;
	};
	struct Record {
		bool deleted = false;
		EntityRef parent;
		int64_t order = 0;
		Section section;
		String cell_grid;
		int32_t cell_x = 0;
		int32_t cell_y = 0;
		int32_t cell_z = 0;
		bool has_order = false;
		bool has_section = false;
		bool has_cell = false;
		uint64_t snapshot_visit = 0;
	};
	using RecordMap = AHashMap<EntityId, Record, EntityIdHasher>;
	RecordMap records;
	HashMap<EntityId, HashSet<EntityId, EntityIdHasher>, EntityIdHasher> children;

	void _unlink_parent(EntityId p_id) {
		EntityId parent = records[p_id].parent.id;
		auto *siblings = children.getptr(parent);
		if (siblings) {
			siblings->erase(p_id);
			if (siblings->is_empty()) {
				children.erase(parent);
			}
		}
	}

	void _set_parent(EntityId p_id, EntityRef p_parent) {
		_unlink_parent(p_id);
		records[p_id].parent = p_parent;
		if (p_parent.id.is_valid()) {
			children[p_parent.id].insert(p_id);
		}
	}

public:
	Vector<EntityId> get_ids() const {
		Vector<EntityId> result;
		result.reserve(records.size());
		for (const KeyValue<EntityId, Record> &entry : records) {
			result.push_back(entry.key);
		}
		return result;
	}

	Vector<EntityId> get_children(EntityId p_id) const {
		Vector<EntityId> result;
		const auto *entries = children.getptr(p_id);
		if (entries) {
			for (EntityId id : *entries) {
				result.push_back(id);
			}
		}
		return result;
	}

	Error add_record(EntityId p_id, EntityRef p_parent = {}) {
		ERR_FAIL_COND_V(!p_id.is_valid(), ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(records.has(p_id), ERR_ALREADY_EXISTS);
		ERR_FAIL_COND_V(p_parent.id.is_valid() && get_state(p_parent.id) != EntityReferenceState::UNLOADED, ERR_INVALID_PARAMETER);
		records.insert(p_id, { false, p_parent });
		if (p_parent.id.is_valid()) {
			children[p_parent.id].insert(p_id);
		}
		return OK;
	}

	EntityReferenceState get_state(EntityId p_id) const {
		const Record *record = records.getptr(p_id);
		return record ? (record->deleted ? EntityReferenceState::DELETED : EntityReferenceState::UNLOADED) : EntityReferenceState::MISSING;
	}

	EntityRef get_parent(EntityId p_id) const {
		const Record *record = records.getptr(p_id);
		return record ? record->parent : EntityRef();
	}

	int get_record_count() const { return records.size(); }

	EntityCatalog() = default;
	EntityCatalog(const EntityCatalog &) = delete;
	EntityCatalog &operator=(const EntityCatalog &) = delete;
};
