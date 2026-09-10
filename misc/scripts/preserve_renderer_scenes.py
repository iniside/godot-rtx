import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "demos/rtxdi_manual"
OUTPUT = PROJECT / "entity_migration"
SCENES = (
    "main.tscn", "test.tscn", "shadows_merged.tscn", "shadows_expanded.tscn",
    "energy_directional.tscn", "energy_emission.tscn", "unsupported.tscn",
    "microgeometry/scene.tscn", "microgeometry_stress/scene.tscn",
)
WRAPPERS = tuple("geometry_parts/" + name + ".tscn" for name in ("cube", "sphere", "plane", "deformer"))
GLTFS = (
    "geometry.gltf", "migrated/cube.glb", "migrated/zdm2.glb",
    "microgeometry/xyzrgb_dragon.glb", "microgeometry_stress/lucy.glb",
    "microgeometry_stress/thai_statuette.glb",
)
SCRIPTS = ("main.gd", "migrated/test.gd", "migrated/camera.gd", "microgeometry/scene.gd", "microgeometry_stress/scene.gd")
CONFIGS = ("project.godot", "override.cfg", "microgeometry/export_presets.cfg.example", "export_presets.cfg")
NODE_TYPES = {"Node3D", "WorldEnvironment", "MeshInstance3D", "MultiMeshInstance3D", "Camera3D", "DirectionalLight3D", "OmniLight3D", "SpotLight3D", "AreaLight3D", "CanvasLayer", "Label", "SubViewportContainer", "SubViewport"}
IDENTITY = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def records(text):
    buffer = []
    stack = []
    quoted = escaped = False
    for line in text.splitlines():
        if not buffer and (not line.strip() or line.lstrip().startswith(";")):
            continue
        buffer.append(line)
        for char in line:
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char in "([{":
                stack.append(char)
            elif char in ")]}":
                if not stack or {"(": ")", "[": "]", "{": "}"}[stack.pop()] != char:
                    raise ValueError("Unbalanced serialized value")
        if not quoted and not stack:
            yield "\n".join(buffer)
            buffer = []
    if buffer:
        raise ValueError("Unterminated serialized value")


def parse_scene(name):
    sections = []
    for record in records((PROJECT / name).read_text(encoding="utf-8")):
        if record.startswith("["):
            kind, _, remainder = record[1:-1].partition(" ")
            attributes = {}
            while remainder:
                match = re.match(r'(\w+)=("(?:[^"\\]|\\.)*"|ExtResource\("[^"\n]+"\)|\d+)(?: |$)', remainder)
                if not match:
                    raise ValueError(f"Unsupported section header in {name}: {remainder}")
                key, value = match.group(1, 2)
                attributes[key] = json.loads(value) if value.startswith('"') else value
                remainder = remainder[match.end():]
            if kind not in {"gd_scene", "gd_resource", "ext_resource", "sub_resource", "resource", "node", "editable"}:
                raise ValueError(f"Unsupported section {kind} in {name}")
            sections.append({"section": kind, "attributes": attributes, "properties": {}})
        else:
            key, separator, value = record.partition(" = ")
            if not separator or not sections or key in sections[-1]["properties"]:
                raise ValueError(f"Unsupported property in {name}: {record[:100]}")
            sections[-1]["properties"][key] = value
    return sections


def numbers(value):
    return [float(v.strip()) for v in value[value.index("(") + 1:-1].split(",")]


def compose(a, b):
    result = [sum(a[k * 3 + row] * b[col * 3 + k] for k in range(3)) for col in range(3) for row in range(3)]
    result += [a[9 + row] + sum(a[k * 3 + row] * b[9 + k] for k in range(3)) for row in range(3)]
    return result


def rotation(axis, angle):
    c, s = math.cos(angle), math.sin(angle)
    matrices = ([1, 0, 0, 0, c, s, 0, -s, c], [c, 0, -s, 0, 1, 0, s, 0, c], [c, s, 0, -s, c, 0, 0, 0, 1])
    return list(matrices[axis]) + [0, 0, 0]


def local_transform(properties):
    if "transform" in properties:
        if any(key in properties for key in ("position", "rotation", "rotation_degrees", "scale")):
            raise ValueError("Mixed transform representations require explicit recipe")
        return numbers(properties["transform"])
    result = IDENTITY.copy()
    if "rotation_degrees" in properties:
        angles = [math.radians(v) for v in numbers(properties["rotation_degrees"])]
        result = compose(compose(rotation(1, angles[1]), rotation(0, angles[0])), rotation(2, angles[2]))
    elif "rotation" in properties:
        raise ValueError("Unexpected Euler representation")
    if "scale" in properties:
        scale = numbers(properties["scale"])
        result[:9] = [v * scale[i // 3] for i, v in enumerate(result[:9])]
    if "position" in properties:
        result[9:] = numbers(properties["position"])
    return result


def read_gltf(name):
    path = PROJECT / name
    import_options = dict(line.split("=", 1) for line in Path(str(path) + ".import").read_text(encoding="utf-8").splitlines() if "=" in line)
    for key, expected in {
        "nodes/root_type": '""', "nodes/root_name": '""', "nodes/root_script": "null",
        "nodes/apply_root_scale": "true", "nodes/root_scale": "1.0",
        "meshes/light_baking": "1", "import_script/path": '""', "_subresources": "{}",
    }.items():
        if import_options.get(key) != expected:
            raise ValueError(f"Unplanned import option {name}: {key}={import_options.get(key)}")
    if path.suffix == ".glb":
        with path.open("rb") as stream:
            magic, version, size = struct.unpack("<III", stream.read(12))
            length, kind = struct.unpack("<II", stream.read(8))
            if magic != 0x46546C67 or version != 2 or size != path.stat().st_size or kind != 0x4E4F534A:
                raise ValueError(f"Unsupported GLB {name}")
            data = json.loads(stream.read(length))
    else:
        data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("extensionsRequired") or data.get("skins") or data.get("animations"):
        raise ValueError(f"Unplanned imported feature in {name}")
    roots = data["scenes"][data.get("scene", 0)]["nodes"]
    if roots != list(range(len(data["nodes"]))):
        raise ValueError(f"Unplanned glTF hierarchy in {name}")
    for node in data["nodes"]:
        if set(node) - {"name", "mesh", "rotation"}:
            raise ValueError(f"Unplanned glTF node in {name}: {node}")
    return data


def qualify(properties, source, external):
    result = {}
    for key, value in properties.items():
        if key == "script":
            continue
        value = re.sub(r'ExtResource\("([^"\n]+)"\)', lambda m: 'ExtResource("' + external[m[1]]["path"] + '")', value)
        value = re.sub(r'SubResource\("([^"\n]+)"\)', lambda m: 'SubResource("res://' + source + "::" + m[1] + '")', value)
        result[key] = value
    return result


def imported_nodes(name, gltfs):
    result = {".": {"type": "Node3D", "properties": {}, "source": name}}
    for index, item in enumerate(gltfs[name]["nodes"]):
        transform = IDENTITY.copy()
        if "rotation" in item:
            x, y, z, w = item["rotation"]
            norm = x * x + y * y + z * z + w * w
            s = 2.0 / norm
            transform[:9] = [1 - s * (y*y + z*z), s * (x*y + z*w), s * (x*z - y*w), s * (x*y - z*w), 1 - s * (x*x + z*z), s * (y*z + x*w), s * (x*z + y*w), s * (y*z - x*w), 1 - s * (x*x + y*y)]
        result[item["name"]] = {
            "type": "MeshInstance3D", "properties": {"gi_mode": "1"}, "source": name,
            "imported_local_transform": transform,
            "mesh_asset": {"path": "res://" + name, "gltf_mesh": item["mesh"], "gltf_node": index},
        }
    return result


def resolve(name, parsed, gltfs):
    external = {s["attributes"]["id"]: s["attributes"] for s in parsed[name] if s["section"] == "ext_resource"}
    nodes = {}
    for section in parsed[name]:
        if section["section"] != "node":
            continue
        attr = section["attributes"]
        parent = attr.get("parent")
        path = "." if parent is None else attr["name"] if parent == "." else parent + "/" + attr["name"]
        if "instance" in attr:
            asset = external[re.fullmatch(r'ExtResource\("([^"\n]+)"\)', attr["instance"])[1]]["path"].removeprefix("res://")
            if asset in WRAPPERS:
                children = resolve(asset, parsed, gltfs)
            elif asset in GLTFS:
                children = imported_nodes(asset, gltfs)
            else:
                raise ValueError(f"Unplanned instance {asset}")
            for child, record in children.items():
                destination = path if child == "." else child if path == "." else path + "/" + child
                if destination in nodes:
                    raise ValueError(f"Duplicate instance path {destination}")
                nodes[destination] = copy.deepcopy(record)
        elif "type" in attr:
            if attr["type"] not in NODE_TYPES or path in nodes:
                raise ValueError(f"Unplanned node in {name}: {attr}")
            nodes[path] = {"type": attr["type"], "properties": {}, "source": name}
        elif path not in nodes:
            raise ValueError(f"Unresolved inherited override {name}:{path}")
        nodes[path]["properties"].update(qualify(section["properties"], name, external))
        if "unique_id" in attr:
            nodes[path]["source_unique_id"] = attr["unique_id"]
    return nodes


def effective_nodes(nodes):
    transforms, visibility = {}, {}
    for path, record in nodes.items():
        parent = path.rsplit("/", 1)[0] if "/" in path else "."
        properties = record["properties"]
        local = record.get("imported_local_transform", local_transform(properties))
        if "transform" in properties:
            local = local_transform(properties)
        world = local if path == "." else compose(transforms[parent], local)
        transforms[path] = world
        visible = properties.get("visible", "true") != "false"
        visibility[path] = visible if path == "." else visible and visibility[parent]
        if path == "HUD" or path.startswith("HUD/") or path == "SecondaryView" or path.startswith("SecondaryView/"):
            continue
        if record["type"] == "Node3D":
            continue
        if record["type"] not in {"WorldEnvironment", "MeshInstance3D", "MultiMeshInstance3D", "Camera3D", "DirectionalLight3D", "OmniLight3D", "SpotLight3D", "AreaLight3D"}:
            raise ValueError(f"Unplanned world node {path}")
        yield {"path": path, **record, "local_transform": local, "world_transform": world, "effective_visible": visibility[path]}


def defaults(types):
    result = {}
    pending = set(types) | {"Viewport"}
    while pending:
        name = pending.pop()
        path = ROOT / "doc/classes" / (name + ".xml")
        document = ET.parse(path).getroot()
        parent = document.get("inherits")
        members = {m.get("name"): dict(m.attrib) for m in document.findall("./members/member")}
        constants = {c.get("name"): dict(c.attrib) for c in document.findall("./constants/constant")}
        result[name] = {"source": path.relative_to(ROOT).as_posix(), "sha256": sha(path), "inherits": parent, "members": members, "constants": constants}
        if parent and parent not in result:
            pending.add(parent)
    return dict(sorted(result.items()))


def main():
    parsed = {name: parse_scene(name) for name in SCENES + WRAPPERS + ("geometry_parts/plane_mesh.tres",)}
    gltfs = {name: read_gltf(name) for name in GLTFS}
    sources = set(parsed) | set(GLTFS) | set(SCRIPTS)
    types = set()
    for sections in parsed.values():
        for section in sections:
            attr = section["attributes"]
            if section["section"] == "ext_resource":
                sources.add(attr["path"].removeprefix("res://"))
            if section["section"] in {"node", "sub_resource", "gd_resource"} and "type" in attr:
                types.add(attr["type"])
    sources.update({"microgeometry/seam_grid.mtl"})
    sources.update(name for name in CONFIGS if (PROJECT / name).exists())
    sources.update(name + ".import" for name in list(sources) if (PROJECT / (name + ".import")).exists())
    sources.update(name + ".uid" for name in SCRIPTS if (PROJECT / (name + ".uid")).exists())
    tracked = set(subprocess.check_output(["git", "ls-files", "--", "demos/rtxdi_manual"], cwd=ROOT, text=True).splitlines())
    assets = []
    for name in sorted(sources):
        path = PROJECT / name
        item = {"path": "res://" + name, "bytes": path.stat().st_size, "sha256": sha(path), "tracked": "demos/rtxdi_manual/" + name in tracked}
        if name in SCRIPTS:
            item["disposition"] = "recipe_source_only_do_not_execute_or_port"
        elif path.suffix in {".tscn", ".tres"}:
            item["disposition"] = "parsed_records_retained"
        elif path.suffix in {".cfg", ".godot", ".import", ".uid", ".example"}:
            item["text"] = path.read_text(encoding="utf-8")
        else:
            item["disposition"] = "retain_shared_source_asset_at_original_path"
        assets.append(item)
    write_json(OUTPUT / "sources.json", assets)
    write_json(OUTPUT / "class_defaults.json", defaults(types | {"MeshInstance3D"}))
    write_json(OUTPUT / "imported_assets.json", gltfs)
    for name in WRAPPERS + ("geometry_parts/plane_mesh.tres",):
        write_json(OUTPUT / "serialized" / (name + ".json"), parsed[name])
    scenes = []
    for name in SCENES:
        write_json(OUTPUT / "serialized" / (name + ".json"), parsed[name])
        resolved = list(effective_nodes(resolve(name, parsed, gltfs)))
        output = OUTPUT / "resolved" / (name + ".jsonl")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w", encoding="utf-8", newline="\n") as stream:
            for record in resolved:
                stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
        counts = {}
        for record in resolved:
            counts[record["type"]] = counts.get(record["type"], 0) + 1
        scenes.append({"source": "res://" + name, "target": "res://" + name.removesuffix(".tscn") + ".escn", "serialized": "serialized/" + name + ".json", "resolved": "resolved/" + name + ".jsonl", "counts": counts, "visible_meshes": sum(r["type"] == "MeshInstance3D" and r["effective_visible"] for r in resolved)})
    write_json(OUTPUT / "manifest.json", {
        "step": 1,
        "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "working_tree_inputs": True,
        "recipes": "native_recipes.json",
        "source_identity_and_import_config": "sources.json",
        "imported_asset_metadata": "imported_assets.json",
        "documented_defaults": "class_defaults.json",
        "interpretation": {
            "serialized": "Lossless property-value text and ordered sections, including authored hierarchy and resource identity. JSON strings contain Godot Variant text, not evaluated Python.",
            "resolved": "One JSONL record per renderer-relevant leaf, with scoped resource references and composed authored transform/visibility. Hidden imported siblings are retained. HUD and secondary viewport are omitted here, retained only in source provenance.",
            "transforms": "12 binary64 numbers: basis columns x/y/z then origin. Source decimal text remains in serialized records. glTF quaternion normalized before conversion; degree rotations use Godot YXZ order. These are authored poses before native startup recipes.",
            "defaults": "Merge inherited class members base-first, then derived documented defaults, imported data and explicit properties. Null resource defaults may be implicit. The full source XML SHA and metadata are pinned; this is not a ClassDB runtime dump.",
            "assets": "Keep source files at their original paths and verify SHA256 before conversion. GLB JSON metadata does not replace its binary buffers. Untracked source assets are explicitly marked and must remain available.",
            "conversion": "Step4 consumes these fixed inputs using its native component schema and EntityScene IO. This step introduces no .escn format or compatibility loader. Step8 completes target scene/import/export authoring; delete the temporary tool after conversion, with final cleanup in step9.",
            "regeneration": "From repo root: python misc/scripts/preserve_renderer_scenes.py. This overwrites only extraction artifacts in entity_migration and reads the fixed allowlist; native_recipes.json and validation.json are authored separately. Reinspect owner input changes before recapturing.",
            "unknown_data": "Unplanned section/node/instance/glTF topology and unresolved overrides fail extraction. All property values are retained literally; component-field mapping and any unsupported property errors belong to step4 conversion.",
        },
        "scenes": scenes,
    })
    print(json.dumps(scenes, indent=2))


if __name__ == "__main__":
    main()
