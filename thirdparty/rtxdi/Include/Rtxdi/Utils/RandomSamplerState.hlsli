/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RTXDI_RANDOM_SAMPLER_STATE_HLSLI
#define RTXDI_RANDOM_SAMPLER_STATE_HLSLI

#include <Rtxdi/Utils/Math.hlsli>
#include <Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli>

struct RTXDI_RandomSamplerState
{
    uint seed;
    uint index;
};

uint RTXDI_pcg_hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Initialized the random sampler for a given pixel or tile index.
// The pass parameter is provided to help generate different RNG sequences
// for different resampling passes, which is important for image quality.
// In general, a high quality RNG is critical to get good results from ReSTIR.
// A table-based blue noise RNG dose not provide enough entropy, for example.
RTXDI_RandomSamplerState RTXDI_InitRandomSampler(uint2 pixelPos, uint frameIndex, uint pass)
{
    RTXDI_RandomSamplerState state;

    uint linearPixelIndex = RTXDI_ZCurveToLinearIndex(pixelPos);

    state.index = 1;
    state.seed = RTXDI_pcg_hash(RTXDI_JenkinsHash(linearPixelIndex) + frameIndex + (pass * RTXDI_RANDOM_SAMPLER_PRIME_CONSTANT));

    return state;

}

// Draws a random number X from the sampler, so that (0 <= X < 1).
float RTXDI_GetNextRandom(inout RTXDI_RandomSamplerState rng)
{
    uint v = RTXDI_pcg_hash(rng.seed + rng.index++);
    const uint one = asuint(1.f);
    const uint mask = (1 << 23) - 1;
    return asfloat((mask & v) | one) - 1.f;
}

RTXDI_RandomSamplerState RTXDI_CreateRandomSamplerFromDirectSeed(uint seed, uint index)
{
	RTXDI_RandomSamplerState rng;
	rng.seed = seed;
	rng.index = index;
	return rng;
}

float RTXDI_GaussRand(float mean, float stddev, inout RTXDI_RandomSamplerState rng)
{
	// Sampling from the normal distribution (Box-Muller method)
	const float2 UV = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
	float Z = sqrt(-2.0 * log(UV.x)) * cos(2.0 * RTXDI_PI * UV.y);
	Z = Z * stddev * mean + mean; // want the standard deviation to scale with mean to provide a soft threshold

	return Z;
}

#endif // RTXDI_RANDOM_SAMPLER_STATE_HLSLI