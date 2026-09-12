#pragma once

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
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

	struct PreparedComponent {
		uint64_t schema_id = 0;
		void *buffer = nullptr;
	};

	struct PreparedEntity {
		EntityId id;
		EntityRef parent;
		bool deleted = false;
		bool live = false;
		int64_t order = 0;
		String name;
		String path;
		bool cluster = false;
		Array components;
		LocalVector<PreparedComponent> values;

		PreparedEntity() = default;
		PreparedEntity(const PreparedEntity &) = delete;
		PreparedEntity &operator=(const PreparedEntity &) = delete;
		PreparedEntity(PreparedEntity &&p_other) { *this = std::move(p_other); }
		PreparedEntity &operator=(PreparedEntity &&p_other);
		~PreparedEntity() { release(); }
		Error decode_component(const EntityComponentSchema &p_schema, const Variant &p_value);
		void release();
	};

	enum SectionAction {
		SECTION_SET,
		SECTION_ERASE,
		SECTION_KEEP,
	};

	struct PreparedSet {
		EntityScene *scene = nullptr;
		const LocalVector<PreparedEntity> *entities = nullptr;
		HashMap<EntityId, uint32_t, EntityIdHasher> lookup;

		PreparedSet(EntityScene &p_scene) :
				scene(&p_scene) {}
		explicit PreparedSet(const LocalVector<PreparedEntity> &p_entities);
		const PreparedEntity *find(EntityId p_id) const;
		bool is_deleted(EntityId p_id) const;
		EntityRef get_parent(EntityId p_id) const;
		int64_t get_order(EntityId p_id) const;
		Error collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const;
		SectionAction build_section(EntityId p_id, Section &r_section) const;
		void write_components(EntityWorld &p_target, EntityHandle p_handle, EntityId p_id) const;
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

	struct Stats {
		int jobs_dispatched = 0;
		int jobs_completed = 0;
		int jobs_discarded = 0;
		int cells_committed = 0;
		int entities_committed = 0;
		uint64_t dispatch_usec = 0;
		uint64_t commit_usec = 0;
		uint64_t worker_usec = 0;
	};

private:
	struct PreparedCell {
		CellKey key;
		LocalVector<PreparedEntity> entities;
		Error error = OK;
		EntityId failing;
		String failing_field;
	};

	struct CellJob {
		String directory;
		String relative;
		HashSet<EntityId, EntityIdHasher> skip;
		HashSet<EntityId, EntityIdHasher> globals;
		Vector<String> missing;
		Vector<EntityId> ancestors;
		PreparedCell result;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		SafeFlag cancelled;
		uint64_t worker_usec = 0;
	};

	static constexpr uint32_t MAX_CELL_JOBS = 4;

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
	HashMap<EntityId, Section, EntityIdHasher> deleted_storage;
	HashMap<CellKey, HashSet<EntityId, EntityIdHasher>, CellKeyHasher> cells;
	HashMap<EntityId, CellKey, EntityIdHasher> cell_of;
	HashSet<EntityId, EntityIdHasher> globals;
	HashSet<EntityId, EntityIdHasher> dirty;
	HashMap<CellKey, Vector<EntityId>, CellKeyHasher> resident_cells;
	LocalVector<CellJob *> cell_jobs;
	HashMap<CellKey, uint64_t, CellKeyHasher> failed_cells;
	HashMap<CellKey, bool, CellKeyHasher> probed_cells;
	uint64_t probed_revision = 0;
	HashMap<CellKey, LocalVector<Ref<Resource>>, CellKeyHasher> cell_assets;
	struct PrefabMember {
		String instance;
		String source;
	};
	HashMap<EntityId, PrefabMember, EntityIdHasher> prefab_members;
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
	static int32_t _cell_index(double p_value, double p_size);
	String _storage_directory(EntityId p_id, String &r_source) const;
	Error _assign_cell(EntityId p_id);
	void _forget_cell(EntityId p_id);
	Error _assign_cells(const Vector<EntityId> &p_ids);
	void _forget_entity(EntityId p_id);
	String _cell_path(const CellKey &p_cell) const;
	void _index_prefabs();
	Error _load_resident(const Vector<EntityId> &p_ids, LoadProfile *r_profile);
	Error _read_record(EntityId p_id, Dictionary &r_record, bool *r_stored = nullptr, bool p_prefer_stored = false);
	static Dictionary _shallow_record(const Dictionary &p_record);
	Error _read_stored(EntityId p_id, Dictionary &r_record);
	Error _encode_record(EntityId p_id, Dictionary &r_record);
	Error _collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const;
	Error _prepare(const Vector<EntityId> &p_ids, Ref<EntityScene> &r_scene, bool p_prefer_stored = false);
	Error _prepare_entities(const Vector<EntityId> &p_ids, LocalVector<PreparedEntity> &r_entities, LoadProfile *r_profile);
	static Error _decode_record(const Dictionary &p_record, PreparedEntity &r_prepared, String &r_field, LoadProfile *r_profile);
	Error _decode_entity(EntityId p_id, const Dictionary &p_record, PreparedEntity &r_prepared, LoadProfile *r_profile);
	Error _can_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids) const;
	Error _commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, bool p_resident, bool p_dirty = true);
	Error _install(EntityId p_id, const Dictionary &p_record);
	Error _validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, const String &p_prefix = String());
	static Error _describe_components(const Dictionary &p_record, Section &r_section, Vector<uint64_t> &r_types, String &r_field);
	Error _describe(EntityId p_id, const Dictionary &p_record, Section &r_section);
	bool _is_cell_in_flight(const CellKey &p_cell) const;
	Error _dispatch_cell(const CellKey &p_cell);
	Error _load_cell_assets(CellJob &p_job);
	static Error _prepare_stored(EntityId p_id, const Dictionary &p_record, PreparedEntity &r_prepared, String &r_field);
	static Error _read_cell(CellJob &p_job);
	static void _run_cell_job(void *p_job);
	Error _revalidate_job(CellJob &p_job, Vector<EntityId> &r_ids);
	Error _commit_cell(CellJob &p_job, Stats &r_stats);
	Error _fail(EntityId p_id, const String &p_field, Error p_error);
	void _relocate(const String &p_path);

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
	Error request_cells(const Vector<CellKey> &p_cells, int *r_remaining = nullptr, Stats *r_stats = nullptr);
	Error commit_ready(int p_max_entities, Stats *r_stats = nullptr);
	void flush_streaming();
	Error release_cells(const Vector<CellKey> &p_cells);
	Vector<CellKey> get_resident_cells() const;
	bool is_cell_resident(const CellKey &p_cell) const { return resident_cells.has(p_cell); }
	int get_resident_cell_count() const { return resident_cells.size(); }
	bool cell_exists(const CellKey &p_cell, int *r_probe_budget = nullptr);
	CellKey cell_for_position(const String &p_grid, const Vector3 &p_position) const;
	AABB cell_aabb(const CellKey &p_cell) const;
	Vector<String> get_grid_names() const;
	double get_grid_size(const String &p_grid) const;
	double get_grid_range(const String &p_grid) const;
	String get_entity_name(EntityId p_id) const;
	Error load_subset(const Vector<EntityId> &p_ids);
	Error unload_subset(const Vector<EntityId> &p_ids);
	Error pin(const Vector<EntityId> &p_ids);
	void unpin(const Vector<EntityId> &p_ids);
	Error relocate(const String &p_path);
	Error create_play_document(Ref<EntityScene> &r_scene);
};
