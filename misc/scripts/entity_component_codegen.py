#!/usr/bin/env python3

import json
import math
import os
from pathlib import Path
import re
import subprocess


SCALARS = {"bool", "uint32_t", "double", "String", "Basis", "EntityId", "EntityRef"}
ANNOTATION = re.compile(r'ENTITY_(COMPONENT|VALUE|FIELD)\(\s*"([^"\n]+)"\s*\)')
DECLARATION = re.compile(r'\b(?:struct|class)\s+ENTITY_(?:COMPONENT|VALUE)\(\s*"[^"\n]+"\s*\)\s+(\w+)')


def annotations(node, text):
    result = []
    for child in node.get("inner", []):
        if child.get("kind") != "AnnotateAttr":
            continue
        begin = child["range"]["begin"]
        offset = begin.get("expansionLoc", begin).get("offset", -1)
        match = ANNOTATION.match(text, offset)
        if not match:
            continue
        pairs = [part.split("=", 1) for part in match[2].split(";")]
        metadata = dict(pairs)
        if len(metadata) != len(pairs):
            raise ValueError(f"Duplicate metadata for {node.get('name')}")
        allowed = {"id"} if match[1] != "FIELD" else {"id", "unit", "serialize", "edit", "reference", "min", "max"}
        if metadata.keys() - allowed:
            raise ValueError(f"Unknown metadata for {node.get('name')}: {metadata.keys() - allowed}")
        if not re.fullmatch(r"[0-9a-f]{16}", metadata.get("id", "")) or int(metadata["id"], 16) == 0:
            raise ValueError(f"Expected nonzero 16-digit schema ID for {node.get('name')}")
        for option in ("serialize", "edit"):
            if metadata.get(option, "true") not in ("true", "false"):
                raise ValueError(f"Invalid {option} for {node.get('name')}")
        if metadata.get("reference", "") not in ("", "entity", "asset"):
            raise ValueError(f"Invalid reference for {node.get('name')}")
        if ("min" in metadata) != ("max" in metadata):
            raise ValueError(f"Range requires min and max for {node.get('name')}")
        if "min" in metadata:
            low, high = float(metadata["min"]), float(metadata["max"])
            if not math.isfinite(low) or not math.isfinite(high) or low > high:
                raise ValueError(f"Invalid range for {node.get('name')}")
        result.append((match[1], metadata))
    return result


def parse_records(ast_text, source_text):
    decoder = json.JSONDecoder()
    records = []
    offset = 0
    nodes = []
    while offset < len(ast_text):
        while offset < len(ast_text) and ast_text[offset].isspace():
            offset += 1
        if offset == len(ast_text):
            break
        node, offset = decoder.raw_decode(ast_text, offset)
        nodes.append(node)
    while nodes:
        node = nodes.pop(0)
        if node.get("kind") in ("TranslationUnitDecl", "NamespaceDecl"):
            nodes[0:0] = node.get("inner", [])
            continue
        if node.get("kind") != "CXXRecordDecl" or not node.get("completeDefinition"):
            continue
        marks = annotations(node, source_text)
        if not marks:
            continue
        if len(marks) != 1 or marks[0][0] not in ("COMPONENT", "VALUE") or node.get("bases"):
            raise ValueError(f"Unsupported scene entity declaration {node.get('name')}")
        record = {"name": node["name"], "kind": marks[0][0], "metadata": marks[0][1], "fields": []}
        ids = set()
        for field in node.get("inner", []):
            if field.get("kind") != "FieldDecl":
                continue
            field_marks = annotations(field, source_text)
            if len(field_marks) != 1 or field_marks[0][0] != "FIELD" or field.get("isBitfield"):
                raise ValueError(f"Field needs an explicit schema ID: {record['name']}::{field.get('name')}")
            metadata = field_marks[0][1]
            if metadata["id"] in ids:
                raise ValueError(f"Duplicate field ID in {record['name']}: {metadata['id']}")
            ids.add(metadata["id"])
            record["fields"].append({"name": field["name"], "type": field["type"]["qualType"], "metadata": metadata})
        records.append(record)
    if not records:
        raise ValueError("Clang found no annotated scene entity declarations")
    ids = [record["metadata"]["id"] for record in records]
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate scene entity type ID")
    names = {record["name"] for record in records}

    def supported(native_type):
        if native_type in SCALARS or native_type in names:
            return True
        match = re.fullmatch(r"Vector<(.+)>", native_type)
        if match:
            return supported(match[1].strip())
        return native_type in ("Ref<Mesh>", "Ref<Material>")

    for record in records:
        for field in record["fields"]:
            if not supported(field["type"]):
                raise ValueError(f"Unsupported field type {record['name']}::{field['name']}: {field['type']}")
    return records


def emit(records):
    type_ids = {record["name"]: record["metadata"]["id"] for record in records}
    header = ['#pragma once', '', '#include "entity_component_schema.h"', '']
    implementation = ['#include "entity_component_schema.gen.h"', '', '#include <cstddef>', '']
    for record in records:
        name = record["name"]
        header += [
            'template <>', f'struct EntityComponentTraits<{name}> {{',
            f'\tstatic constexpr uint64_t id = 0x{record["metadata"]["id"]}ULL;',
            f'\tstatic constexpr const char *name = "{name}";',
            f'\tstatic constexpr bool is_component = {str(record["kind"] == "COMPONENT").lower()};',
            '\tstatic Vector<EntityFieldSchema> fields();', '};', '',
            'template <>', f'struct EntityCodec<{name}> {{',
            '\tstatic constexpr Variant::Type variant_type = Variant::DICTIONARY;',
            f'\tstatic Error encode(const {name} &p_value, Variant &r_value) {{ return entity_encode_struct(p_value, r_value); }}',
            f'\tstatic Error decode(const Variant &p_value, {name} &r_value) {{ return entity_decode_struct(p_value, r_value); }}',
            '\tstatic ecs_entity_t meta_type(flecs::world &p_world);', '};', '',
        ]
        implementation += [f'Vector<EntityFieldSchema> EntityComponentTraits<{name}>::fields() {{', '\tVector<EntityFieldSchema> fields;']
        for field in record["fields"]:
            metadata = field["metadata"]
            implementation += [
                '\t{', f'\t\tEntityFieldSchema field = entity_make_field<{name}, {field["type"]}, &{name}::{field["name"]}>(0x{metadata["id"]}ULL, "{field["name"]}", "{field["type"]}");',
                f'\t\tfield.serialized = {metadata.get("serialize", "true")};',
                f'\t\tfield.editable = {metadata.get("edit", "true")};',
                f'\t\tfield.entity_reference = {str(metadata.get("reference") == "entity").lower()};',
                f'\t\tfield.asset_reference = {str(metadata.get("reference") == "asset").lower()};',
            ]
            if "unit" in metadata:
                implementation += [f'\t\tfield.unit = {json.dumps(metadata["unit"])};']
            if field["type"] in type_ids:
                implementation += [f'\t\tfield.nested_type_id = 0x{type_ids[field["type"]]}ULL;']
            if "min" in metadata:
                implementation += ['\t\tfield.has_range = true;', f'\t\tfield.minimum = {float(metadata["min"])};', f'\t\tfield.maximum = {float(metadata["max"])};']
            implementation += ['\t\tfields.push_back(field);', '\t}']
        implementation += ['\treturn fields;', '}', '', f'ecs_entity_t EntityCodec<{name}>::meta_type(flecs::world &p_world) {{', f'\tauto component = p_world.component<{name}>("{name}");', '\tif (!ecs_has_id(p_world.c_ptr(), component.id(), ecs_id(EcsStruct))) {']
        for field in record["fields"]:
            implementation += [f'\t\tcomponent.member(EntityCodec<{field["type"]}>::meta_type(p_world), "{field["name"]}", 1, offsetof({name}, {field["name"]}));']
        implementation += ['\t}', '\treturn component.id();', '}', '']
    implementation += ['void register_entity_component_schemas(flecs::world &p_world, EntitySchemaRegistry &r_registry) {']
    for record in records:
        implementation += [f'\tr_registry.add(entity_make_component<{record["name"]}>(p_world));']
    implementation += ['}', '']
    return '\n'.join(header), '\n'.join(implementation)


def generate(target, source, env):
    root = Path(env.Dir("#").abspath)
    scanner = Path(str(target[2]))
    scanner.write_text('#include "scene/entity/entity_components.h"\n', encoding="utf-8")
    source_text = Path(str(source[0])).read_text(encoding="utf-8")
    names = DECLARATION.findall(source_text)
    if not names:
        raise ValueError("No scene entity declarations in scanner input")
    prefix = os.path.commonprefix(names)
    command = [env["ENTITY_CLANG"], *env["ENTITY_CLANG_FLAGS"], "-DENTITY_SCHEMA_SCAN", "-Xclang", "-ast-dump=json"]
    if prefix:
        command += ["-Xclang", "-ast-dump-filter=" + prefix]
    command += [str(scanner)]
    result = subprocess.run(command, cwd=root, env={str(k): str(v) for k, v in env["ENV"].items()}, capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Entity schema Clang scan failed:\n{result.stderr}")
    records = parse_records(result.stdout, source_text)
    header, implementation = emit(records)
    for path, content in zip(target[:2], (header, implementation)):
        Path(str(path)).write_text(content, encoding="utf-8")


def configure(env):
    compiler = env.WhereIs("clang-cl" if env.msvc else "clang++")
    if not compiler:
        raise RuntimeError("Native entity schemas require host clang-cl/clang++ on PATH")
    flags = [str(flag) for flag in env.subst_list("$_CPPDEFFLAGS")[0]]
    include_prefix = "/I" if env.msvc else "-I"
    flags += [include_prefix + env.Dir(path).abspath for path in env["CPPPATH"]]
    if env.msvc:
        flags += ["/std:c++17", "/Zs", "/EHs-c-", "/Zc:__cplusplus"]
    else:
        flags += ["-std=c++17", "-fsyntax-only", "-fno-exceptions"]
    env["ENTITY_CLANG"] = compiler
    env["ENTITY_CLANG_FLAGS"] = flags
    version = subprocess.run([compiler, "--version"], capture_output=True, text=True, check=True).stdout
    return json.dumps({"compiler": compiler, "version": version, "flags": flags, "include": str(env["ENV"].get("INCLUDE", ""))}, sort_keys=True)
