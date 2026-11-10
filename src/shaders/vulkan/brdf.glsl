// Metallic-roughness BRDF: GGX distribution, height-correlated Smith
// visibility, Schlick Fresnel, Lambert diffuse.
#ifndef HRL_BRDF_GLSL
#define HRL_BRDF_GLSL

const float PI = 3.14159265359;

float D_GGX(float NdotH, float a)
{
	float a2 = a * a;
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * d * d, 1e-7);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float a)
{
	float a2 = a * a;
	float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
	float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
	return 0.5 / max(gv + gl, 1e-5);
}

vec3 F_Schlick(float cosTheta, vec3 F0)
{
	float f = pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
	return F0 + (1.0 - F0) * f;
}

// Radiance leaving the surface towards V for a light of radiance 1 coming from L
// (already multiplied by NdotL).
vec3 EvalBRDF(vec3 N, vec3 V, vec3 L, vec3 diffuseColor, vec3 F0, float roughness)
{
	float NdotL = dot(N, L);
	if (NdotL <= 0.0) return vec3(0.0);
	vec3 H = normalize(V + L);
	float NdotV = max(dot(N, V), 1e-4);
	float NdotH = max(dot(N, H), 0.0);
	float a = roughness * roughness;
	vec3 F = F_Schlick(max(dot(V, H), 0.0), F0);
	vec3 specular = F * (D_GGX(NdotH, a) * V_SmithGGXCorrelated(NdotV, NdotL, a));
	vec3 diffuse = (1.0 - F) * diffuseColor / PI;
	return (diffuse + specular) * NdotL;
}

vec2 Hammersley(uint i, uint n)
{
	uint bits = (i << 16u) | (i >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

vec3 ImportanceSampleGGX(vec2 xi, vec3 N, float a)
{
	float phi = 2.0 * PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
	vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 T = normalize(cross(up, N));
	vec3 B = cross(N, T);
	return normalize(T * H.x + B * H.y + N * H.z);
}

// Direction of a cube map texel. face : +X -X +Y -Y +Z -Z, uv in [0,1].
vec3 CubeDirection(uint face, vec2 uv)
{
	vec2 p = uv * 2.0 - 1.0;
	switch (face)
	{
	case 0u: return normalize(vec3(1.0, -p.y, -p.x));
	case 1u: return normalize(vec3(-1.0, -p.y, p.x));
	case 2u: return normalize(vec3(p.x, 1.0, p.y));
	case 3u: return normalize(vec3(p.x, -1.0, -p.y));
	case 4u: return normalize(vec3(p.x, -p.y, 1.0));
	default: return normalize(vec3(-p.x, -p.y, -1.0));
	}
}

// Equirectangular lookup, same mapping as the HRL sky sphere (+Y up).
vec2 EquirectUV(vec3 d)
{
	return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

#endif
