#[compute]

#VERSION_DEFINES

#include "ddgi_sdk_inc.slang"

#ifdef MODE_IRRADIANCE
#define RTXGI_DDGI_BLEND_RADIANCE 1
#define RTXGI_DDGI_PROBE_NUM_TEXELS 8
#define RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS 6
#define OUTPUT_REGISTER 2
#else
#define RTXGI_DDGI_BLEND_RADIANCE 0
#define RTXGI_DDGI_PROBE_NUM_TEXELS 16
#define RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS 14
#define OUTPUT_REGISTER 3
#endif

#define DDGIProbeBlendingCS main
#include "rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl"
