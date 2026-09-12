#pragma once

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "scene/entity/entity_world.h"

class EntitySceneCommands;
class EntitySceneIO;

class EntityScene : public Resource {
	GDCLASS(EntityScene, Resource);
	friend class EntitySceneIO;
	friend class EntitySceneCommands;
	friend class ResourceFormatLoaderEntityScene;

	struct Section {
		String path;
		bool cluster = false;
		String name;
		Dictionary record;
		Array components;
	};

	struct Grid {
		double size = 0.0;
		double range = 0.0;
	};

public:
	struct CellKey {
		String grid;
		int32_t x = 0;
		int32_t y = 0;
		int32_t z = 0;
		bool operator==(const CellKey &p_other) const { return x == p_other.x && y == p_other.y && z == p_other.z && grid == p_other.grid; }
		bool operator!=(const CellKey &p_other) const { return !(*this == p_other); }
	};

	struct CellKeyHasher {
		static uint32_t hash(const CellKey &p_key) {
			uint32_t value = hash_murmur3_one_32(uint32_t(p_key.x), p_key.grid.hash());
			value = hash_murmur3_one_32(uint32_t(p_key.y), value);
			return hash_fmix32(hash_murmur3_one_32(uint32_t(p_key.z), value));
		}
	};

private:
	EntityId document_id;
	EntityCatalog catalog;
	EntityWorld *world = nullptr;
	EntitySceneCommands *commands = nullptr;
	String storage_path;
	HashMap<String, Grid> grids;
	String default_grid;
	double default_range = 0.0;
	String cluster_path;
	Dictionary cluster_records;
	HashMap<EntityId, Section, EntityIdHasher> sections;
	HashMap<CellKey, HashSet<EntityId, EntityIdHasher>, CellKeyHasher> cells;
	HashMap<EntityId, CellKey, EntityIdHasher> cell_of;
	HashSet<EntityId, EntityIdHasher> globals;
	HashSet<EntityId, EntityIdHasher> dirty;
	HashMap<CellKey, Vector<EntityId>, CellKeyHasher> resident_cells;
	struct PrefabMember {
		String instance;
		String source;
	};
	HashMap<EntityId, PrefabMember, EntityIdHasher> prefab_members;
	Ref<EntityScene> scratch;
	uint64_t residency_serial = 0;
	bool global_pinned = false;
	HashMap<EntityId, uint32_t, EntityIdHasher> pins;
	HashMap<EntityId, int64_t, EntityIdHasher> order;
	Dictionary prefab_instances;
	uint64_t revision = 0;
	String last_error;
	Thread::ID owner_thread = 0;

	struct LoadProfile {
		uint64_t read_record = 0;
		uint64_t install = 0;
		uint64_t describe = 0;
		uint64_t world = 0;
		uint64_t prepare = 0;
		uint64_t check = 0;
		uint64_t commit = 0;
	};

	Error _owner();
	static bool _parse_cell_directory(const String &p_directory, CellKey &r_key);
	String _storage_directory(EntityId p_id) const;
	void _assign_cell(EntityId p_id);
	void _forget_cell(EntityId p_id);
	void _rebuild_cells();
	void _index_prefabs();
	void _clear_scratch();
	Error _load_resident(const Vector<EntityId> &p_ids, LoadProfile *r_profile);
	Error _cell_entities(const CellKey &p_cell, Vector<EntityId> &r_ids, Vector<EntityId> &r_ancestors) const;
	Error _read_record(EntityId p_id, Dictionary &r_record, bool *r_stored = nullptr, bool p_prefer_stored = false);
	Error _read_stored(EntityId p_id, Dictionary &r_record);
	Error _encode_record(EntityId p_id, Dictionary &r_record);
	Error _collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const;
	Error _prepare(const Vector<EntityId> &p_ids, Ref<EntityScene> &r_scene, bool p_prefer_stored = false, LoadProfile *r_profile = nullptr, bool p_scratch = false);
	Error _can_commit(const EntityScene &p_prepared, const Vector<EntityId> &p_ids) const;
	void _commit(EntityScene &p_prepared, const Vector<EntityId> &p_ids, bool p_resident, bool p_dirty = true);
	Error _install(EntityId p_id, const Dictionary &p_record, LoadProfile *r_profile = nullptr);
	Error _validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, bool p_decode_assets, const String &p_prefix = String());
	Error _describe(EntityId p_id, const Dictionary &p_record, Section &r_section, bool p_decode_assets = true);
	Error _fail(EntityId p_id, const String &p_field, Error p_error);

protected:
	static void _bind_methods();

public:
	EntityScene();
	~EntityScene() override;
	EntityId get_document_id() const { return document_id; }
	String get_document_id_text() const { return document_id.to_string(); }
	uint64_t get_revision() const { return revision; }
	const EntityCatalog &get_catalog() const { return catalog; }
	EntityWorld *get_world();
	EntitySceneCommands &get_commands();
	EntityResolution resolve(EntityId p_id) const;
	int get_record_count() const { return catalog.get_record_count(); }
	int get_resident_count() const { return world ? world->get_resident_count() : 0; }
	String get_last_error() const { return last_error; }
	int64_t get_order(EntityId p_id) const;
	uint64_t get_residency_serial() const { return residency_serial; }
	Error load_global();
	bool is_global_loaded() const { return global_pinned; }
	Error request_cells(const Vector<CellKey> &p_cells, int p_max_entities, int *r_remaining = nullptr);
	Error release_cells(const Vector<CellKey> &p_cells);
	Vector<CellKey> get_cells() const;
	Vector<CellKey> get_resident_cells() const;
	bool is_cell_resident(const CellKey &p_cell) const { return resident_cells.has(p_cell); }
	int get_cell_count() const { return cells.size(); }
	CellKey cell_for_position(const String &p_grid, const Vector3 &p_position) const;
	AABB cell_aabb(const CellKey &p_cell) const;
	Error load_subset(const Vector<EntityId> &p_ids);
	Error unload_subset(const Vector<EntityId> &p_ids);
	Error pin(const Vector<EntityId> &p_ids);
	void unpin(const Vector<EntityId> &p_ids);
	Error create_play_document(Ref<EntityScene> &r_scene);
};
