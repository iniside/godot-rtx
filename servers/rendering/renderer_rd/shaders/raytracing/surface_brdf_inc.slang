#ifndef SURFACE_BRDF_INC_SLANG
#define SURFACE_BRDF_INC_SLANG

/* Creative Commons CC0 Public Domain. To the extent possible under law,
Jakub Boksansky has waived all copyright and related or neighboring rights
to Crash Course in BRDF Implementation Code Sample.
This work is published from: Germany. */

#include "../surface_data_inc.slang"

static const float PI = 3.141592653589f;
static const float TWO_PI = 2.0f * PI;
static const float ONE_OVER_PI = 1.0f / PI;
static const float MIN_DIELECTRICS_F0 = 0.04f;
static const int DIFFUSE_TYPE = 1;
static const int SPECULAR_TYPE = 2;

struct MaterialProperties {
	float3 baseColor;
	float metalness;

	float3 emissive;
	float roughness;
	float dielectricF0;
	float transmissivness;
	float opacity;
};

struct BrdfData {

	float3 specularF0;
	float3 diffuseReflectance;

	float roughness;
	float alpha;
	float alphaSquared;

	float3 F;

	float3 V;
	float3 N;
	float3 H;
	float3 L;

	float NdotL;
	float NdotV;

	float LdotH;
	float NdotH;
	float VdotH;

	bool Vbackfacing;
	bool Lbackfacing;
};

float luminance(float3 rgb) {
	return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 baseColorToSpecularF0(float3 baseColor, float metalness, float dielectricF0) {
	return lerp(float3(dielectricF0, dielectricF0, dielectricF0), baseColor, metalness);
}

float3 baseColorToDiffuseReflectance(float3 baseColor, float metalness) {
	return baseColor * (1.0f - metalness);
}

float4 getRotationToZAxis(float3 v) {

	if (v.z < -0.99999f) {
		return float4(1.0f, 0.0f, 0.0f, 0.0f);
	}

	return normalize(float4(v.y, -v.x, 0.0f, 1.0f + v.z));
}

float4 invertRotation(float4 q) {
	return float4(-q.x, -q.y, -q.z, q.w);
}

float3 rotatePoint(float4 q, float3 v) {
	const float3 qAxis = float3(q.x, q.y, q.z);
	return 2.0f * dot(qAxis, v) * qAxis + (q.w * q.w - dot(qAxis, qAxis)) * v + 2.0f * q.w * cross(qAxis, v);
}

float3 sampleHemisphere(float2 u, out float pdf) {
	float a = sqrt(u.x);
	float b = TWO_PI * u.y;

	float3 result = float3(
			a * cos(b),
			a * sin(b),
			sqrt(1.0f - u.x));

	pdf = result.z * ONE_OVER_PI;

	return result;
}

float3 sampleHemisphere(float2 u) {
	float pdf;
	return sampleHemisphere(u, pdf);
}

float diffusePdf(float NdotL) {
	return NdotL * ONE_OVER_PI;
}

float3 evalFresnelSchlick(float3 f0, float f90, float NdotS) {
	return f0 + (f90 - f0) * pow(1.0f - NdotS, 5.0f);
}

float3 evalFresnel(float3 f0, float f90, float NdotS) {

	return evalFresnelSchlick(f0, f90, NdotS);
}

float shadowedF90(float3 F0) {

	const float t = (1.0f / MIN_DIELECTRICS_F0);
	return min(1.0f, t * luminance(F0));
}

float lambertian(const BrdfData data) {
	return 1.0f;
}

float3 evalLambertian(const BrdfData data) {
	return data.diffuseReflectance * (ONE_OVER_PI * data.NdotL);
}

float Smith_G1_GGX(float a) {
	float a2 = a * a;
	return 2.0f / (sqrt((a2 + 1.0f) / a2) + 1.0f);
}

float Smith_G1_GGX(float alpha, float NdotS, float alphaSquared, float NdotSSquared) {
	return 2.0f / (sqrt(((alphaSquared * (1.0f - NdotSSquared)) + NdotSSquared) / NdotSSquared) + 1.0f);
}

float Smith_G2_Height_Correlated_GGX_Lagarde(float alphaSquared, float NdotL, float NdotV) {
	float a = NdotV * sqrt(alphaSquared + NdotL * (NdotL - alphaSquared * NdotL));
	float b = NdotL * sqrt(alphaSquared + NdotV * (NdotV - alphaSquared * NdotV));
	return 0.5f / (a + b);
}

float Smith_G2_Over_G1_Height_Correlated(float alpha, float alphaSquared, float NdotL, float NdotV) {
	float G1V = Smith_G1_GGX(alpha, NdotV, alphaSquared, NdotV * NdotV);
	float G1L = Smith_G1_GGX(alpha, NdotL, alphaSquared, NdotL * NdotL);
	return G1L / (G1V + G1L - G1V * G1L);
}

float Smith_G2(float alpha, float alphaSquared, float NdotL, float NdotV) {
	return Smith_G2_Height_Correlated_GGX_Lagarde(alphaSquared, NdotL, NdotV);
}

float GGX_D(float alphaSquared, float NdotH) {
	float b = ((alphaSquared - 1.0f) * NdotH * NdotH + 1.0f);
	return alphaSquared / (PI * b * b);
}

float3 sampleGGXVNDF(float3 Ve, float2 alpha2D, float2 u) {

	float3 Vh = normalize(float3(alpha2D.x * Ve.x, alpha2D.y * Ve.y, Ve.z));

	float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	float3 T1 = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
	float3 T2 = cross(Vh, T1);

	float r = sqrt(u.x);
	float phi = TWO_PI * u.y;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5f * (1.0f + Vh.z);
	t2 = lerp(sqrt(1.0f - t1 * t1), t2, s);

	float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

	return normalize(float3(alpha2D.x * Nh.x, alpha2D.y * Nh.y, max(0.0f, Nh.z)));
}

float sampleGGXVNDFReflectionPdf(float alpha, float alphaSquared, float NdotH, float NdotV, float LdotH) {
	NdotH = max(0.00001f, NdotH);
	NdotV = max(0.00001f, NdotV);
	return (GGX_D(max(0.00001f, alphaSquared), NdotH) * Smith_G1_GGX(alpha, NdotV, alphaSquared, NdotV * NdotV)) / (4.0f * NdotV);
}

float specularSampleWeightGGXVNDF(float alpha, float alphaSquared, float NdotL, float NdotV, float HdotL, float NdotH) {
	return Smith_G2_Over_G1_Height_Correlated(alpha, alphaSquared, NdotL, NdotV);
}

float evalEmsApprox(float r2, float NdV) {
	float r4 = r2 * r2;
	float nv0 = 0.2 * r2;
	float nv1 = 0.32 * r2 + 1.94 * r4;
	return lerp(nv0, nv1, NdV);
}

float3 evalMultiScatterApproximation(float alpha, float NdV, float3 F0) {
	float Ems = evalEmsApprox(alpha, NdV);
	return 1.0 + F0 * Ems;
}

float3 sampleSpecularMicrofacet(float3 Vlocal, float alpha, float alphaSquared, float3 specularF0, float2 u, out float3 weight) {

	float3 Hlocal;
	if (alpha == 0.0f) {

		Hlocal = float3(0.0f, 0.0f, 1.0f);
	} else {

		Hlocal = sampleGGXVNDF(Vlocal, float2(alpha, alpha), u);
	}

	float3 Llocal = reflect(-Vlocal, Hlocal);

	float HdotL = max(0.00001f, min(1.0f, dot(Hlocal, Llocal)));
	const float3 Nlocal = float3(0.0f, 0.0f, 1.0f);
	float NdotL = max(0.00001f, min(1.0f, dot(Nlocal, Llocal)));
	float NdotV = max(0.00001f, min(1.0f, dot(Nlocal, Vlocal)));
	float NdotH = max(0.00001f, min(1.0f, dot(Nlocal, Hlocal)));
	float3 F = evalFresnel(specularF0, shadowedF90(specularF0), HdotL);

	float3 ms = evalMultiScatterApproximation(alpha, NdotV, specularF0);

	weight = ms * F * specularSampleWeightGGXVNDF(alpha, alphaSquared, NdotL, NdotV, HdotL, NdotH);

	return Llocal;
}

float3 evalMicrofacet(const BrdfData data) {
	float D = GGX_D(max(0.00001f, data.alphaSquared), data.NdotH);
	float G2 = Smith_G2(data.alpha, data.alphaSquared, data.NdotL, data.NdotV);

	return data.F * (G2 * D * data.NdotL);
}

BrdfData prepareBRDFData(float3 N, float3 L, float3 V, MaterialProperties material) {
	BrdfData data;

	data.V = V;
	data.N = N;
	data.H = normalize(L + V);
	data.L = L;

	float NdotL = dot(N, L);
	float NdotV = dot(N, V);
	data.Vbackfacing = (NdotV <= 0.0f);
	data.Lbackfacing = (NdotL <= 0.0f);

	data.NdotL = min(max(0.00001f, NdotL), 1.0f);
	data.NdotV = min(max(0.00001f, NdotV), 1.0f);

	data.LdotH = saturate(dot(L, data.H));
	data.NdotH = saturate(dot(N, data.H));
	data.VdotH = saturate(dot(V, data.H));

	data.specularF0 = baseColorToSpecularF0(material.baseColor, material.metalness, material.dielectricF0);
	data.diffuseReflectance = baseColorToDiffuseReflectance(material.baseColor, material.metalness);

	data.roughness = material.roughness;
	data.alpha = material.roughness * material.roughness;
	data.alphaSquared = data.alpha * data.alpha;

	data.F = evalFresnel(data.specularF0, shadowedF90(data.specularF0), data.LdotH);

	return data;
}

void evalCombinedBRDFSeparate(float3 N, float3 L, float3 V, MaterialProperties material, out float3 outDiffuse, out float3 outSpecular) {
	outDiffuse = float3(0.0f, 0.0f, 0.0f);
	outSpecular = float3(0.0f, 0.0f, 0.0f);

	const BrdfData data = prepareBRDFData(N, L, V, material);

	if (data.Lbackfacing) {
		return;
	}

	outSpecular = evalMicrofacet(data);
	outDiffuse = evalLambertian(data);

	outDiffuse = (float3(1.0f, 1.0f, 1.0f) - data.F) * outDiffuse;
}

bool evalIndirectCombinedBRDF(float2 u, float3 shadingNormal, float3 geometryNormal, float3 V, MaterialProperties material, const int brdfType, out float3 rayDirection, out float3 sampleWeight) {

	float4 qRotationToZ = getRotationToZAxis(shadingNormal);
	float3 Vlocal = rotatePoint(qRotationToZ, V);
	const float3 Nlocal = float3(0.0f, 0.0f, 1.0f);

	float3 rayDirectionLocal = float3(0.0f, 0.0f, 0.0f);

	if (brdfType == DIFFUSE_TYPE) {

		rayDirectionLocal = sampleHemisphere(u);
		const BrdfData data = prepareBRDFData(Nlocal, rayDirectionLocal, Vlocal, material);

		sampleWeight = data.diffuseReflectance * lambertian(data);

		float3 Hspecular = sampleGGXVNDF(Vlocal, float2(data.alpha, data.alpha), u);

		float VdotH = max(0.00001f, min(1.0f, dot(Vlocal, Hspecular)));
		sampleWeight *= (float3(1.0f, 1.0f, 1.0f) - evalFresnel(data.specularF0, shadowedF90(data.specularF0), VdotH));

	} else if (brdfType == SPECULAR_TYPE) {
		const BrdfData data = prepareBRDFData(Nlocal, float3(0.0f, 0.0f, 1.0f) , Vlocal, material);
		rayDirectionLocal = sampleSpecularMicrofacet(Vlocal, data.alpha, data.alphaSquared, data.specularF0, u, sampleWeight);
	}

	rayDirection = normalize(rotatePoint(invertRotation(qRotationToZ), rayDirectionLocal));

	if (luminance(sampleWeight) == 0.0f) {
		return false;
	}

	if (dot(geometryNormal, rayDirection) <= 0.0f) {
		return false;
	}

	return true;
}

MaterialProperties surface_material_properties(SurfaceMaterial surface_material) {
	MaterialProperties material;
	material.baseColor = surface_material.base_color;
	material.metalness = surface_material.metallic;
	material.emissive = surface_material.emission;
	material.roughness = max(surface_material.roughness, 0.02);
	material.dielectricF0 = 0.08 * surface_material.specular;
	material.transmissivness = 0.0;
	material.opacity = surface_material.opacity;
	return material;
}

void surface_evaluate_brdf(SurfaceData surface, float3 direction, out float3 diffuse, out float3 specular) {
	evalCombinedBRDFSeparate(surface.normal, direction, surface.view_direction, surface_material_properties(surface.material), diffuse, specular);
}

float surface_brdf_pdf(SurfaceData surface, float3 direction) {
	if (dot(surface.geometry_normal, direction) <= 0.0 || dot(surface.normal, direction) <= 0.0 || dot(surface.normal, surface.view_direction) <= 0.0) {
		return 0.0;
	}
	BrdfData data = prepareBRDFData(surface.normal, direction, surface.view_direction, surface_material_properties(surface.material));
	return 0.5 * (diffusePdf(data.NdotL) + sampleGGXVNDFReflectionPdf(data.alpha, data.alphaSquared, data.NdotH, data.NdotV, data.LdotH));
}

bool surface_sample_brdf(SurfaceData surface, float3 random, out float3 direction) {
	int type = random.x < 0.5 ? DIFFUSE_TYPE : SPECULAR_TYPE;
	float3 weight;
	return evalIndirectCombinedBRDF(random.yz, surface.normal, surface.geometry_normal, surface.view_direction, surface_material_properties(surface.material), type, direction, weight);
}

#endif
