#pragma once

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/hashfuncs.h"

struct EntityId {
	uint64_t high = 0;
	uint64_t low = 0;

	bool is_valid() const { return high != 0 || low != 0; }
	bool operator==(const EntityId &p_other) const { return high == p_other.high && low == p_other.low; }
	bool operator!=(const EntityId &p_other) const { return !(*this == p_other); }
	String to_string() const;
	static Error parse(const String &p_text, EntityId &r_id);
	static Error generate(EntityId &r_id);
};

struct EntityIdHasher {
	static uint32_t hash(const EntityId &p_id) { return hash_murmur3_one_64(p_id.low, hash_murmur3_one_64(p_id.high)); }
};

struct EntityRef {
	EntityId id;
	bool operator==(const EntityRef &p_other) const { return id == p_other.id; }
};

struct EntityHandle {
	uint64_t world_generation = 0;
	uint64_t entity = 0;
	bool is_valid() const { return world_generation != 0 && entity != 0; }
	bool operator==(const EntityHandle &p_other) const { return world_generation == p_other.world_generation && entity == p_other.entity; }
};

enum class EntityReferenceState {
	RESIDENT,
	UNLOADED,
	MISSING,
	DELETED,
};

struct EntityResolution {
	EntityReferenceState state = EntityReferenceState::MISSING;
	EntityHandle handle;
};
