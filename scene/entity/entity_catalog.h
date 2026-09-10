#pragma once

#include "entity_id.h"

#include "core/templates/hash_map.h"

class EntityWorld;

class EntityCatalog {
	friend class EntityWorld;
	HashMap<EntityId, bool, EntityIdHasher> records;

public:
	Error add_record(EntityId p_id) {
		ERR_FAIL_COND_V(!p_id.is_valid(), ERR_INVALID_PARAMETER);
		ERR_FAIL_COND_V(records.has(p_id), ERR_ALREADY_EXISTS);
		records.insert(p_id, false);
		return OK;
	}

	EntityReferenceState get_state(EntityId p_id) const {
		const bool *deleted = records.getptr(p_id);
		return deleted ? (*deleted ? EntityReferenceState::DELETED : EntityReferenceState::UNLOADED) : EntityReferenceState::MISSING;
	}

	int get_record_count() const { return records.size(); }

	EntityCatalog() = default;
	EntityCatalog(const EntityCatalog &) = delete;
	EntityCatalog &operator=(const EntityCatalog &) = delete;
};
