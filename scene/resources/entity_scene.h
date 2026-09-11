#pragma once

#include "core/io/resource.h"
#include "core/io/file_access.h"
#include "scene/entity/entity_world.h"

class EntitySceneCommands;
class EntitySceneIO;

class EntityScene : public Resource {
	GDCLASS(EntityScene, Resource);
	friend class EntitySceneIO;
	friend class EntitySceneCommands;
	friend class ResourceFormatLoaderEntityScene;

	struct Section {
		uint64_t offset = 0;
		uint64_t length = 0;
		PackedByteArray bytes;
		Array dependencies;
		Array components;
	};
	EntityId document_id;
	EntityCatalog catalog;
	EntityWorld *world = nullptr;
	EntitySceneCommands *commands = nullptr;
	Ref<FileAccess> source;
	String storage_path;
	HashMap<EntityId, Section, EntityIdHasher> sections;
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
	};

	Error _owner();
	Error _read_record(EntityId p_id, Dictionary &r_record, bool *r_stored = nullptr, bool p_prefer_stored = false);
	Error _read_bytes(EntityId p_id, PackedByteArray &r_bytes);
	Error _encode_record(EntityId p_id, Dictionary &r_record);
	Error _collect_required(const Vector<EntityId> &p_ids, Vector<EntityId> &r_ids) const;
	Error _prepare(const Vector<EntityId> &p_ids, Ref<EntityScene> &r_scene, bool p_prefer_stored = false, LoadProfile *r_profile = nullptr);
	Error _can_commit(const EntityScene &p_prepared, const Vector<EntityId> &p_ids) const;
	void _commit(EntityScene &p_prepared, const Vector<EntityId> &p_ids, bool p_resident);
	Error _install(EntityId p_id, const Dictionary &p_record, LoadProfile *r_profile = nullptr);
	Error _validate_fields(EntityId p_id, uint64_t p_type, const Dictionary &p_fields, Array *r_dependencies = nullptr, const String &p_prefix = String());
	Error _describe(EntityId p_id, const Dictionary &p_record, Section &r_section);
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
	Error load_subset(const Vector<EntityId> &p_ids);
	Error unload_subset(const Vector<EntityId> &p_ids);
	Error pin(const Vector<EntityId> &p_ids);
	void unpin(const Vector<EntityId> &p_ids);
	Error create_play_document(Ref<EntityScene> &r_scene);
};
