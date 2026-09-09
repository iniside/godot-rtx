extends SceneTree


func _initialize() -> void:
	call_deferred("_inspect")


func _inspect() -> void:
	var packed: PackedScene = load("res://xyzrgb_dragon.glb")
	if packed == null:
		print("DRAGON_RESOURCE null")
		quit()
		return
	print("DRAGON_DEPENDENCIES ", JSON.stringify(ResourceLoader.get_dependencies("res://xyzrgb_dragon.glb")))
	var instance := packed.instantiate()
	var nodes: Array[Node] = [instance]
	var mesh_count := 0
	while not nodes.is_empty():
		var node: Node = nodes.pop_back()
		nodes.append_array(node.get_children())
		if node is MeshInstance3D:
			mesh_count += 1
			var mesh: ArrayMesh = node.mesh
			var geometry: MicroGeometry = mesh.micro_geometry
			print("DRAGON_MESH ", node.name, " surfaces=", mesh.get_surface_count())
			if geometry != null:
				print("DRAGON_GEOMETRY ", geometry.resource_path)
				print("DRAGON_CONTENT_ID ", geometry.get_content_id())
				print("DRAGON_STATISTICS ", JSON.stringify(geometry.get_statistics()))
	print("DRAGON_MESH_COUNT ", mesh_count)
	instance.free()
	quit()
