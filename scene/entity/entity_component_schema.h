#pragma once

#include "entity_components.h"
#include "entity_flecs.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

#include <limits>

template <typename T>
struct EntityComponentTraits;

template <typename T>
struct EntityCodec;

struct EntityFieldSchema {
	uint64_t id = 0;
	StringName name;
	StringName native_type;
	uint64_t nested_type_id = 0;
	Variant::Type variant_type = Variant::NIL;
	bool serialized = true;
	bool editable = true;
	bool entity_reference = false;
	bool asset_reference = false;
	bool has_range = false;
	double minimum = 0.0;
	double maximum = 0.0;
	StringName unit;
	Variant default_value;
	Error (*read)(const void *, Variant &) = nullptr;
	Error (*write)(void *, const Variant &) = nullptr;
	Error (*validate)(const Variant &) = nullptr;
	Error (*make_array_element)(Variant &) = nullptr;
};

struct EntityComponentSchema {
	uint64_t id = 0;
	StringName name;
	ecs_entity_t runtime_id = 0;
	bool is_component = false;
	Vector<EntityFieldSchema> fields;
	Error (*encode)(const void *, Variant &) = nullptr;
	Error (*decode)(void *, const Variant &) = nullptr;
	Error (*set_field)(flecs::world &, ecs_entity_t, uint64_t, const Variant &) = nullptr;
	Error (*set_serialized)(flecs::world &, ecs_entity_t, const Variant &) = nullptr;
	void (*add_default)(flecs::world &, ecs_entity_t) = nullptr;
	void (*copy_to)(flecs::world &, ecs_entity_t, const void *) = nullptr;
	const EntityFieldSchema *find_field(uint64_t p_id) const;
};

class EntitySchemaRegistry {
	HashMap<uint64_t, EntityComponentSchema> components;

public:
	void add(EntityComponentSchema p_schema);
	const EntityComponentSchema *find(uint64_t p_id) const { return components.getptr(p_id); }
	const HashMap<uint64_t, EntityComponentSchema> &get_types() const { return components; }
};

void initialize_entity_types();
void register_entity_component_schemas(flecs::world &p_world, EntitySchemaRegistry &r_registry);

Error entity_encode_asset(const Ref<Resource> &p_asset, Variant &r_value);
Error entity_decode_asset(const Variant &p_value, Ref<Resource> &r_asset);

template <typename T>
ecs_entity_t entity_string_meta(flecs::world &p_world) {
	auto component = p_world.component<T>();
	if (!ecs_has_id(p_world.c_ptr(), component.id(), ecs_id(EcsOpaque))) {
		component.opaque(flecs::String).serialize([](const flecs::serializer *p_serializer, const T *p_value) -> int {
			Variant value;
			if (EntityCodec<T>::encode(*p_value, value) != OK) {
				return -1;
			}
			CharString utf8 = String(value).utf8();
			const char *text = utf8.get_data();
			return p_serializer->value(flecs::String, &text);
		});
	}
	return component.id();
}

template <>
struct EntityCodec<bool> {
	static constexpr Variant::Type variant_type = Variant::BOOL;
	static Error encode(bool p_value, Variant &r_value) {
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, bool &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &) { return flecs::Bool; }
};

template <>
struct EntityCodec<uint32_t> {
	static constexpr Variant::Type variant_type = Variant::INT;
	static Error encode(uint32_t p_value, Variant &r_value) {
		r_value = int64_t(p_value);
		return OK;
	}
	static Error decode(const Variant &p_value, uint32_t &r_value) {
		if (p_value.get_type() != variant_type || int64_t(p_value) < 0 || int64_t(p_value) > UINT32_MAX) {
			return ERR_INVALID_DATA;
		}
		r_value = uint32_t(int64_t(p_value));
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &) { return flecs::U32; }
};

template <>
struct EntityCodec<double> {
	static constexpr Variant::Type variant_type = Variant::FLOAT;
	static Error encode(double p_value, Variant &r_value) {
		if (!Math::is_finite(p_value)) {
			return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, double &r_value) {
		if (p_value.get_type() != Variant::FLOAT && p_value.get_type() != Variant::INT) {
			return ERR_INVALID_DATA;
		}
		double value = p_value;
		if (!Math::is_finite(value)) {
			return ERR_INVALID_DATA;
		}
		r_value = value;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &) { return flecs::F64; }
};

template <>
struct EntityCodec<String> {
	static constexpr Variant::Type variant_type = Variant::STRING;
	static Error encode(const String &p_value, Variant &r_value) {
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, String &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<String>(p_world); }
};

template <>
struct EntityCodec<EntityId> {
	static constexpr Variant::Type variant_type = Variant::STRING;
	static Error encode(EntityId p_value, Variant &r_value) {
		r_value = p_value.to_string();
		return OK;
	}
	static Error decode(const Variant &p_value, EntityId &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		return EntityId::parse(p_value, r_value);
	}
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<EntityId>(p_world); }
};

template <>
struct EntityCodec<EntityRef> {
	static constexpr Variant::Type variant_type = Variant::STRING;
	static Error encode(const EntityRef &p_value, Variant &r_value) { return EntityCodec<EntityId>::encode(p_value.id, r_value); }
	static Error decode(const Variant &p_value, EntityRef &r_value) { return EntityCodec<EntityId>::decode(p_value, r_value.id); }
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<EntityRef>(p_world); }
};

template <>
struct EntityCodec<Basis> {
	static constexpr Variant::Type variant_type = Variant::BASIS;
	static Error encode(const Basis &p_value, Variant &r_value) {
		if (!p_value.is_finite()) {
			return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, Basis &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		Basis value = p_value;
		if (!value.is_finite()) {
			return ERR_INVALID_DATA;
		}
		r_value = value;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &p_world);
};

template <typename T, Variant::Type Type>
struct EntityFiniteVariantCodec {
	static constexpr Variant::Type variant_type = Type;
	static Error encode(const T &p_value, Variant &r_value) {
		if (!p_value.is_finite()) {
			return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, T &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		T value = p_value;
		if (!value.is_finite()) {
			return ERR_INVALID_DATA;
		}
		r_value = value;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<T>(p_world); }
};

template <>
struct EntityCodec<Vector2> : EntityFiniteVariantCodec<Vector2, Variant::VECTOR2> {};

template <>
struct EntityCodec<Vector3> : EntityFiniteVariantCodec<Vector3, Variant::VECTOR3> {};

template <>
struct EntityCodec<Rect2> : EntityFiniteVariantCodec<Rect2, Variant::RECT2> {};

template <>
struct EntityCodec<AABB> : EntityFiniteVariantCodec<AABB, Variant::AABB> {};

template <>
struct EntityCodec<Color> {
	static constexpr Variant::Type variant_type = Variant::COLOR;
	static Error encode(const Color &p_value, Variant &r_value) {
		if (!Math::is_finite(p_value.r) || !Math::is_finite(p_value.g) || !Math::is_finite(p_value.b) || !Math::is_finite(p_value.a)) {
			return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, Color &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		Color value = p_value;
		Variant encoded;
		Error error = encode(value, encoded);
		if (error == OK) {
			r_value = value;
		}
		return error;
	}
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<Color>(p_world); }
};

template <>
struct EntityCodec<Variant> {
	static constexpr Variant::Type variant_type = Variant::NIL;
	static Error encode(const Variant &p_value, Variant &r_value) {
		switch (p_value.get_type()) {
			case Variant::NIL:
			case Variant::BOOL:
			case Variant::INT:
			case Variant::VECTOR2I:
			case Variant::VECTOR3I:
			case Variant::VECTOR4I:
				break;
			case Variant::FLOAT:
				if (!Math::is_finite(double(p_value))) {
					return ERR_INVALID_DATA;
				}
				break;
			case Variant::VECTOR2:
				return EntityCodec<Vector2>::encode(p_value, r_value);
			case Variant::VECTOR3:
				return EntityCodec<Vector3>::encode(p_value, r_value);
			case Variant::VECTOR4:
				if (!Vector4(p_value).is_finite()) {
					return ERR_INVALID_DATA;
				}
				break;
			case Variant::COLOR:
				return EntityCodec<Color>::encode(p_value, r_value);
			default:
				return ERR_INVALID_DATA;
		}
		r_value = p_value;
		return OK;
	}
	static Error decode(const Variant &p_value, Variant &r_value) { return encode(p_value, r_value); }
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<Variant>(p_world); }
};

template <typename T>
struct EntityCodec<Ref<T>> {
	static constexpr Variant::Type variant_type = Variant::STRING;
	static Error encode(const Ref<T> &p_value, Variant &r_value) { return entity_encode_asset(p_value, r_value); }
	static Error decode(const Variant &p_value, Ref<T> &r_value) {
		Ref<Resource> asset;
		Error error = entity_decode_asset(p_value, asset);
		if (error != OK) {
			return error;
		}
		Ref<T> typed = asset;
		if (asset.is_valid() && typed.is_null()) {
			return ERR_INVALID_DATA;
		}
		r_value = typed;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &p_world) { return entity_string_meta<Ref<T>>(p_world); }
};

template <typename T>
struct EntityCodec<Vector<T>> {
	static constexpr Variant::Type variant_type = Variant::ARRAY;
	static Error make_default_element(Variant &r_value) { return EntityCodec<T>::encode(T(), r_value); }
	static Error encode(const Vector<T> &p_value, Variant &r_value) {
		Array values;
		values.resize(p_value.size());
		for (int i = 0; i < p_value.size(); i++) {
			Variant value;
			Error error = EntityCodec<T>::encode(p_value[i], value);
			if (error != OK) {
				return error;
			}
			values[i] = value;
		}
		r_value = values;
		return OK;
	}
	static Error decode(const Variant &p_value, Vector<T> &r_value) {
		if (p_value.get_type() != variant_type) {
			return ERR_INVALID_DATA;
		}
		Array values = p_value;
		Vector<T> result;
		result.resize(values.size());
		for (int i = 0; i < values.size(); i++) {
			Error error = EntityCodec<T>::decode(values[i], result.write[i]);
			if (error != OK) {
				return error;
			}
		}
		r_value = result;
		return OK;
	}
	static ecs_entity_t meta_type(flecs::world &p_world) {
		auto component = p_world.component<Vector<T>>();
		if (!ecs_has_id(p_world.c_ptr(), component.id(), ecs_id(EcsOpaque))) {
			ecs_vector_desc_t descriptor = {};
			descriptor.type = EntityCodec<T>::meta_type(p_world);
			ecs_entity_t vector_type = ecs_vector_init(p_world.c_ptr(), &descriptor);
			component.template opaque<T>(vector_type)
					.count([](const Vector<T> *p_value) -> size_t { return p_value->size(); })
					.serialize([](const flecs::serializer *p_serializer, const Vector<T> *p_value) -> int {
						for (const T &value : *p_value) {
							if (p_serializer->value(value) != 0) {
								return -1;
							}
						}
						return 0;
					});
		}
		return component.id();
	}
};

template <typename C, typename T, T C::*Member>
EntityFieldSchema entity_make_field(uint64_t p_id, const char *p_name, const char *p_type) {
	EntityFieldSchema result;
	result.id = p_id;
	result.name = p_name;
	result.native_type = p_type;
	result.variant_type = EntityCodec<T>::variant_type;
	if constexpr (EntityCodec<T>::variant_type == Variant::ARRAY) {
		result.make_array_element = &EntityCodec<T>::make_default_element;
	}
	result.read = [](const void *p_component, Variant &r_value) { return EntityCodec<T>::encode(static_cast<const C *>(p_component)->*Member, r_value); };
	result.write = [](void *p_component, const Variant &p_value) { return EntityCodec<T>::decode(p_value, static_cast<C *>(p_component)->*Member); };
	result.validate = [](const Variant &p_value) {
		T value;
		return EntityCodec<T>::decode(p_value, value);
	};
	C defaults;
	Error error = result.read(&defaults, result.default_value);
	ERR_FAIL_COND_V(error != OK, EntityFieldSchema());
	return result;
}

template <typename T>
Error entity_encode_struct(const T &p_value, Variant &r_value) {
	Dictionary fields;
	for (const EntityFieldSchema &field : EntityComponentTraits<T>::fields()) {
		if (!field.serialized) {
			continue;
		}
		Variant value;
		Error error = field.read(&p_value, value);
		ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot encode entity field " + String(field.name));
		fields[String::num_uint64(field.id, 16)] = value;
	}
	r_value = fields;
	return OK;
}

template <typename T>
Error entity_decode_struct(const Variant &p_value, T &r_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return ERR_INVALID_DATA;
	}
	Dictionary values = p_value;
	Vector<EntityFieldSchema> fields = EntityComponentTraits<T>::fields();
	T result;
	int recognized = 0;
	for (const EntityFieldSchema &field : fields) {
		String key = String::num_uint64(field.id, 16);
		if (!field.serialized || !values.has(key)) {
			continue;
		}
		Error error = field.write(&result, values[key]);
		ERR_FAIL_COND_V_MSG(error != OK, error, "Cannot decode entity field " + String(field.name));
		recognized++;
	}
	if (recognized != values.size()) {
		return ERR_INVALID_DATA;
	}
	r_value = result;
	return OK;
}

template <typename T>
EntityComponentSchema entity_make_component(flecs::world &p_world) {
	EntityComponentSchema result;
	result.id = EntityComponentTraits<T>::id;
	result.name = EntityComponentTraits<T>::name;
	result.runtime_id = EntityCodec<T>::meta_type(p_world);
	result.is_component = EntityComponentTraits<T>::is_component;
	result.fields = EntityComponentTraits<T>::fields();
	result.encode = [](const void *p_component, Variant &r_value) { return EntityCodec<T>::encode(*static_cast<const T *>(p_component), r_value); };
	result.decode = [](void *p_component, const Variant &p_value) { return EntityCodec<T>::decode(p_value, *static_cast<T *>(p_component)); };
	result.add_default = [](flecs::world &p_ecs, ecs_entity_t p_entity) { p_ecs.entity(p_entity).template set<T>(T()); };
	result.copy_to = [](flecs::world &p_ecs, ecs_entity_t p_entity, const void *p_value) { p_ecs.entity(p_entity).template set<T>(*static_cast<const T *>(p_value)); };
	result.set_serialized = [](flecs::world &p_ecs, ecs_entity_t p_entity, const Variant &p_value) -> Error {
		T value;
		Error error = EntityCodec<T>::decode(p_value, value);
		if (error != OK) {
			return error;
		}
		p_ecs.entity(p_entity).template set<T>(value);
		return OK;
	};
	result.set_field = [](flecs::world &p_ecs, ecs_entity_t p_entity, uint64_t p_field, const Variant &p_value) -> Error {
		const T *existing = p_ecs.entity(p_entity).template try_get<T>();
		if (!existing) {
			return ERR_DOES_NOT_EXIST;
		}
		T value = *existing;
		for (const EntityFieldSchema &field : EntityComponentTraits<T>::fields()) {
			if (field.id != p_field) {
				continue;
			}
			if (!field.editable) {
				return ERR_UNAUTHORIZED;
			}
			Error error = field.write(&value, p_value);
			if (error != OK) {
				return error;
			}
			p_ecs.entity(p_entity).template set<T>(value);
			return OK;
		}
		return ERR_DOES_NOT_EXIST;
	};
	return result;
}
