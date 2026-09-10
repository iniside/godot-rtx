#pragma once

#include "entity_id.h"

#include "core/math/basis.h"
#include "core/templates/vector.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

#ifdef ENTITY_SCHEMA_SCAN
#define ENTITY_COMPONENT(m_data) __attribute__((annotate("entity_component:" m_data)))
#define ENTITY_VALUE(m_data) __attribute__((annotate("entity_value:" m_data)))
#define ENTITY_FIELD(m_data) __attribute__((annotate("entity_field:" m_data)))
#else
#define ENTITY_COMPONENT(m_data)
#define ENTITY_VALUE(m_data)
#define ENTITY_FIELD(m_data)
#endif

struct ENTITY_VALUE("id=1000000000000001") EntityPosition {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	double x = 0.0;
	ENTITY_FIELD("id=0000000000000002;unit=m")
	double y = 0.0;
	ENTITY_FIELD("id=0000000000000003;unit=m")
	double z = 0.0;
};

struct ENTITY_VALUE("id=1000000000000002") EntityPose {
	ENTITY_FIELD("id=0000000000000001;unit=m")
	EntityPosition translation;
	ENTITY_FIELD("id=0000000000000002")
	Basis basis;
};

struct ENTITY_COMPONENT("id=2000000000000001") EntityName {
	ENTITY_FIELD("id=0000000000000001")
	String name;
	ENTITY_FIELD("id=0000000000000002;reference=entity")
	EntityRef document_group;
};

struct ENTITY_COMPONENT("id=2000000000000002") EntityTransform {
	ENTITY_FIELD("id=0000000000000001")
	EntityPose local;
	ENTITY_FIELD("id=0000000000000002;serialize=false;edit=false")
	EntityPose current;
	ENTITY_FIELD("id=0000000000000003;serialize=false;edit=false")
	EntityPose previous;
	ENTITY_FIELD("id=0000000000000004;serialize=false;edit=false")
	EntityPose render;
};

struct ENTITY_COMPONENT("id=2000000000000003") EntityMesh {
	ENTITY_FIELD("id=0000000000000001;reference=asset")
	Ref<Mesh> mesh;
	ENTITY_FIELD("id=0000000000000002;reference=asset")
	Ref<Material> material_override;
	ENTITY_FIELD("id=0000000000000003;reference=asset")
	Vector<Ref<Material>> surface_materials;
	ENTITY_FIELD("id=0000000000000004")
	bool visible = true;
	ENTITY_FIELD("id=0000000000000005;min=0;max=4294967295")
	uint32_t layers = 1;
};

struct ENTITY_COMPONENT("id=2000000000000004") EntityVisibility {
	ENTITY_FIELD("id=0000000000000001")
	bool visible = true;
	ENTITY_FIELD("id=0000000000000002")
	bool inherit_parent = false;
	ENTITY_FIELD("id=0000000000000003;serialize=false;edit=false")
	bool effective = true;
};
