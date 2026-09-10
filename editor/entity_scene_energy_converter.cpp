#include "entity_scene_energy_converter.h"

#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "scene/entity/entity_scene_commands.h"
#include "scene/entity/entity_scene_io.h"
#include "scene/resources/packed_scene.h"

namespace {

Variant renderer_property(const Ref<SceneState> &p_state, const String &p_node_path, const StringName &p_property, const Variant &p_default = Variant()) {
	for (int i = 0; i < p_state->get_node_count(); i++) {
		if (String(p_state->get_node_path(i)).trim_prefix("./") != p_node_path) {
			continue;
		}
		for (int j = 0; j < p_state->get_node_property_count(i); j++) {
			if (p_state->get_node_property_name(i, j) == p_property) {
				return p_state->get_node_property_value(i, j);
			}
		}
	}
	return p_default;
}

template <typename T>
Error save_renderer_asset(const Ref<T> &p_source, const String &p_path, Ref<T> &r_saved, bool p_bundle = false) {
	ERR_FAIL_COND_V(p_source.is_null(), ERR_FILE_MISSING_DEPENDENCIES);
	Ref<Resource> copy = p_source->duplicate_deep(p_bundle ? RESOURCE_DEEP_DUPLICATE_ALL : RESOURCE_DEEP_DUPLICATE_INTERNAL);
	ERR_FAIL_COND_V(copy.is_null(), ERR_CANT_CREATE);
	Error error = ResourceSaver::save(copy, p_path, p_bundle ? ResourceSaver::FLAG_BUNDLE_RESOURCES : ResourceSaver::FLAG_NONE);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot save renderer asset " + p_path);
	ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_path);
	ERR_FAIL_COND_V(uid == ResourceUID::INVALID_ID, ERR_INVALID_DATA);
	ResourceUID *uids = ResourceUID::get_singleton();
	if (uids->has_id(uid)) {
		uids->set_id(uid, p_path);
	} else {
		uids->add_id(uid, p_path);
	}
	r_saved = ResourceLoader::load(p_path, "", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	ERR_FAIL_COND_V(error != OK || r_saved.is_null(), ERR_FILE_CORRUPT);
	Variant encoded;
	error = EntityCodec<Ref<T>>::encode(r_saved, encoded);
	ERR_FAIL_COND_V(error != OK || encoded != Variant(uids->id_to_text(uid)), ERR_INVALID_DATA);
	print_line("Renderer asset: " + p_path + " " + String(encoded));
	return OK;
}

template <typename T>
Error add_energy_component(EntityId p_document, EntityId p_entity, const T &p_component, Vector<EntitySceneCommands::Command> &r_commands) {
	EntitySceneCommands::Command command;
	command.document = p_document;
	command.entity = p_entity;
	command.kind = EntitySceneCommands::ADD_COMPONENT;
	command.component = EntityComponentTraits<T>::id;
	Error error = EntityCodec<T>::encode(p_component, command.after);
	ERR_FAIL_COND_V(error != OK, error);
	r_commands.push_back(command);
	return OK;
}

}

Error convert_energy_directional_entity_scene() {
	Ref<PackedScene> source = ResourceLoader::load("res://energy_directional.tscn");
	Ref<PackedScene> imported = ResourceLoader::load("res://geometry.gltf");
	ERR_FAIL_COND_V(source.is_null() || imported.is_null(), ERR_FILE_MISSING_DEPENDENCIES);
	Ref<FileAccess> input = FileAccess::open("res://entity_migration/resolved/energy_directional.tscn.jsonl", FileAccess::READ);
	ERR_FAIL_COND_V(input.is_null(), ERR_FILE_CANT_OPEN);
	Ref<DirAccess> directory = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	Error error = directory->make_dir_recursive("res://native_assets/energy_directional");
	ERR_FAIL_COND_V(error != OK, error);
	Ref<Material> material_source = renderer_property(source->get_state(), "Geometry/EnergyReceiver/Plane", "material_override");
	Ref<Environment> environment_source = renderer_property(source->get_state(), "WorldEnvironment", "environment");
	Ref<Material> material;
	Ref<Environment> environment;
	error = save_renderer_asset(material_source, "res://native_assets/energy_directional/receiver.tres", material, true);
	ERR_FAIL_COND_V(error != OK, error);
	error = save_renderer_asset(environment_source, "res://native_assets/energy_directional/environment.tres", environment, true);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(environment->get_background() != Environment::BG_COLOR || environment->get_sky().is_valid(), ERR_INVALID_DATA);
	Ref<EntityScene> scene;
	scene.instantiate();
	Vector<EntitySceneCommands::Command> commands;
	Vector<EntityId> ids;
	const char *expected_paths[] = { "WorldEnvironment", "Geometry/EnergyReceiver/Cube", "Geometry/EnergyReceiver/Plane", "Geometry/EnergyReceiver/Sphere", "Geometry/EnergyReceiver/Deformer", "Lights/Directional", "Cameras/Camera", "Cameras/AlternateCamera" };
	for (int i = 0; i < 8; i++) {
		Variant parsed = JSON::parse_string(input->get_line());
		ERR_FAIL_COND_V(parsed.get_type() != Variant::DICTIONARY, ERR_PARSE_ERROR);
		Dictionary row = parsed;
		ERR_FAIL_COND_V(row.get("path", String()) != Variant(expected_paths[i]), ERR_INVALID_DATA);
		Array pose = row.get("world_transform", Array());
		ERR_FAIL_COND_V(pose.size() != 12, ERR_INVALID_DATA);
		EntityId id;
		error = EntityId::generate(id);
		ERR_FAIL_COND_V(error != OK, error);
		ids.push_back(id);
		EntitySceneCommands::Command create;
		create.document = scene->get_document_id();
		create.entity = id;
		create.kind = EntitySceneCommands::CREATE;
		Dictionary record;
		record["components"] = Dictionary();
		create.after = record;
		commands.push_back(create);
		EntityName name;
		name.name = expected_paths[i];
		error = add_energy_component(create.document, id, name, commands);
		ERR_FAIL_COND_V(error != OK, error);
		EntityTransform transform;
		transform.local.translation = { double(pose[9]), double(pose[10]), double(pose[11]) };
		transform.local.basis = Basis(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], pose[7], pose[8]);
		error = add_energy_component(create.document, id, transform, commands);
		ERR_FAIL_COND_V(error != OK, error);
		if (i == 0) {
			EntityEnvironment component;
			component.environment = environment;
			error = add_energy_component(create.document, id, component, commands);
		} else if (i <= 4) {
			String mesh_name = String(expected_paths[i]).get_file();
			Ref<Mesh> mesh_source = renderer_property(imported->get_state(), mesh_name, "mesh");
			EntityMesh component;
			error = save_renderer_asset(mesh_source, "res://native_assets/energy_directional/" + mesh_name.to_snake_case() + ".res", component.mesh, true);
			ERR_FAIL_COND_V(error != OK, error);
			ERR_FAIL_COND_V(component.mesh->get_surface_count() != mesh_source->get_surface_count(), ERR_INVALID_DATA);
			component.visible = i == 2;
			ERR_FAIL_COND_V(bool(row.get("effective_visible", false)) != component.visible, ERR_INVALID_DATA);
			if (component.visible) {
				component.material_override = material;
			}
			error = add_energy_component(create.document, id, component, commands);
			ERR_FAIL_COND_V(error != OK, error);
			EntityGeometry geometry;
			geometry.use_baked_light = true;
			int morphs = component.mesh->get_blend_shape_count();
			ERR_FAIL_COND_V(morphs != mesh_source->get_blend_shape_count() || morphs != (i == 4 ? 1 : 0), ERR_INVALID_DATA);
			if (morphs) {
				ERR_FAIL_COND_V(component.mesh->get_blend_shape_name(0) != StringName("Stretch"), ERR_INVALID_DATA);
				geometry.blend_shape_weights.push_back(0.0);
			}
			error = add_energy_component(create.document, id, geometry, commands);
		} else if (i == 5) {
			EntityLight component;
			component.shadow = true;
			error = add_energy_component(create.document, id, component, commands);
		} else {
			EntityCamera component;
			component.current = i == 6;
			component.far_distance = i == 6 ? 100.0 : 4000.0;
			error = add_energy_component(create.document, id, component, commands);
		}
		ERR_FAIL_COND_V(error != OK, error);
	}
	while (!input->eof_reached()) {
		ERR_FAIL_COND_V(!input->get_line().strip_edges().is_empty(), ERR_INVALID_DATA);
	}
	error = scene->get_commands().execute("Convert preserved directional energy scene", commands);
	ERR_FAIL_COND_V_MSG(error != OK, error, scene->get_last_error());
	error = EntitySceneIO::save(**scene, "res://energy_directional.escn");
	ERR_FAIL_COND_V(error != OK, error);
	Ref<EntityScene> saved;
	error = EntitySceneIO::load("res://energy_directional.escn", saved);
	ERR_FAIL_COND_V(error != OK || saved.is_null(), ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(saved->get_record_count() != 8, ERR_INVALID_DATA);
	error = saved->load_subset(ids);
	ERR_FAIL_COND_V(error != OK || saved->get_resident_count() != 8, ERR_INVALID_DATA);
	for (const EntitySceneCommands::Command &command : commands) {
		if (command.kind != EntitySceneCommands::ADD_COMPONENT) {
			continue;
		}
		Variant value;
		error = saved->get_world()->read_component(saved->resolve(command.entity).handle, command.component, value);
		ERR_FAIL_COND_V(error != OK || value != command.after, ERR_INVALID_DATA);
	}
	for (int i = 0; i < ids.size(); i++) {
		EntityHandle handle = saved->resolve(ids[i]).handle;
		const EntityMesh *mesh = saved->get_world()->get<EntityMesh>(handle);
		const EntityCamera *camera = saved->get_world()->get<EntityCamera>(handle);
		const EntityLight *light = saved->get_world()->get<EntityLight>(handle);
		if (camera || light) {
			const EntityTransform *transform = saved->get_world()->get<EntityTransform>(handle);
			ERR_FAIL_NULL_V(transform, ERR_INVALID_DATA);
			print_line(vformat("Energy orientation: %s position=(%s, %s, %s) forward=%s", expected_paths[i], transform->local.translation.x, transform->local.translation.y, transform->local.translation.z, -transform->local.basis.get_column(2)));
		}
		if (mesh) {
			print_line(vformat("Energy mesh: %s visible=%s surfaces=%d morphs=%d material=%s", expected_paths[i], mesh->visible, mesh->mesh->get_surface_count(), mesh->mesh->get_blend_shape_count(), mesh->material_override.is_valid() ? mesh->material_override->get_path() : String()));
		} else if (camera) {
			print_line(vformat("Energy camera: %s current=%s near=%s far=%s fov=%s", expected_paths[i], camera->current, camera->near_distance, camera->far_distance, camera->fov));
		} else if (light) {
			print_line(vformat("Energy light: %s type=%d shadow=%s energy=%s color=%s", expected_paths[i], light->type, light->shadow, light->energy, light->color));
		}
	}
	print_line("Converted energy_directional.escn: 8 records, 4 meshes, 1 visible mesh, 1 Stretch morph, 2 cameras, 1 directional light, 1 environment; saved components match readback.");
	return OK;
}

Error convert_main_entity_scene() {
	Ref<PackedScene> source = ResourceLoader::load("res://main.tscn");
	ERR_FAIL_COND_V(source.is_null(), ERR_FILE_MISSING_DEPENDENCIES);
	Ref<SceneState> state = source->get_state();
	Ref<FileAccess> input = FileAccess::open("res://entity_migration/resolved/main.tscn.jsonl", FileAccess::READ);
	ERR_FAIL_COND_V(input.is_null(), ERR_FILE_CANT_OPEN);
	Ref<DirAccess> directory = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	Error error = directory->make_dir_recursive("res://native_assets/main");
	ERR_FAIL_COND_V(error != OK, error);
	const char *mesh_names[] = { "Cube", "Plane", "Sphere", "Deformer" };
	Ref<Mesh> meshes[4];
	for (int i = 0; i < 4; i++) {
		String path = "res://native_assets/energy_directional/" + String(mesh_names[i]).to_snake_case() + ".res";
		meshes[i] = ResourceLoader::load(path);
		ERR_FAIL_COND_V_MSG(meshes[i].is_null(), ERR_FILE_MISSING_DEPENDENCIES, "Convert energy_directional before main: " + path);
		ERR_FAIL_COND_V(meshes[i]->get_surface_count() != 1 || meshes[i]->get_blend_shape_count() != (i == 3 ? 1 : 0), ERR_INVALID_DATA);
		Variant encoded;
		error = EntityCodec<Ref<Mesh>>::encode(meshes[i], encoded);
		ERR_FAIL_COND_V(error != OK, error);
		print_line("Main shared mesh: " + path + " " + String(encoded));
	}
	ERR_FAIL_COND_V(meshes[3]->get_blend_shape_name(0) != StringName("Stretch"), ERR_INVALID_DATA);
	Ref<Environment> environment_source = renderer_property(state, "WorldEnvironment", "environment");
	Ref<Environment> environment;
	error = save_renderer_asset(environment_source, "res://native_assets/main/environment.tres", environment);
	ERR_FAIL_COND_V(error != OK, error);
	ERR_FAIL_COND_V(environment->get_background() != Environment::BG_SKY || environment->get_sky().is_null(), ERR_INVALID_DATA);
	const char *groups[] = { "Floor", "BackWall", "PBR_0_0", "PBR_0_1", "PBR_0_2", "PBR_0_3", "PBR_1_0", "PBR_1_1", "PBR_1_2", "PBR_1_3", "MaskedPanel", "TexturedEmitter", "MovingCaster", "DeformingCaster", "OffscreenCaster" };
	const int visible_meshes[] = { 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 0, 3, 0 };
	const char *remaining_paths[] = { "Lights/Directional", "Lights/Point", "Lights/Spot", "Lights/Area", "Cameras/Camera", "Cameras/AlternateCamera" };
	Ref<EntityScene> scene;
	scene.instantiate();
	Vector<EntitySceneCommands::Command> commands;
	Vector<EntityId> ids;
	int material_count = 0;
	for (int i = 0; i < 67; i++) {
		String expected_path = i == 0 ? String("WorldEnvironment") : i <= 60 ? "Geometry/" + String(groups[(i - 1) / 4]) + "/" + mesh_names[(i - 1) % 4] : String(remaining_paths[i - 61]);
		Variant parsed = JSON::parse_string(input->get_line());
		ERR_FAIL_COND_V(parsed.get_type() != Variant::DICTIONARY, ERR_PARSE_ERROR);
		Dictionary row = parsed;
		ERR_FAIL_COND_V(row.get("path", String()) != Variant(expected_path), ERR_INVALID_DATA);
		Array pose = row.get("world_transform", Array());
		ERR_FAIL_COND_V(pose.size() != 12, ERR_INVALID_DATA);
		EntityId id;
		error = EntityId::generate(id);
		ERR_FAIL_COND_V(error != OK, error);
		ids.push_back(id);
		EntitySceneCommands::Command create;
		create.document = scene->get_document_id();
		create.entity = id;
		create.kind = EntitySceneCommands::CREATE;
		Dictionary record;
		record["components"] = Dictionary();
		create.after = record;
		commands.push_back(create);
		EntitySceneCommands::Command order = create;
		order.kind = EntitySceneCommands::SET_ORDER;
		order.before = int64_t(0);
		order.after = int64_t(i);
		commands.push_back(order);
		EntityName name;
		name.name = expected_path;
		error = add_energy_component(create.document, id, name, commands);
		ERR_FAIL_COND_V(error != OK, error);
		EntityTransform transform;
		transform.local.translation = { double(pose[9]), double(pose[10]), double(pose[11]) };
		transform.local.basis = Basis(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], pose[7], pose[8]);
		error = add_energy_component(create.document, id, transform, commands);
		ERR_FAIL_COND_V(error != OK, error);
		if (i == 0) {
			EntityEnvironment component;
			component.environment = environment;
			error = add_energy_component(create.document, id, component, commands);
		} else if (i <= 60) {
			int mesh_index = (i - 1) % 4;
			int group_index = (i - 1) / 4;
			Dictionary asset = row.get("mesh_asset", Dictionary());
			ERR_FAIL_COND_V(asset.get("path", String()) != Variant("res://geometry.gltf") || int(asset.get("gltf_mesh", -1)) != mesh_index, ERR_INVALID_DATA);
			EntityMesh component;
			component.mesh = meshes[mesh_index];
			component.visible = mesh_index == visible_meshes[group_index];
			ERR_FAIL_COND_V(bool(row.get("effective_visible", false)) != component.visible, ERR_INVALID_DATA);
			if (component.visible) {
				Ref<Material> material_source = renderer_property(state, expected_path, "material_override");
				error = save_renderer_asset(material_source, "res://native_assets/main/" + String(groups[group_index]).to_snake_case() + ".tres", component.material_override);
				ERR_FAIL_COND_V(error != OK, error);
				material_count++;
			}
			error = add_energy_component(create.document, id, component, commands);
			ERR_FAIL_COND_V(error != OK, error);
			EntityGeometry geometry;
			geometry.use_baked_light = true;
			if (mesh_index == 3) {
				geometry.blend_shape_weights.push_back(renderer_property(state, expected_path, "blend_shapes/Stretch", 0.0));
			}
			error = add_energy_component(create.document, id, geometry, commands);
		} else if (i <= 64) {
			EntityLight component;
			component.type = i - 61;
			component.color = renderer_property(state, expected_path, "light_color");
			component.energy = renderer_property(state, expected_path, "light_energy");
			component.shadow = renderer_property(state, expected_path, "shadow_enabled");
			if (i > 61) {
				component.specular = i == 64 ? 1.0 : 0.5;
				component.shadow_max_distance = 0.0;
				component.shadow_fade_start = 1.0;
				component.shadow_normal_bias = 1.0;
				component.intensity = 1000.0;
				component.range = renderer_property(state, expected_path, i == 62 ? "omni_range" : i == 63 ? "spot_range" : "area_range");
			}
			if (i == 63) {
				component.shadow_bias = 0.03;
				component.spot_angle = renderer_property(state, expected_path, "spot_angle");
			} else if (i == 64) {
				component.size = 0.5;
				component.area_size = renderer_property(state, expected_path, "area_size");
			}
			error = add_energy_component(create.document, id, component, commands);
		} else {
			EntityCamera component;
			component.current = renderer_property(state, expected_path, "current", false);
			component.far_distance = renderer_property(state, expected_path, "far", 4000.0);
			error = add_energy_component(create.document, id, component, commands);
		}
		ERR_FAIL_COND_V(error != OK, error);
	}
	ERR_FAIL_COND_V(material_count != 15, ERR_INVALID_DATA);
	while (!input->eof_reached()) {
		ERR_FAIL_COND_V(!input->get_line().strip_edges().is_empty(), ERR_INVALID_DATA);
	}
	error = scene->get_commands().execute("Convert preserved renderer gallery", commands);
	ERR_FAIL_COND_V_MSG(error != OK, error, scene->get_last_error());
	error = EntitySceneIO::save(**scene, "res://main.escn");
	ERR_FAIL_COND_V(error != OK, error);
	Ref<EntityScene> saved;
	error = EntitySceneIO::load("res://main.escn", saved);
	ERR_FAIL_COND_V(error != OK || saved.is_null(), ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(saved->get_record_count() != 67, ERR_INVALID_DATA);
	error = saved->load_subset(ids);
	ERR_FAIL_COND_V(error != OK || saved->get_resident_count() != 67, ERR_INVALID_DATA);
	for (const EntitySceneCommands::Command &command : commands) {
		if (command.kind != EntitySceneCommands::ADD_COMPONENT) {
			continue;
		}
		Variant value;
		error = saved->get_world()->read_component(saved->resolve(command.entity).handle, command.component, value);
		ERR_FAIL_COND_V(error != OK || value != command.after, ERR_INVALID_DATA);
	}
	for (int i = 0; i < ids.size(); i++) {
		ERR_FAIL_COND_V(saved->get_order(ids[i]) != i, ERR_INVALID_DATA);
		EntityHandle handle = saved->resolve(ids[i]).handle;
		const EntityName *name = saved->get_world()->get<EntityName>(handle);
		const EntityTransform *transform = saved->get_world()->get<EntityTransform>(handle);
		const EntityMesh *mesh = saved->get_world()->get<EntityMesh>(handle);
		const EntityCamera *camera = saved->get_world()->get<EntityCamera>(handle);
		const EntityLight *light = saved->get_world()->get<EntityLight>(handle);
		ERR_FAIL_NULL_V(name, ERR_INVALID_DATA);
		ERR_FAIL_NULL_V(transform, ERR_INVALID_DATA);
		if (mesh) {
			const EntityGeometry *geometry = saved->get_world()->get<EntityGeometry>(handle);
			ERR_FAIL_NULL_V(geometry, ERR_INVALID_DATA);
			ERR_FAIL_COND_V(mesh->mesh != meshes[(i - 1) % 4], ERR_INVALID_DATA);
			print_line(vformat("Main mesh: %s visible=%s asset=%s material=%s morph_weights=%s", name->name, mesh->visible, mesh->mesh->get_path(), mesh->material_override.is_valid() ? mesh->material_override->get_path() : String(), geometry->blend_shape_weights));
		} else if (camera || light) {
			print_line(vformat("Main orientation: %s position=(%s, %s, %s) forward=%s", name->name, transform->local.translation.x, transform->local.translation.y, transform->local.translation.z, -transform->local.basis.get_column(2)));
			if (camera) {
				print_line(vformat("Main camera: current=%s near=%s far=%s fov=%s", camera->current, camera->near_distance, camera->far_distance, camera->fov));
			} else {
				print_line(vformat("Main light: type=%d shadow=%s energy=%s color=%s range=%s", light->type, light->shadow, light->energy, light->color, light->range));
			}
		}
	}
	print_line("Converted main.escn: 67 records, 60 mesh instances sharing 4 meshes, 15 visible meshes, 45 hidden meshes, 15 materials, 15 Stretch targets, 2 cameras, 4 shadowed lights, 1 sky environment; saved components and order match readback. Static authored frame; animation and interactive controls remain pending.");
	return OK;
}

Error convert_microgeometry_stress_entity_scene() {
	uint64_t started = OS::get_singleton()->get_ticks_usec();
	print_line("Stress conversion: loading preserved shared assets.");
	Ref<PackedScene> source = ResourceLoader::load("res://microgeometry_stress/scene.tscn");
	ERR_FAIL_COND_V(source.is_null(), ERR_FILE_MISSING_DEPENDENCIES);
	Ref<SceneState> state = source->get_state();
	Ref<DirAccess> directory = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	const String asset_directory = "res://native_assets/microgeometry_stress/";
	Error error = directory->make_dir_recursive(asset_directory);
	ERR_FAIL_COND_V(error != OK, error);
	const char *mesh_names[] = { "lucy", "thai_statuette" };
	Ref<ArrayMesh> meshes[2];
	for (int i = 0; i < 2; i++) {
		String name = mesh_names[i];
		Ref<PackedScene> imported = ResourceLoader::load("res://microgeometry_stress/" + name + ".glb");
		ERR_FAIL_COND_V(imported.is_null(), ERR_FILE_MISSING_DEPENDENCIES);
		Ref<SceneState> imported_state = imported->get_state();
		Ref<ArrayMesh> original;
		int mesh_count = 0;
		for (int node = 0; node < imported_state->get_node_count(); node++) {
			if (imported_state->get_node_type(node) != StringName("MeshInstance3D")) {
				continue;
			}
			String path = String(imported_state->get_node_path(node)).trim_prefix("./");
			original = renderer_property(imported_state, path, "mesh");
			ERR_FAIL_COND_V(Transform3D(renderer_property(imported_state, path, "transform", Transform3D())) != Transform3D(), ERR_INVALID_DATA);
			mesh_count++;
		}
		ERR_FAIL_COND_V(mesh_count != 1 || original.is_null() || original->get_surface_count() != 1, ERR_INVALID_DATA);
		Ref<MicroGeometry> original_geometry = original->get_micro_geometry();
		ERR_FAIL_COND_V(original_geometry.is_null(), ERR_FILE_MISSING_DEPENDENCIES);
		Dictionary statistics = original_geometry->get_statistics();
		ERR_FAIL_COND_V(int(statistics.get("levels", 0)) == 0 || int(statistics.get("pages", 0)) == 0, ERR_INVALID_DATA);
		String geometry_path = asset_directory + name + ".mgdata";
		error = ResourceSaver::save(original_geometry, geometry_path);
		ERR_FAIL_COND_V(error != OK, error);
		Ref<MicroGeometry> geometry = ResourceLoader::load(geometry_path, "", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
		ERR_FAIL_COND_V(error != OK || geometry.is_null(), ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V(geometry->get_content_id() != original_geometry->get_content_id() || geometry->get_statistics() != statistics, ERR_INVALID_DATA);
		Ref<ArrayMesh> copy = original->duplicate_deep(RESOURCE_DEEP_DUPLICATE_INTERNAL);
		ERR_FAIL_COND_V(copy.is_null(), ERR_CANT_CREATE);
		copy->set_micro_geometry(geometry);
		error = save_renderer_asset(copy, asset_directory + name + ".res", meshes[i]);
		ERR_FAIL_COND_V(error != OK, error);
		Ref<MicroGeometry> saved_geometry = meshes[i]->get_micro_geometry();
		ERR_FAIL_COND_V(saved_geometry.is_null() || saved_geometry->get_path() != geometry_path, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(saved_geometry->get_content_id() != original_geometry->get_content_id() || saved_geometry->get_statistics() != statistics, ERR_INVALID_DATA);
		ERR_FAIL_COND_V(meshes[i]->get_aabb() != original->get_aabb() || meshes[i]->surface_get_arrays(0) != original->surface_get_arrays(0), ERR_INVALID_DATA);
		List<String> dependencies;
		ResourceLoader::get_dependencies(meshes[i]->get_path(), &dependencies);
		ERR_FAIL_COND_V(dependencies.size() != 1, ERR_INVALID_DATA);
		String dependency = dependencies.front()->get();
		if (dependency.get_slice_count("::") == 3) {
			ERR_FAIL_COND_V(ResourceUID::get_singleton()->text_to_id(dependency.get_slice("::", 0)) != ResourceLoader::get_resource_uid(geometry_path), ERR_INVALID_DATA);
			dependency = dependency.get_slice("::", 2);
		}
		ERR_FAIL_COND_V(dependency != geometry_path, ERR_INVALID_DATA);
		print_line(vformat("Stress shared mesh: %s geometry=%s content_id=%s aabb=%s statistics=%s elapsed_ms=%s", meshes[i]->get_path(), geometry_path, saved_geometry->get_content_id(), meshes[i]->get_aabb(), JSON::stringify(statistics), (OS::get_singleton()->get_ticks_usec() - started) / 1000.0));
	}
	Ref<Mesh> floor_source = renderer_property(state, "Floor", "mesh");
	Ref<Mesh> floor;
	error = save_renderer_asset(floor_source, asset_directory + "floor.res", floor);
	ERR_FAIL_COND_V(error != OK, error);
	Ref<Environment> environment_source = renderer_property(state, "WorldEnvironment", "environment");
	Ref<Environment> environment;
	error = save_renderer_asset(environment_source, asset_directory + "environment.tres", environment);
	ERR_FAIL_COND_V(error != OK, error);
	print_line(vformat("Stress conversion: shared assets ready elapsed_ms=%s; authoring 10004 preserved records.", (OS::get_singleton()->get_ticks_usec() - started) / 1000.0));
	Ref<FileAccess> input = FileAccess::open("res://entity_migration/resolved/microgeometry_stress/scene.tscn.jsonl", FileAccess::READ);
	ERR_FAIL_COND_V(input.is_null(), ERR_FILE_CANT_OPEN);
	Ref<EntityScene> scene;
	scene.instantiate();
	Vector<EntitySceneCommands::Command> commands;
	Vector<EntityId> ids;
	int counts[2] = {};
	for (int i = 0; i < 10004; i++) {
		Variant parsed = JSON::parse_string(input->get_line());
		ERR_FAIL_COND_V(parsed.get_type() != Variant::DICTIONARY, ERR_PARSE_ERROR);
		Dictionary row = parsed;
		String path = row.get("path", String());
		Array pose = row.get("world_transform", Array());
		ERR_FAIL_COND_V(pose.size() != 12 || !bool(row.get("effective_visible", false)), ERR_INVALID_DATA);
		EntityId id;
		error = EntityId::generate(id);
		ERR_FAIL_COND_V(error != OK, error);
		ids.push_back(id);
		EntitySceneCommands::Command create;
		create.document = scene->get_document_id();
		create.entity = id;
		create.kind = EntitySceneCommands::CREATE;
		Dictionary record;
		record["components"] = Dictionary();
		create.after = record;
		commands.push_back(create);
		EntitySceneCommands::Command order = create;
		order.kind = EntitySceneCommands::SET_ORDER;
		order.before = int64_t(0);
		order.after = int64_t(i);
		commands.push_back(order);
		EntityName name;
		name.name = path;
		error = add_energy_component(create.document, id, name, commands);
		ERR_FAIL_COND_V(error != OK, error);
		EntityTransform transform;
		transform.local.translation = { double(pose[9]), double(pose[10]), double(pose[11]) };
		transform.local.basis = Basis(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], pose[7], pose[8]);
		if (i >= 10002) {
			Variant authored_rotation = renderer_property(state, path, "rotation_degrees");
			ERR_FAIL_COND_V(authored_rotation.get_type() != Variant::VECTOR3, ERR_INVALID_DATA);
			Vector3 degrees = authored_rotation;
			transform.local.basis = Basis::from_euler(Vector3(Math::deg_to_rad(degrees.x), Math::deg_to_rad(degrees.y), Math::deg_to_rad(degrees.z)), EulerOrder::YXZ);
			print_line(vformat("Stress authored orientation: %s degrees=%s position=(%s, %s, %s) forward=%s", path, degrees, transform.local.translation.x, transform.local.translation.y, transform.local.translation.z, -transform.local.basis.get_column(2)));
		}
		error = add_energy_component(create.document, id, transform, commands);
		ERR_FAIL_COND_V(error != OK, error);
		if (i == 0) {
			ERR_FAIL_COND_V(path != "WorldEnvironment", ERR_INVALID_DATA);
			EntityEnvironment component;
			component.environment = environment;
			error = add_energy_component(create.document, id, component, commands);
		} else if (i <= 10001) {
			EntityMesh component;
			EntityGeometry geometry;
			if (i <= 10000) {
				Dictionary asset = row.get("mesh_asset", Dictionary());
				String asset_path = asset.get("path", String());
				int mesh_index = asset_path == "res://microgeometry_stress/lucy.glb" ? 0 : 1;
				ERR_FAIL_COND_V(asset_path != "res://microgeometry_stress/" + String(mesh_names[mesh_index]) + ".glb" || int(asset.get("gltf_mesh", -1)) != 0, ERR_INVALID_DATA);
				String expected = "Instances/" + String(mesh_index == 0 ? "Lucy" : "Thai") + "_" + String::num_int64(counts[mesh_index]).pad_zeros(4) + "/" + mesh_names[mesh_index];
				ERR_FAIL_COND_V(path != expected, ERR_INVALID_DATA);
				component.mesh = meshes[mesh_index];
				geometry.use_baked_light = true;
				counts[mesh_index]++;
			} else {
				ERR_FAIL_COND_V(path != "Floor", ERR_INVALID_DATA);
				component.mesh = floor;
			}
			error = add_energy_component(create.document, id, component, commands);
			ERR_FAIL_COND_V(error != OK, error);
			error = add_energy_component(create.document, id, geometry, commands);
		} else if (i == 10002) {
			ERR_FAIL_COND_V(path != "Sun", ERR_INVALID_DATA);
			EntityLight component;
			component.energy = renderer_property(state, path, "light_energy", 1.0);
			component.shadow = renderer_property(state, path, "shadow_enabled", false);
			component.shadow_max_distance = renderer_property(state, path, "directional_shadow_max_distance", 100.0);
			error = add_energy_component(create.document, id, component, commands);
		} else {
			ERR_FAIL_COND_V(path != "Camera", ERR_INVALID_DATA);
			EntityCamera component;
			component.current = renderer_property(state, path, "current", false);
			component.fov = renderer_property(state, path, "fov", 75.0);
			component.far_distance = renderer_property(state, path, "far", 4000.0);
			error = add_energy_component(create.document, id, component, commands);
		}
		ERR_FAIL_COND_V(error != OK, error);
	}
	ERR_FAIL_COND_V(counts[0] != 5000 || counts[1] != 5000, ERR_INVALID_DATA);
	while (!input->eof_reached()) {
		ERR_FAIL_COND_V(!input->get_line().strip_edges().is_empty(), ERR_INVALID_DATA);
	}
	print_line(vformat("Stress conversion: execute %d commands elapsed_ms=%s.", commands.size(), (OS::get_singleton()->get_ticks_usec() - started) / 1000.0));
	error = scene->get_commands().execute("Convert preserved microgeometry stress scene", commands);
	ERR_FAIL_COND_V_MSG(error != OK, error, scene->get_last_error());
	print_line(vformat("Stress conversion: transaction complete elapsed_ms=%s; saving native scene.", (OS::get_singleton()->get_ticks_usec() - started) / 1000.0));
	error = EntitySceneIO::save(**scene, "res://microgeometry_stress/scene.escn");
	ERR_FAIL_COND_V(error != OK, error);
	print_line(vformat("Stress conversion: saved elapsed_ms=%s; loading native records.", (OS::get_singleton()->get_ticks_usec() - started) / 1000.0));
	Ref<EntityScene> saved;
	error = EntitySceneIO::load("res://microgeometry_stress/scene.escn", saved);
	ERR_FAIL_COND_V(error != OK || saved.is_null(), ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(saved->get_record_count() != 10004, ERR_INVALID_DATA);
	error = saved->load_subset(ids);
	ERR_FAIL_COND_V(error != OK || saved->get_resident_count() != 10004, ERR_INVALID_DATA);
	for (const EntitySceneCommands::Command &command : commands) {
		if (command.kind != EntitySceneCommands::ADD_COMPONENT) {
			continue;
		}
		Variant value;
		error = saved->get_world()->read_component(saved->resolve(command.entity).handle, command.component, value);
		ERR_FAIL_COND_V(error != OK || value != command.after, ERR_INVALID_DATA);
	}
	int saved_counts[2] = {};
	Ref<Mesh> shared_readback[2];
	for (int i = 0; i < ids.size(); i++) {
		ERR_FAIL_COND_V(saved->get_order(ids[i]) != i, ERR_INVALID_DATA);
		EntityHandle handle = saved->resolve(ids[i]).handle;
		if (i > 0 && i <= 10000) {
			const EntityMesh *mesh = saved->get_world()->get<EntityMesh>(handle);
			ERR_FAIL_NULL_V(mesh, ERR_INVALID_DATA);
			ERR_FAIL_COND_V(!mesh->visible || mesh->mesh.is_null(), ERR_INVALID_DATA);
			int mesh_index = mesh->mesh->get_path() == meshes[0]->get_path() ? 0 : 1;
			ERR_FAIL_COND_V(mesh->mesh->get_path() != meshes[mesh_index]->get_path(), ERR_INVALID_DATA);
			if (shared_readback[mesh_index].is_null()) {
				shared_readback[mesh_index] = mesh->mesh;
			}
			ERR_FAIL_COND_V(mesh->mesh != shared_readback[mesh_index], ERR_INVALID_DATA);
			saved_counts[mesh_index]++;
		}
	}
	ERR_FAIL_COND_V(saved_counts[0] != 5000 || saved_counts[1] != 5000, ERR_INVALID_DATA);
	print_line(vformat("Converted microgeometry_stress/scene.escn: 10004 records, 5000 Lucy + 5000 Thai sharing 2 meshes and 2 microgeometry assets, 1 floor, 1 camera, 1 shadowed sun, 1 sky environment; all saved components, order and mesh sharing match readback. elapsed_ms=%s. Static authored frame; orbit and interactive controls remain pending.", (OS::get_singleton()->get_ticks_usec() - started) / 1000.0));
	return OK;
}
