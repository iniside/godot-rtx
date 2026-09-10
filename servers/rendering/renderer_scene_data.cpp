#include "renderer_scene_data.h"

#include "rendering_server.h"

ToolRenderData ToolRenderData::create(RID p_base, RID p_scenario, const Ref<Resource> &p_asset) {
	ToolRenderData result;
	result.handle = RenderingServer::get_singleton()->tool_render_create();
	result.base = p_base;
	result.scenario = p_scenario;
	result.base_asset = p_asset;
	result.publish();
	return result;
}

void ToolRenderData::publish() const {
	ERR_FAIL_COND(handle.is_null());
	RenderingServer::get_singleton()->tool_render_update(*this);
}

void ToolRenderData::clear() {
	if (handle.is_valid()) {
		RenderingServer::get_singleton()->free_rid(handle);
		handle = RID();
	}
	assets.clear();
	base_asset.unref();
	material_asset.unref();
}

Color render_light_color_from_temperature(float p_temperature) {
	float T2 = p_temperature * p_temperature;
	float u = (0.860117757f + 1.54118254e-4f * p_temperature + 1.28641212e-7f * T2) /
			(1.0f + 8.42420235e-4f * p_temperature + 7.08145163e-7f * T2);
	float v = (0.317398726f + 4.22806245e-5f * p_temperature + 4.20481691e-8f * T2) /
			(1.0f - 2.89741816e-5f * p_temperature + 1.61456053e-7f * T2);

	float d = 1.0f / (2.0f * u - 8.0f * v + 4.0f);
	float x = 3.0f * u * d;
	float y = 2.0f * v * d;

	const float a = 1.0 / MAX(y, 1e-5f);
	Vector3 xyz = Vector3(x * a, 1.0, (1.0f - x - y) * a);

	Vector3 linear = Vector3(3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
			-0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
			0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);
	linear /= MAX(1e-5f, linear[linear.max_axis_index()]);
	return Color(linear.x, linear.y, linear.z).clamp().linear_to_srgb();
}
