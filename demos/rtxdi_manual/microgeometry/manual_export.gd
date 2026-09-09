extends SceneTree


class MicroGeometryExportInspection:
	extends EditorExportPlugin

	var customized := 0

	func _get_name() -> String:
		return "MicroGeometryManualExportInspection"

	func _begin_customize_resources(_platform: EditorExportPlatform, _features: PackedStringArray) -> bool:
		return true

	func _get_customization_configuration_hash() -> int:
		return 2026090902

	func _customize_resource(resource: Resource, path: String) -> Resource:
		if resource is MicroGeometry:
			customized += 1
			print("MICROGEOMETRY_EXPORT_CUSTOMIZE path=", path, " content_id=", resource.get_content_id(), " statistics=", JSON.stringify(resource.get_statistics()))
			return resource
		return null

	func _end_customize_resources() -> void:
		print("MICROGEOMETRY_EXPORT_CUSTOMIZED count=", customized)


class ExportRegistrar:
	extends EditorPlugin


var registrar: EditorPlugin
var inspection: EditorExportPlugin


func _initialize() -> void:
	registrar = ExportRegistrar.new()
	inspection = MicroGeometryExportInspection.new()
	get_root().add_child(registrar)
	registrar.add_export_plugin(inspection)
