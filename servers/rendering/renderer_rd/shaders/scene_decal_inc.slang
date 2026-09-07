void scene_apply_decal(DecalData decal, Texture2D<float4> decal_atlas, Texture2D<float4> decal_atlas_srgb, SamplerState decal_sampler,
		bool use_mipmaps, uint layer_mask, float3 position, float3 geo_normal, float3 position_dx, float3 position_dy,
		inout float3 albedo, inout float3 normal, inout float ao, inout float roughness, inout float metallic, inout float3 emission) {
	if (!bool(decal.mask & layer_mask)) {
		return;
	}

	float3 uv_local = (mul(float4(position, 1.0), decal.xform)).xyz;
	if (any(uv_local < float3(0.0, -1.0, 0.0)) || any(uv_local > float3(1.0))) {
		return;
	}

	float fade = pow(1.0 - (uv_local.y > 0.0 ? uv_local.y : -uv_local.y), uv_local.y > 0.0 ? decal.upper_fade : decal.lower_fade);

	if (decal.normal_fade > 0.0) {
		fade *= smoothstep(decal.normal_fade, 1.0, dot(geo_normal, decal.normal) * 0.5 + 0.5);
	}

	float2 ddx = (mul(float4(position_dx, 0.0), decal.xform)).xz;
	float2 ddy = (mul(float4(position_dy, 0.0), decal.xform)).xz;

	if (any(decal.albedo_rect != float4(0.0))) {

		float4 decal_albedo;
		if (use_mipmaps) {
			decal_albedo = decal_atlas_srgb.SampleGrad(decal_sampler, uv_local.xz * decal.albedo_rect.zw + decal.albedo_rect.xy, ddx * decal.albedo_rect.zw, ddy * decal.albedo_rect.zw);
		} else {
			decal_albedo = decal_atlas_srgb.SampleLevel(decal_sampler, uv_local.xz * decal.albedo_rect.zw + decal.albedo_rect.xy, 0.0);
		}
		decal_albedo *= decal.modulate;
		decal_albedo.a *= fade;
		albedo = lerp(albedo, decal_albedo.rgb, decal_albedo.a * decal.albedo_mix);

		if (any(decal.normal_rect != float4(0.0))) {
			float3 decal_normal;
			if (use_mipmaps) {
				decal_normal = decal_atlas.SampleGrad(decal_sampler, uv_local.xz * decal.normal_rect.zw + decal.normal_rect.xy, ddx * decal.normal_rect.zw, ddy * decal.normal_rect.zw).xyz;
			} else {
				decal_normal = decal_atlas.SampleLevel(decal_sampler, uv_local.xz * decal.normal_rect.zw + decal.normal_rect.xy, 0.0).xyz;
			}
			decal_normal.xy = decal_normal.xy * float2(2.0, -2.0) - float2(1.0, -1.0);
			decal_normal.z = sqrt(max(0.0, 1.0 - dot(decal_normal.xy, decal_normal.xy)));

			decal_normal = (mul(decal_normal.xzy, decal.normal_xform)).xyz;

			normal = normalize(lerp(normal, decal_normal, decal_albedo.a));
		}

		if (any(decal.orm_rect != float4(0.0))) {
			float3 decal_orm;
			if (use_mipmaps) {
				decal_orm = decal_atlas.SampleGrad(decal_sampler, uv_local.xz * decal.orm_rect.zw + decal.orm_rect.xy, ddx * decal.orm_rect.zw, ddy * decal.orm_rect.zw).xyz;
			} else {
				decal_orm = decal_atlas.SampleLevel(decal_sampler, uv_local.xz * decal.orm_rect.zw + decal.orm_rect.xy, 0.0).xyz;
			}
			ao = lerp(ao, decal_orm.r, decal_albedo.a);
			roughness = lerp(roughness, decal_orm.g, decal_albedo.a);
			metallic = lerp(metallic, decal_orm.b, decal_albedo.a);
		}
	}

	if (any(decal.emission_rect != float4(0.0))) {

		if (use_mipmaps) {
			emission += decal_atlas_srgb.SampleGrad(decal_sampler, uv_local.xz * decal.emission_rect.zw + decal.emission_rect.xy, ddx * decal.emission_rect.zw, ddy * decal.emission_rect.zw).xyz * decal.modulate.rgb * decal.emission_energy * fade;
		} else {
			emission += decal_atlas_srgb.SampleLevel(decal_sampler, uv_local.xz * decal.emission_rect.zw + decal.emission_rect.xy, 0.0).xyz * decal.modulate.rgb * decal.emission_energy * fade;
		}
	}
}
