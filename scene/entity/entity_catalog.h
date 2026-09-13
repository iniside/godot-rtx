#pragma once

#include "entity_id.h"
#include "entity_task_scheduler.h"

#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class EntityWorld;

class EntityCatalog {
	friend class EntityWorld;
	friend class EntityScene;
	friend class EntitySceneCommands;
	friend class EntitySceneIO;

public:
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
		String prefab_instance;
		String prefab_source;
		int32_t cell_x = 0;
		int32_t cell_y = 0;
		int32_t cell_z = 0;
		bool has_order = false;
		bool has_section = false;
		bool has_cell = false;
	};

	struct RowLocation {
		uint32_t block = UINT32_MAX;
		uint32_t generation = 0;
		uint32_t row = UINT32_MAX;
		bool is_valid() const { return block != UINT32_MAX && row != UINT32_MAX; }
		bool operator==(const RowLocation &p_other) const { return block == p_other.block && generation == p_other.generation && row == p_other.row; }
	};

	struct RowState {
		EntityHandle handle;
		uint64_t revision = 0;
		mutable uint64_t snapshot_visit = 0;
		uint64_t incarnation = 1;
		uint32_t overlay = UINT32_MAX;
		uint32_t pin_count = 0;
		uint32_t render_mask = 0;
		bool active = true;
		bool resident = false;
		bool document_dirty = false;
		bool world_changed = false;
		bool global = false;
		bool prefab = false;
		bool dependency = false;
		bool tombstone = false;
	};

	struct ArchetypeSpan {
		Vector<uint64_t> signature;
		uint32_t first = 0;
		uint32_t count = 0;
	};
	struct ParentEdge {
		EntityId parent;
		uint32_t child = 0;
	};
	struct ParentEdgeOrder {
		bool operator()(const ParentEdge &p_left, const ParentEdge &p_right) const {
			return p_left.parent.high != p_right.parent.high ? p_left.parent.high < p_right.parent.high : (p_left.parent.low != p_right.parent.low ? p_left.parent.low < p_right.parent.low : p_left.child < p_right.child);
		}
	};

	struct CellRuntimeBlock {
		uint32_t generation = 1;
		String grid;
		int32_t x = 0;
		int32_t y = 0;
		int32_t z = 0;
		bool ordinary_cell = false;
		Vector<EntityId> ids;
		Vector<Record> base;
		Vector<Record> overlays;
		Vector<RowState> states;
		Vector<uint32_t> topological_rows;
		Vector<uint32_t> child_offsets;
		Vector<uint32_t> child_rows;
		Vector<ArchetypeSpan> archetypes;
	};
	struct ReleasePlan {
		Vector<EntityId> unloading;
		Vector<RowLocation> retiring;
		Vector<EntityId> retiring_ids;
	};

private:
	struct LocatorEntry {
		EntityId id;
		RowLocation location;
	};
	struct LocatorRun : RefCounted {
		Vector<LocatorEntry> entries;
	};
	struct ChildEntry {
		EntityId child;
		EntityId parent;
		RowLocation location;
	};
	struct ChildRun : RefCounted {
		Vector<ChildEntry> by_child;
		Vector<ChildEntry> by_parent;
	};
	struct ChildOrder {
		bool operator()(const ChildEntry &p_left, const ChildEntry &p_right) const {
			return p_left.child.high != p_right.child.high ? p_left.child.high < p_right.child.high : p_left.child.low < p_right.child.low;
		}
	};
	struct ParentRunOrder {
		bool operator()(const ChildEntry &p_left, const ChildEntry &p_right) const {
			return p_left.parent.high != p_right.parent.high ? p_left.parent.high < p_right.parent.high : (p_left.parent.low != p_right.parent.low ? p_left.parent.low < p_right.parent.low : ChildOrder()(p_left, p_right));
		}
	};
	struct LocatorOrder {
		bool operator()(const LocatorEntry &p_left, const LocatorEntry &p_right) const { return _id_less(p_left.id, p_right.id); }
	};
	struct IdOrder {
		bool operator()(EntityId p_left, EntityId p_right) const { return _id_less(p_left, p_right); }
	};

	LocalVector<CellRuntimeBlock *> blocks;
	struct CompactionJob : EntityTaskScheduler::Graph {
		LocalVector<Ref<LocatorRun>> locator_snapshot;
		LocalVector<Ref<ChildRun>> child_snapshot;
		Ref<LocatorRun> locator_result;
		Ref<ChildRun> child_result;
		uint64_t worker_usec = 0;
		uint64_t retained_bytes = 0;
		uint32_t enumerate() override { return 0; }
		void read_range(uint32_t) override {}
		uint32_t prepare(bool) override { return 0; }
		void prepare_range(uint32_t) override {}
		void finish_prepare() override;
	};

	static constexpr uint32_t RUN_COMPACTION_THRESHOLD = 4;
	static constexpr uint32_t RUN_HARD_CAP = 8;
	static constexpr uint64_t RUN_BYTE_HARD_CAP = 60 * 1024 * 1024;
	static_assert(RUN_BYTE_HARD_CAP <= (EntityTaskScheduler::Graph::BYTE_BUDGET - sizeof(CompactionJob) - 4096) / 4);
	LocalVector<Ref<LocatorRun>> locator_runs;
	LocalVector<Ref<ChildRun>> child_runs;
	Ref<EntityTaskScheduler::Mailbox> compaction_mailbox;
	CompactionJob *compaction_job = nullptr;
	uint32_t next_block_generation = 1;
	uint32_t active_records = 0;
	uint32_t resident_records = 0;
	uint64_t pending_compaction_bytes = 0;
	uint64_t run_backpressure_count = 0;
	uint64_t max_locator_runs = 0;
	uint64_t max_child_runs = 0;
	uint64_t max_run_bytes = 0;
	Error last_publish_error = OK;
	bool shutting_down = false;

	static bool _id_less(EntityId p_left, EntityId p_right);
	const LocatorEntry *_find_in_run(const LocatorRun &p_run, EntityId p_id) const;
	uint64_t _run_bytes() const;
	Error _ensure_run_capacity(uint32_t p_locator_runs, uint32_t p_child_runs, uint64_t p_bytes);
	Error _prepare_child_publication(uint32_t p_rows);
	void _record_run_high_water();
	void _publish_locator_entries(Vector<LocatorEntry> &&p_entries);
	void _publish_child_entries(Vector<ChildEntry> &&p_entries);
	void _publish_locator_rows(uint32_t p_block, const Vector<uint32_t> *p_rows = nullptr, bool p_invalid = false);
	void _publish_child_rows(uint32_t p_block, const Vector<uint32_t> *p_rows = nullptr, bool p_invalid = false);
	void _rebuild_block_indices(uint32_t p_block);
	void _schedule_compaction(bool p_force = false);
	void _collect_compaction();

public:
	EntityCatalog() = default;
	~EntityCatalog();
	EntityCatalog(const EntityCatalog &) = delete;
	EntityCatalog &operator=(const EntityCatalog &) = delete;

	RowLocation locate(EntityId p_id) const;
	Record *edit_record(EntityId p_id);
	const Record *get_record(EntityId p_id) const;
	RowState *get_state_ptr(EntityId p_id);
	const RowState *get_state_ptr(EntityId p_id) const;
	RowState *get_state_ptr(RowLocation p_location);
	const RowState *get_state_ptr(RowLocation p_location) const;
	Record *edit_record(RowLocation p_location);
	const Record *get_record(RowLocation p_location) const;

	uint32_t add_block(const Vector<EntityId> &p_ids, const Vector<Record> &p_records, const String &p_grid = String(), int32_t p_x = 0, int32_t p_y = 0, int32_t p_z = 0, bool p_ordinary_cell = false);
	Error add_record(EntityId p_id, EntityRef p_parent = {});
	Error insert_record(EntityId p_id, const Record &p_record);
	bool erase_record(EntityId p_id);
	Error build_release_plan(const Vector<EntityId> &p_members, ReleasePlan &r_plan);
	Error retire_rows(const Vector<RowLocation> &p_rows, EntityWorld *p_world = nullptr);
	bool has_record(EntityId p_id) const { return locate(p_id).is_valid(); }
	void clear();
	void maintenance();
	void flush_maintenance();

	Vector<EntityId> get_ids() const;
	Vector<EntityId> get_children(EntityId p_id) const;
	Error _refresh_parent(EntityId p_id);
	Error _refresh_parents(const Vector<EntityId> &p_ids);
	Error _set_parent(EntityId p_id, EntityRef p_parent);
	EntityReferenceState get_state(EntityId p_id) const;
	EntityRef get_parent(EntityId p_id) const;
	int get_record_count() const;
	int get_resident_count() const;
	CellRuntimeBlock *get_block(uint32_t p_block);
	const CellRuntimeBlock *get_block(uint32_t p_block) const;
	const LocalVector<CellRuntimeBlock *> &get_blocks() const { return blocks; }
	uint32_t get_locator_run_count() const { return locator_runs.size(); }
	uint32_t get_child_run_count() const { return child_runs.size(); }
	Error get_last_publish_error() const { return last_publish_error; }
};
