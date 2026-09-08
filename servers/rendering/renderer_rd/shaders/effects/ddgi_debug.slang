#[vertex]

#VERSION_DEFINES
#include "ddgi_debug_inc.slang"

[shader("vertex")]
DebugVertex main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
	DebugVertex output = (DebugVertex)0;
	const float2 corners[6] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
	float2 corner = corners[vertex_id % 6u];
	if (params.mode == 2u || params.mode == 3u) {
		output.position = float4(corner, 0.0, 1.0);
		return output;
	}
	DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(ddgi_volumes[params.cascade]);
	output.color = ddgi_debug_color(params.cascade);
	if (params.mode == 4u) {
		uint axis = instance_id / 4u;
		uint side = instance_id % 4u;
		float3 center = volume.origin + volume.probeScrollOffsets * volume.probeSpacing;
		float3 extent = (volume.probeCounts - 1) * volume.probeSpacing * 0.5;
		float3 a = center;
		a[axis] -= extent[axis];
		a[(axis + 1u) % 3u] += ((side & 1u) != 0u ? 1.0 : -1.0) * extent[(axis + 1u) % 3u];
		a[(axis + 2u) % 3u] += ((side & 2u) != 0u ? 1.0 : -1.0) * extent[(axis + 2u) % 3u];
		float3 b = a;
		b[axis] += extent[axis] * 2.0;
		float4 clip_a = ddgi_debug_project(a);
		float4 clip_b = ddgi_debug_project(b);
		if (!ddgi_debug_clip(clip_a, clip_b, clip_a.w - 0.001, clip_b.w - 0.001) ||
				!ddgi_debug_clip(clip_a, clip_b, clip_a.w - clip_a.z, clip_b.w - clip_b.z) ||
				!ddgi_debug_clip(clip_a, clip_b, clip_a.z, clip_b.z)) {
			output.position = float4(2.0, 2.0, 0.0, 1.0);
			return output;
		}
		float2 direction = (clip_b.xy / clip_b.w - clip_a.xy / clip_a.w) * float2(params.output_size);
		float2 perpendicular = float2(-direction.y, direction.x) / max(length(direction), 0.001);
		output.position = corner.x < 0.0 ? clip_a : clip_b;
		output.position.xy += perpendicular * corner.y * 2.0 / float2(params.output_size) * output.position.w;
		return output;
	}
	uint3 coordinate = DDGIGetProbeTexelCoords(instance_id, volume);
	float3 position = ddgi_probe_position(instance_id, volume, ddgi_probe_data[params.cascade]);
	output.position = ddgi_debug_project(position);
	if (output.position.w <= 0.0 || output.position.z < 0.0 || output.position.z > output.position.w) {
		output.position = float4(2.0, 2.0, 0.0, 1.0);
		return output;
	}
	output.position.xy += corner * 7.0 / float2(params.output_size) * output.position.w;
	output.disc = corner;
	if (params.mode == 1u) {
		uint stamp = probe_update_frame.Load(int4(coordinate, 0));
		bool recycled = (params.reset_cascades & (1u << params.cascade)) != 0u && probe_reset_mask[instance_id] != 0u;
		bool inactive = ddgi_probe_data[params.cascade].Load(int4(coordinate, 0)).w == RTXGI_DDGI_PROBE_STATE_INACTIVE;
		bool valid = ddgi_validity[params.cascade].Load(int4(coordinate, 0)) != 0u;
		if (recycled) {
			output.color = float3(1.0, 0.0, 1.0);
		} else if (inactive) {
			output.color = 0.35;
		} else if (!valid || stamp == 0u) {
			output.color = float3(1.0, 0.0, 0.0);
		} else {
			float age = saturate(float(ddgi_frame.z - stamp) / 120.0);
			output.color = lerp(float3(0.0, 1.0, 0.2), float3(0.1, 0.2, 1.0), age);
		}
	}
	return output;
}

#[fragment]

#VERSION_DEFINES
#include "ddgi_debug_inc.slang"

[shader("fragment")]
float4 main(DebugVertex input) : SV_Target0 {
	float2 uv = input.position.xy / float2(params.output_size);
	float2 source_uv = uv + scene.data.taa_jitter * 0.5;
	uint2 pixel = uint2(clamp(source_uv * float2(params.internal_size), 0.0, float2(params.internal_size - 1u)));
	if (params.mode == 0u || params.mode == 1u || params.mode == 4u) {
		if (params.mode != 4u && dot(input.disc, input.disc) > 1.0) {
			discard;
		}
		if (input.position.z + 1e-6 < surface_depth.Load(int3(pixel, 0))) {
			discard;
		}
		return float4(input.color, 1.0);
	}
	if (params.mode == 3u) {
		float3 radiance = max(camera_indirect.Load(int3(pixel, 0)).rgb, 0.0);
		float3 mapped = radiance / (1.0 + radiance);
		float3 encoded = select(mapped <= 0.0031308, mapped * 12.92, 1.055 * pow(mapped, 1.0 / 2.4) - 0.055);
		return float4(encoded, 1.0);
	}
	bool legend = uv.y > 0.88;
	if (legend) {
		float2 center_uv = 0.5 + scene.data.taa_jitter * 0.5;
		pixel = uint2(clamp(center_uv * float2(params.internal_size), 0.0, float2(params.internal_size - 1u)));
	}
	SurfaceData surface;
	float3 position, view;
	if (!ddgi_camera_surface(pixel, params.internal_size, params.orthogonal != 0u, surface, position, view)) {
		return float4(0.0, 0.0, 0.0, 1.0);
	}
	DDGIQueryWeights weights;
	ddgi_query(position, surface.normal, -view, weights);
	if (legend) {
		uint column = min(uint(uv.x * 7.0), 6u);
		float value = column == 6u ? weights.uncovered : (uv.y < 0.94 ? weights.contribution[column] : weights.confidence[column]);
		float3 color = column == 6u ? float3(1.0, 0.0, 1.0) : ddgi_debug_color(column);
		float fill = frac(uv.x * 7.0);
		return float4(fill <= value ? color : color * 0.08, 1.0);
	}
	float3 color = float3(1.0, 0.0, 1.0) * weights.uncovered;
	for (uint cascade = 0u; cascade < ddgi_frame.y; cascade++) {
		color += ddgi_debug_color(cascade) * weights.contribution[cascade];
	}
	if (all(abs(input.position.xy - float2(params.output_size) * 0.5) < 2.0)) {
		color = 1.0;
	}
	return float4(color, 1.0);
}
