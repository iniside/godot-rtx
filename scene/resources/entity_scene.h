#pragma once

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "scene/entity/entity_task_scheduler.h"
#include "scene/entity/entity_world.h"

class EntitySceneCommands;
class EntitySceneIO;

class EntityScene : public Resource {
	GDCLASS(EntityScene, Resource);
	friend class EntitySceneIO;
	friend class EntitySceneCommands;
	friend class ResourceFormatLoaderEntityScene;

	using Section = EntityCatalog::Section;

	struct Grid {
		double size = 0.0;
		double range = 0.0;
	};

	struct PreparedColumn {
		const EntityComponentSchema *schema = nullptr;
		void *buffer = nullptr;
		void *row(uint32_t p_row) const { return static_cast<uint8_t *>(buffer) + schema->size * p_row; }
	};

	struct PreparedGroup {
		Vector<uint64_t> signature;
		LocalVector<PreparedColumn> columns;
		LocalVector<uint32_t> constructed;
		uint32_t capacity = 0;
		uint64_t allocated_bytes = 0;
		uint64_t constructions = 0;
		uint32_t moved_rows = 0;
		bool profile = false;

		PreparedGroup() = default;
		PreparedGroup(const PreparedGroup &) = delete;
		PreparedGroup &operator=(const PreparedGroup &) = delete;
		~PreparedGroup();
		Error allocate();
		Error decode_row(uint32_t p_row, const Dictionary &p_components, String &r_field);
		void release_row(uint32_t p_row);
		void move_row(uint32_t p_from, uint32_t p_to, const LocalVector<const ecs_type_info_t *> &p_types);
	};

	struct PreparedSignatureHasher {
		static uint32_t hash(const Vector<uint64_t> &p_signature) {
			uint32_t hash = HASH_MURMUR3_SEED;
			for (uint64_t id : p_signature) {
				hash = hash_murmur3_one_64(id, hash);
			}
			return hash_fmix32(hash);
		}
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
		PreparedGroup *group = nullptr;
		uint32_t group_index = UINT32_MAX;
		uint32_t row = 0;
		bool owns_group = false;

		PreparedEntity() = default;
		PreparedEntity(const PreparedEntity &) = delete;
		PreparedEntity &operator=(const PreparedEntity &) = delete;
		PreparedEntity(PreparedEntity &&p_other) { *this = std::move(p_other); }
		PreparedEntity &operator=(PreparedEntity &&p_other);
		~PreparedEntity() { release(); }
		void release();
	};

	enum SectionAction {
		SECTION_SET,
		SECTION_ERASE,
		SECTION_KEEP,
	};
	struct CommitProfile {
		uint64_t required_catalog_usec = 0;
		uint64_t ecs_parent_remove_usec = 0;
		uint64_t ecs_entity_destroy_usec = 0;
		uint64_t ecs_entity_create_identity_usec = 0;
		uint64_t ecs_materialize_parent_set_usec = 0;
		uint64_t resident_remove_usec = 0;
		uint64_t resident_insert_usec = 0;
		uint64_t initial_dirty_usec = 0;
		uint64_t prepared_schema_lookup_usec = 0;
		uint64_t component_mutation_usec = 0;
		uint64_t component_changed_usec = 0;
		uint64_t change_bookkeeping_usec = 0;
		uint64_t sections_order_usec = 0;
		uint64_t catalog_parent_usec = 0;
		uint64_t ecs_final_parent_set_usec = 0;
		uint64_t assign_cell_usec = 0;
		uint64_t metadata_commit_usec = 0;
		uint64_t ecs_bulk_create_components_usec = 0;
		uint64_t bulk_compact_usec = 0;
		uint64_t transform_finalize_usec = 0;
		uint64_t initial_packet_prepare_usec = 0;
		int bulk_groups = 0;
		int bulk_rows = 0;
		int compacted_rows = 0;
		int skipped_rows = 0;
		int entities_materialized = 0;
		int entities_destroyed = 0;
		int parent_removals = 0;
		int materialize_parent_sets = 0;
		int component_mutations = 0;
		int final_parent_sets = 0;
		int metadata_new_records = 0;
		int metadata_record_updates = 0;
		int metadata_order_updates = 0;
		int metadata_section_updates = 0;
		int metadata_membership_removals = 0;
		int metadata_full_cell_clears = 0;
		int cell_membership_insertions = 0;
		int initial_packet_updates = 0;
		int initial_packet_table_rows = 0;
		int initial_packet_fallback_rows = 0;
	};
	struct CommitItem {
		EntityId id;
		EntityRef parent;
		EntityRef previous_parent;
		EntityResolution existing;
		const PreparedEntity *prepared = nullptr;
		Section section;
		SectionAction section_action = SECTION_KEEP;
		String cell_grid;
		int32_t cell_x = 0;
		int32_t cell_y = 0;
		int32_t cell_z = 0;
		int64_t order = 0;
		bool deleted = false;
		bool previous_deleted = false;
		bool had_record = false;
		bool had_cell = false;
		bool had_global = false;
		bool parent_changed = false;
		bool parent_index_changed = false;
		bool had_order = false;
		bool order_changed = false;
		bool had_section = false;
		bool section_changed = false;
		bool install = false;
	};

	struct PreparedSet {
		struct RowOrder {
			const LocalVector<PreparedEntity> *entities = nullptr;
			bool operator()(uint32_t p_left, uint32_t p_right) const {
				const EntityId left = (*entities)[p_left].id;
				const EntityId right = (*entities)[p_right].id;
				return left.high != right.high ? left.high < right.high : left.low < right.low;
			}
		};
		EntityScene *scene = nullptr;
		const LocalVector<PreparedEntity> *entities = nullptr;
		const LocalVector<PreparedGroup *> *bulk_groups = nullptr;
		Vector<uint32_t> sorted_rows;

		PreparedSet(EntityScene &p_scene) :
				scene(&p_scene) {}
		explicit PreparedSet(const LocalVector<PreparedEntity> &p_entities, const LocalVector<PreparedGroup *> *p_bulk_groups = nullptr);
		int32_t find_row(EntityId p_id) const;
		const PreparedEntity *find(EntityId p_id) const;
		bool is_deleted(EntityId p_id) const;
		EntityRef get_parent(EntityId p_id) const;
		int64_t get_order(EntityId p_id) const;
		Error collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const;
		SectionAction build_section(EntityId p_id, Section &r_section) const;
		void build_commit_item(EntityId p_id, CommitItem &r_item) const;
		void write_components(EntityWorld &p_target, EntityHandle p_handle, EntityId p_id, const PreparedEntity *p_entry, CommitProfile *r_profile = nullptr) const;
		Error materialize_groups(EntityWorld &p_target, LocalVector<EntityInitialRenderGroup> &r_initial, CommitProfile *r_profile) const;
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
		int jobs_resumed = 0;
		int cells_committed = 0;
		int entities_committed = 0;
		uint64_t dispatch_usec = 0;
		uint64_t owner_total_usec = 0;
		uint64_t owner_asset_load_usec = 0;
		uint64_t owner_revalidate_usec = 0;
		uint64_t owner_commit_validate_usec = 0;
		uint64_t owner_install_usec = 0;
		uint64_t owner_residency_usec = 0;
		uint64_t owner_job_scan_cleanup_usec = 0;
		uint64_t worker_total_usec = 0;
		uint64_t enumerate_usec = 0;
		uint64_t read_parse_usec = 0;
		uint64_t read_parse_wall_usec = 0;
		int files_read = 0;
		int read_ranges = 0;
		int read_lanes = 0;
		uint64_t parse_usec = 0;
		uint64_t decode_usec = 0;
		uint64_t prepared_groups = 0;
		uint64_t prepared_columns = 0;
		uint64_t prepared_column_allocations = 0;
		uint64_t prepared_constructed_rows = 0;
		uint64_t peak_cell_prepared_column_bytes = 0;
		uint64_t worker_remainder_usec = 0;
		uint64_t install_required_catalog_usec = 0;
		uint64_t install_ecs_parent_remove_usec = 0;
		uint64_t install_ecs_entity_destroy_usec = 0;
		uint64_t install_ecs_entity_create_identity_usec = 0;
		uint64_t install_ecs_materialize_parent_set_usec = 0;
		uint64_t install_resident_remove_usec = 0;
		uint64_t install_resident_insert_usec = 0;
		uint64_t install_initial_dirty_usec = 0;
		uint64_t install_prepared_schema_lookup_usec = 0;
		uint64_t install_component_mutation_usec = 0;
		uint64_t install_component_changed_usec = 0;
		uint64_t install_change_bookkeeping_usec = 0;
		uint64_t install_sections_order_usec = 0;
		uint64_t install_catalog_parent_usec = 0;
		uint64_t install_ecs_final_parent_set_usec = 0;
		uint64_t install_assign_cell_usec = 0;
		uint64_t install_metadata_commit_usec = 0;
		uint64_t install_remainder_usec = 0;
		uint64_t install_ecs_bulk_create_components_usec = 0;
		uint64_t install_bulk_compact_usec = 0;
		uint64_t install_transform_finalize_usec = 0;
		uint64_t install_initial_packet_prepare_usec = 0;
		int bulk_groups = 0;
		int bulk_rows = 0;
		int compacted_rows = 0;
		int skipped_rows = 0;
		int entities_materialized = 0;
		int entities_destroyed = 0;
		int parent_removals = 0;
		int materialize_parent_sets = 0;
		int component_mutations = 0;
		int final_parent_sets = 0;
		int initial_packet_updates = 0;
		int initial_packet_table_rows = 0;
		int initial_packet_fallback_rows = 0;
	};

private:
	struct PreparedCell {
		CellKey key;
		LocalVector<PreparedGroup *> groups;
		LocalVector<PreparedEntity> entities;
		bool grouped = false;
		Error error = OK;
		EntityId failing;
		String failing_field;
		~PreparedCell();
	};

	struct PendingRecord {
		EntityId id;
		String path;
		bool cluster = false;
		Dictionary record;
		uint32_t prepared_index = 0;
		bool operator<(const PendingRecord &p_other) const {
			return path != p_other.path ? path < p_other.path : (id.high != p_other.id.high ? id.high < p_other.id.high : id.low < p_other.id.low);
		}
	};

	struct CellReadRange {
		LocalVector<PendingRecord> records;
		Error error = OK;
		String failing_field;
		uint64_t worker_total_usec = 0;
		uint64_t read_parse_usec = 0;
		uint64_t began_usec = 0;
		uint64_t ended_usec = 0;
		uint32_t files_read = 0;
	};

	struct CellJob : EntityTaskScheduler::Graph {
		String directory;
		String storage_directory;
		String relative;
		PackedStringArray filenames;
		HashMap<String, bool> required_files;
		Vector<EntityId> required;
		LocalVector<CellReadRange> read_ranges;
		Vector<EntityId> skip;
		Vector<EntityId> globals;
		Vector<EntityId> members;
		Vector<String> missing;
		HashSet<String> loaded;
		LocalVector<Ref<Resource>> assets;
		Vector<EntityId> ancestors;
		LocalVector<PendingRecord> pending;
		bool resume = false;
		PreparedCell result;
		uint64_t worker_total_usec = 0;
		uint64_t enumerate_usec = 0;
		uint64_t read_parse_usec = 0;
		uint64_t read_parse_wall_usec = 0;
		uint32_t files_read = 0;
		uint32_t read_range_count = 0;
		uint32_t read_lanes = 0;
		uint64_t parse_usec = 0;
		uint64_t decode_usec = 0;
		uint32_t enumerate() override;
		uint64_t prepared_bytes = 0;
		void read_range(uint32_t p_index) override;
		void prepare(bool p_decode_only) override;
	};

	struct OwnerProfile {
		uint64_t asset_load_usec = 0;
		uint64_t revalidate_usec = 0;
		uint64_t commit_validate_usec = 0;
		uint64_t install_usec = 0;
		uint64_t residency_usec = 0;
		CommitProfile install;
	};

	static uint32_t _max_cell_jobs();
	struct SnapshotIdOrder {
		bool operator()(EntityId p_left, EntityId p_right) const {
			return p_left.high != p_right.high ? p_left.high < p_right.high : p_left.low < p_right.low;
		}
	};
	static bool _snapshot_has(const Vector<EntityId> &p_ids, EntityId p_id);
	static void _sort_snapshot_ids(Vector<EntityId> &r_ids);
	static constexpr uint32_t CELL_READ_RANGE_FILES = 64;

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
	using CellMembers = Vector<EntityId>;
	HashMap<EntityId, Section, EntityIdHasher> deleted_storage;
	HashMap<CellKey, CellMembers, CellKeyHasher> cells;
	String assigned_directory;
	CellKey assigned_key;
	HashMap<CellKey, Vector<EntityId>, CellKeyHasher> resident_cells;
	LocalVector<CellJob *> cell_jobs;
	Ref<EntityTaskScheduler::Mailbox> cell_mailbox;
	HashMap<CellKey, uint64_t, CellKeyHasher> failed_cells;
	HashMap<CellKey, bool, CellKeyHasher> probed_cells;
	uint64_t probed_revision = 0;
	HashMap<CellKey, LocalVector<Ref<Resource>>, CellKeyHasher> cell_assets;
	struct PrefabMember {
		String instance;
		String source;
	};
	struct PrefabMapping {
		EntityId id;
		PrefabMember member;
	};
	Vector<PrefabMapping> prefab_mappings;
	uint64_t residency_serial = 0;
	bool global_pinned = false;
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
	const Section *_get_section(EntityId p_id) const;
	Section *_edit_section(EntityId p_id);
	void _erase_section(EntityId p_id);
	const int64_t *_get_order_ptr(EntityId p_id) const;
	void _set_order(EntityId p_id, int64_t p_order);
	void _erase_order(EntityId p_id);
	bool _is_dirty(EntityId p_id) const;
	void _set_dirty(EntityId p_id, bool p_dirty = true);
	bool _is_global(EntityId p_id) const;
	void _set_global(EntityId p_id, bool p_global);
	bool _is_pinned(EntityId p_id) const;
	void _pin_row(EntityId p_id);
	void _unpin_row(EntityId p_id);
	static bool _cell_members_has(const CellMembers &p_members, EntityId p_id);
	static void _cell_members_insert(CellMembers &r_members, EntityId p_id);
	static void _cell_members_erase(CellMembers &r_members, EntityId p_id);
	static bool _parse_cell_directory(const String &p_directory, CellKey &r_key);
	static int32_t _cell_index(double p_value, double p_size);
	String _storage_directory(EntityId p_id, String &r_source) const;
	Error _assign_cell(EntityId p_id);
	Error _assign_cells(const Vector<EntityId> &p_ids);
	String _cell_path(const CellKey &p_cell) const;
	const PrefabMember *_find_prefab_member(EntityId p_id) const;
	void _index_prefabs();
	Error _load_resident(const Vector<EntityId> &p_ids, LoadProfile *r_profile);
	Error _unload_subset(const Vector<EntityId> &p_ids, bool p_prevalidated_order);
	Error _read_record(EntityId p_id, Dictionary &r_record, bool *r_stored = nullptr, bool p_prefer_stored = false);
	static Dictionary _shallow_record(const Dictionary &p_record);
	Error _read_stored(EntityId p_id, Dictionary &r_record);
	Error _encode_record(EntityId p_id, Dictionary &r_record);
	Error _collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const;
	Error _prepare(const Vector<EntityId> &p_ids, Ref<EntityScene> &r_scene, bool p_prefer_stored = false);
	Error _prepare_entities(const Vector<EntityId> &p_ids, LocalVector<PreparedEntity> &r_entities, LoadProfile *r_profile);
	static Error _decode_record(const Dictionary &p_record, PreparedEntity &r_prepared, String &r_field, LoadProfile *r_profile);
	Error _decode_entity(EntityId p_id, const Dictionary &p_record, PreparedEntity &r_prepared, LoadProfile *r_profile);
	void _build_commit_items(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, LocalVector<CommitItem> &r_items);
	Error _can_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids);
	Error _can_commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, LocalVector<CommitItem> &r_items);
	Error _commit(const PreparedSet &p_prepared, const Vector<EntityId> &p_ids, bool p_resident, bool p_dirty = true, CommitProfile *r_profile = nullptr);
	Error _commit(const PreparedSet &p_prepared, LocalVector<CommitItem> &p_items, bool p_resident, bool p_dirty = true, CommitProfile *r_profile = nullptr, const CellKey *p_streamed_cell = nullptr, const Vector<EntityId> *p_streamed_members = nullptr);
	Error _install(EntityId p_id, const Dictionary &p_record);
	Error _validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, const String &p_prefix = String());
	static Error _describe_components(const Dictionary &p_record, Section &r_section, Vector<uint64_t> &r_types, String &r_field);
	Error _describe(EntityId p_id, const Dictionary &p_record, Section &r_section);
	bool _is_cell_in_flight(const CellKey &p_cell) const;
	Error _dispatch_cell(const CellKey &p_cell);
	Error _load_cell_assets(CellJob &p_job);
	static Error _prepare_stored(EntityId p_id, const Dictionary &p_record, PreparedEntity &r_prepared, Vector<uint64_t> &r_types, String &r_field);
	static void _enumerate_cell(void *p_job);
	static Error _read_cell_range(const CellJob &p_job, uint32_t p_index, CellReadRange &r_range);
	static void _run_cell_read_range(void *p_job, uint32_t p_index);
	static Error _merge_cell_reads(CellJob &p_job);
	void _collect_cell_jobs();
	static Error _decode_cell(CellJob &p_job);
	static void _run_cell_decode(void *p_job);
	Error _revalidate_job(CellJob &p_job, Vector<EntityId> &r_ids);
	Error _commit_cell(CellJob &p_job, Stats &r_stats, OwnerProfile *r_profile = nullptr);
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
