#ifndef RTXDI_RESOLUTION_INC_SLANG
#define RTXDI_RESOLUTION_INC_SLANG

uint2 rtxdi_surface_pixel(uint2 pixel, uint2 lighting_size, uint2 frame_size) {
	if (all(lighting_size == frame_size)) {
		return pixel;
	}
	return min(((pixel * 2u + 1u) * frame_size) / (lighting_size * 2u), frame_size - 1u);
}

float2 rtxdi_lighting_position(float2 surface_position, uint2 lighting_size, uint2 frame_size) {
	if (all(lighting_size == frame_size)) {
		return surface_position;
	}
	int2 cell = clamp(int2(floor(surface_position * float2(lighting_size) / float2(frame_size))), int2(0), int2(lighting_size) - 1);
	float2 center = float2(rtxdi_surface_pixel(uint2(cell), lighting_size, frame_size)) + 0.5;
	int2 lower = clamp(cell - int2(surface_position < center), int2(0), max(int2(lighting_size) - 2, int2(0)));
	int2 upper = min(lower + 1, int2(lighting_size) - 1);
	float2 lower_center = float2(rtxdi_surface_pixel(uint2(lower), lighting_size, frame_size)) + 0.5;
	float2 upper_center = float2(rtxdi_surface_pixel(uint2(upper), lighting_size, frame_size)) + 0.5;
	return float2(lower) + 0.5 + (surface_position - lower_center) / max(upper_center - lower_center, float2(1.0));
}

#endif
