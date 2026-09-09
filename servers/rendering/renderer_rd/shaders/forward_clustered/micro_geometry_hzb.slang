#[compute]
#VERSION_DEFINES

[[vk::binding(0, 0)]] Texture2D<float> source_depth;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> result_depth;
struct Parameters {
	uint2 source_size;
	uint copy_depth;
	uint pad;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	uint width, height;
	result_depth.GetDimensions(width, height);
	if (any(id.xy >= uint2(width, height))) {
		return;
	}
	float depth = 0;
	if (parameters.copy_depth != 0) {
		if (all(id.xy < parameters.source_size)) {
			depth = source_depth.Load(int3(id.xy, 0));
		}
	} else {
		uint2 base = id.xy * 2;
		depth = 1;
		for (uint y = 0; y < 2; y++) {
			for (uint x = 0; x < 2; x++) {
				uint2 pixel = min(base + uint2(x, y), parameters.source_size - 1);
				depth = min(depth, source_depth.Load(int3(pixel, 0)));
			}
		}
	}
	result_depth[id.xy] = depth;
}
