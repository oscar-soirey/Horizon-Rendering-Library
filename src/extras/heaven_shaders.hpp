// HRL extras - GLSL of the Heaven style (see heaven_style.hpp).
//
// Render passes (HRL_CreateComputeShader, contract in src/shaders/vulkan/pass.glsl):
//   kFroxelInject     BEFORE_OPAQUE  participating media of the view, lit by every light
//                                    with its shadows, accumulated over frames; light
//                                    of the sun through the smoke volumes
//   kFroxelIntegrate  BEFORE_OPAQUE  scattering and transmittance from the camera to
//                                    every froxel (what HRL applies to the surfaces)
//   kSmokeMarch       AFTER_OPAQUE   smoke volumes ray marched at half resolution against
//                                    the depth: self shadowed, lit, fogged, accumulated
//   kSmokeComposite   AFTER_OPAQUE   depth aware upsampling, composited into the scene
//   kLens             BEFORE_TONEMAP heat haze, soft edges, fringes, halation, grade
// Surface shaders (HRL_CreateShader, contract in src/shaders/vulkan/custom.glsl):
//   kFormVertex / kFormSurface       smooth, dark, back lit shapes that may dissolve
//   kEmberVertex / kEmberSurface     rising sparks
// kCommon is prepended to every source: shared noise (the baked atlas of
// heaven_noise.hpp) and the smoke volumes. Its helpers start with hv_.
#ifndef HRL_EXTRAS_HEAVEN_SHADERS_HPP
#define HRL_EXTRAS_HEAVEN_SHADERS_HPP

namespace hrl::heaven::glsl
{
// ---------------------------------------------------------------------------
// Shared code
// ---------------------------------------------------------------------------
inline constexpr const char* kCommon = R"GLSL(
uniform sampler2D NoiseAtlas;

// Tileable 3D noise (period 1), see heaven_noise.hpp: r Perlin-Worley, g / b Worley fbm, a Perlin fbm.
vec4 hv_Noise(vec3 p)
{
	const float N = 64.0, TILE = 66.0, GRID = 8.0, ATLAS = 528.0;
	vec3 q = fract(p) * N;
	float z = q.z - 0.5;
	float z0 = floor(z);
	float f = z - z0;
	float s0 = mod(z0 + N, N), s1 = mod(z0 + 1.0, N);
	vec2 inTile = q.xy + 1.0;
	vec2 o0 = vec2(mod(s0, GRID), floor(s0 / GRID)) * TILE;
	vec2 o1 = vec2(mod(s1, GRID), floor(s1 / GRID)) * TILE;
	vec4 a = textureLod(NoiseAtlas, (o0 + inTile) / ATLAS, 0.0);
	vec4 b = textureLod(NoiseAtlas, (o1 + inTile) / ATLAS, 0.0);
	return mix(a, b, f);
}

float hv_Remap(float v, float lo, float hi) { return clamp((v - lo) / max(hi - lo, 1e-4), 0.0, 1.0); }

float hv_Hash12(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

// Smoke volume (HeavenStyle::AddSmoke). 80 bytes, std430.
struct HeavenVolume
{
	vec4 center;    // xyz centre, w bounding radius
	vec4 axes;      // xyz half extents of the ellipsoid, w density (extinction per metre)
	vec4 color;     // rgb albedo, w anisotropy (-1..1)
	vec4 noise;     // x frequency (1 / metres), y erosion (0..1), z rise speed (m/s), w edge softness (0..1)
	vec4 emission;  // rgb light emitted per unit of density, w thinning towards the top (0..1)
};

// Density of a volume at p (0 outside). `detail`: erosion by the high frequencies.
float hv_VolumeDensity(HeavenVolume v, vec3 p, float time, bool detail)
{
	vec3 q = (p - v.center.xyz) / v.axes.xyz;
	float d2 = dot(q, q);
	if (d2 >= 1.0) return 0.0;
	float d = sqrt(d2);
	float shape = 1.0 - smoothstep(1.0 - clamp(v.noise.w, 0.02, 1.0), 1.0, d);
	shape *= 1.0 - v.emission.w * smoothstep(-0.3, 1.0, q.y);
	// Rising, slowly turning smoke: the domain moves up and is bent by a low frequency.
	vec3 np = p * v.noise.x - vec3(0.0, time * v.noise.z * v.noise.x, 0.0) + v.center.xyz * 0.173;
	vec4 warp = hv_Noise(np * 0.37 + vec3(time * 0.013, 0.0, time * 0.009));
	np += (warp.ar - 0.5).xyx * vec3(0.55, 0.35, 0.55);
	vec4 n = hv_Noise(np);
	float base = hv_Remap(n.r, 1.0 - shape * 0.8, 1.0);
	if (detail && base > 0.0)
	{
		vec4 n2 = hv_Noise(np * 3.17 + vec3(0.0, time * 0.04, 0.0));
		float wisps = n2.g * 0.625 + n2.b * 0.375;
		base = hv_Remap(base, v.noise.y * (1.0 - wisps), 1.0);
	}
	return base * v.axes.w;
}

// Ray / bounding sphere of a volume: entry and exit distances (t0 > t1 : missed).
vec2 hv_VolumeRange(HeavenVolume v, vec3 ro, vec3 rd)
{
	vec3 oc = ro - v.center.xyz;
	float b = dot(oc, rd);
	float c = dot(oc, oc) - v.center.w * v.center.w;
	float h = b * b - c;
	if (h < 0.0) return vec2(1.0, -1.0);
	h = sqrt(h);
	return vec2(-b - h, -b + h);
}
)GLSL";

// Shared by the passes that read the volumes.
inline constexpr const char* kVolumesBuffer = R"GLSL(
HRL_BUFFER(Volumes) { vec4 hvVolumeInfo; HeavenVolume hvVolumes[]; };
int hv_VolumeCount() { return int(hvVolumeInfo.x); }

// Optical depth of the volumes from p towards L (low frequencies only: shadows).
float hv_OpticalDepth(vec3 p, vec3 L, float maxDistance, int steps, float time)
{
	float od = 0.0;
	for (int i = 0; i < hv_VolumeCount(); ++i)
	{
		HeavenVolume v = hvVolumes[i];
		vec2 r = hv_VolumeRange(v, p, L);
		r.x = max(r.x, 0.0);
		r.y = min(r.y, maxDistance);
		if (r.x >= r.y) continue;
		float dt = (r.y - r.x) / float(steps);
		for (int s = 0; s < steps; ++s)
			od += hv_VolumeDensity(v, p + L * (r.x + (float(s) + 0.5) * dt), time, false) * dt;
	}
	return od;
}
)GLSL";

// ---------------------------------------------------------------------------
// Froxels: injection
// ---------------------------------------------------------------------------
inline constexpr const char* kFroxelInject = R"GLSL(
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
HRL_IMAGE3D(rgba16f, Scatter);       // rgb : light scattered per metre, a : extinction per metre
HRL_TEXTURE3D(ScatterHistory);
HRL_IMAGE3D(rgba16f, Visibility);    // r : sun light reaching the froxel through the smoke volumes

uniform vec3 FogAlbedo = vec3(1.0, 0.86, 0.72);
uniform float FogDensity = 0.035;
uniform float FogHeightFalloff = 0.12;
uniform float FogBaseHeight = 0.0;
uniform float FogNoiseFrequency = 0.035;
uniform float FogNoiseAmount = 0.8;
uniform vec3 FogWind = vec3(0.5, 0.06, 0.18);
uniform vec3 FogAmbient = vec3(0.02, 0.012, 0.008);
uniform float FogAnisotropy = 0.55;
uniform float AmbientScale = 1.0;
uniform float VolumeScatter = 0.6;   // the smoke volumes add their density to the media
uniform float TemporalBlend = 0.08;  // weight of this frame
uniform float ShadowSteps = 6.0;

float FogDensityAt(vec3 p, float time)
{
	float height = exp(-max(p.y - FogBaseHeight, 0.0) * FogHeightFalloff);
	vec3 q = (p - FogWind * time) * FogNoiseFrequency;
	vec4 n = hv_Noise(q);
	vec4 n2 = hv_Noise(q * 2.7 + vec3(0.0, time * 0.01, 0.0));
	float smoke = clamp(n.a * 0.65 + n2.r * 0.35, 0.0, 1.0);
	float shape = mix(1.0, smoothstep(0.25, 0.75, smoke) * 2.0, FogNoiseAmount);
	return FogDensity * height * shape;
}

void main()
{
	ivec3 cell = ivec3(gl_GlobalInvocationID);
	ivec3 size = imageSize(Scatter);
	if (any(greaterThanEqual(cell, size))) return;
	float time = hrlPass.frame.y;
	// Jittered inside the froxel every frame: the history integrates the whole cell.
	float j = HRL_FrameNoise(vec2(cell.xy) + vec2(cell.z * 7, cell.z * 13));
	vec3 uvw = (vec3(cell) + vec3(0.5, 0.5, j)) / vec3(size);
	vec3 p = HRL_FroxelWorld(uvw);
	vec3 V = normalize(p - uView.camPos.xyz);

	float density = FogDensityAt(p, time);
	vec3 albedo = FogAlbedo;
	float volumes = 0.0;
	vec3 volumeColor = vec3(0.0);
	vec3 emission = vec3(0.0);
	for (int i = 0; i < hv_VolumeCount(); ++i)
	{
		float d = hv_VolumeDensity(hvVolumes[i], p, time, false) * VolumeScatter;
		volumes += d;
		volumeColor += hvVolumes[i].color.rgb * d;
		emission += hvVolumes[i].emission.rgb * d;
	}
	if (volumes > 1e-5) albedo = (FogAlbedo * density + volumeColor) / (density + volumes);
	density += volumes;

	// Light: every light with its shadow (shadow maps or ray traced), the sun through the smoke.
	int mainLight = HRL_MainLight();
	float visibility = 1.0;
	if (mainLight >= 0)
	{
		vec3 L = normalize(-lights[mainLight].direction.xyz);
		visibility = exp(-hv_OpticalDepth(p, L, 200.0, int(ShadowSteps), time));
	}
	vec3 light = vec3(0.0);
	for (uint i = 0u; i < HRL_LightCount(); ++i)
	{
		vec3 L;
		vec3 radiance = HRL_LightRadiance(i, p, L);
		if (int(i) == mainLight) radiance *= visibility;
		light += radiance * HRL_PhaseHG(dot(V, L), FogAnisotropy);
	}
	light += (HRL_AmbientLight(vec3(0.0, 1.0, 0.0)) * AmbientScale) / (4.0 * HRL_PI) + FogAmbient;
	vec4 current = vec4(albedo * density * light + emission, density);

	// Temporal accumulation: the same point in the volume of the previous frame.
	vec3 previousUVW;
	if (hrlPass.frame.w > 0.5 && HRL_PreviousFroxelUVW(p, previousUVW))
	{
		vec4 history = textureLod(ScatterHistory, previousUVW, 0.0);
		current = mix(history, current, TemporalBlend);
	}
	imageStore(Scatter, cell, current);
	imageStore(Visibility, cell, vec4(visibility));
}
)GLSL";

// ---------------------------------------------------------------------------
// Froxels: integration along each view ray
// ---------------------------------------------------------------------------
inline constexpr const char* kFroxelIntegrate = R"GLSL(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
HRL_TEXTURE3D(Scatter);
HRL_IMAGE3D(rgba16f, Integrated);   // rgb : light scattered towards the camera, a : transmittance

void main()
{
	ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
	ivec3 size = imageSize(Integrated);
	if (any(greaterThanEqual(cell, size.xy))) return;
	vec2 uv = (vec2(cell) + 0.5) / vec2(size.xy);
	// World distance per unit of view depth along this ray.
	vec3 dir = normalize(HRL_FroxelWorld(vec3(uv, 1.0)) - uView.camPos.xyz);
	float stretch = 1.0 / max(dot(dir, HRL_CameraForward()), 1e-3);
	vec3 scattered = vec3(0.0);
	float transmittance = 1.0;
	float previousDepth = 0.0;
	for (int z = 0; z < size.z; ++z)
	{
		float w = float(z + 1) / float(size.z);
		float depth = pow(w, uView.mediaParams.y) * uView.mediaParams.x;
		float ds = (depth - previousDepth) * stretch;
		previousDepth = depth;
		vec4 s = texelFetch(Scatter, ivec3(cell, z), 0);
		float extinction = max(s.a, 1e-6);
		float t = exp(-extinction * ds);
		// Energy conserving integration over the slice (Hillaire 2015).
		scattered += transmittance * (s.rgb - s.rgb * t) / extinction;
		transmittance *= t;
		imageStore(Integrated, ivec3(cell, z), vec4(scattered, transmittance));
	}
}
)GLSL";

// ---------------------------------------------------------------------------
// Smoke volumes: half resolution ray march
// ---------------------------------------------------------------------------
inline constexpr const char* kSmokeMarch = R"GLSL(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
HRL_IMAGE2D(rgba16f, Smoke);         // rgb : light of the smoke towards the camera, a : transmittance
HRL_TEXTURE2D(SmokeHistory);
HRL_IMAGE2D(rg16f, SmokeDepth);      // r : view depth of the scene behind, g : view depth of the smoke

uniform float Steps = 40.0;
uniform float ShadowSteps = 5.0;
uniform float ShadowDistance = 12.0;
uniform float MaxDistance = 150.0;
uniform float AmbientScale = 1.0;
uniform vec3 SmokeAmbient = vec3(0.02, 0.012, 0.008);
uniform float MultipleScattering = 0.55;
uniform float TemporalBlend = 0.2;

void main()
{
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(Smoke);
	if (any(greaterThanEqual(px, size))) return;
	float time = hrlPass.frame.y;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	float depth = HRL_SceneDepth(uv);
	vec3 ro = uView.camPos.xyz;
	vec3 rd = HRL_ViewRay(uv);
	float sceneDistance = HRL_IsSky(depth) ? 1e6 : length(HRL_WorldPosition(uv, depth) - ro);
	float tEnd = min(sceneDistance, MaxDistance);
	float sceneViewDepth = HRL_IsSky(depth) ? 65000.0 : sceneDistance * dot(rd, HRL_CameraForward());

	// Part of the ray inside the bounding spheres.
	float t0 = 1e9, t1 = -1.0;
	for (int i = 0; i < hv_VolumeCount(); ++i)
	{
		vec2 r = hv_VolumeRange(hvVolumes[i], ro, rd);
		if (r.x > r.y) continue;
		t0 = min(t0, max(r.x, 0.0));
		t1 = max(t1, min(r.y, tEnd));
	}
	vec3 scattered = vec3(0.0);
	float transmittance = 1.0;
	float weightedDepth = 0.0, weight = 0.0;
	if (t1 > t0)
	{
		int steps = int(clamp(Steps, 4.0, 256.0));
		float dt = (t1 - t0) / float(steps);
		float t = t0 + dt * HRL_FrameNoise(vec2(px));
		int mainLight = HRL_MainLight();
		for (int s = 0; s < steps && transmittance > 0.004; ++s, t += dt)
		{
			vec3 p = ro + rd * t;
			float density = 0.0;
			vec3 albedo = vec3(0.0), emission = vec3(0.0);
			float g = 0.0;
			for (int i = 0; i < hv_VolumeCount(); ++i)
			{
				float d = hv_VolumeDensity(hvVolumes[i], p, time, true);
				density += d;
				albedo += hvVolumes[i].color.rgb * d;
				g += hvVolumes[i].color.w * d;
				emission += hvVolumes[i].emission.rgb * d;
			}
			if (density < 1e-4) continue;
			albedo /= density;
			g /= density;

			vec3 light = vec3(0.0);
			for (uint i = 0u; i < HRL_LightCount(); ++i)
			{
				vec3 L;
				vec3 radiance = HRL_LightRadiance(i, p, L);
				if (dot(radiance, radiance) < 1e-8) continue;
				float maxDistance = int(i) == mainLight ? ShadowDistance : min(ShadowDistance, length(lights[i].position.xyz - p));
				float od = hv_OpticalDepth(p, L, maxDistance, int(ShadowSteps), time);
				// Multiple scattering approximation (Wrenninge): octaves of weaker extinction.
				float cosTheta = dot(rd, L);
				float ms = 0.0, a = 1.0, b = 1.0, c = 1.0;
				for (int o = 0; o < 3; ++o)
				{
					ms += a * exp(-od * b) * mix(HRL_PhaseHG(cosTheta, g * c), HRL_PhaseHG(cosTheta, -0.25 * c), 0.25);
					a *= MultipleScattering; b *= 0.4; c *= 0.5;
				}
				light += radiance * ms;
			}
			// Sky and environment, darker inside the smoke.
			float ambientOcclusion = exp(-hv_OpticalDepth(p, vec3(0.0, 1.0, 0.0), 4.0, 2, time) * 0.5);
			light += (HRL_AmbientLight(vec3(0.0, 1.0, 0.0)) * AmbientScale / (4.0 * HRL_PI) + SmokeAmbient) * ambientOcclusion;

			vec3 radianceOut = albedo * density * light + emission;
			// The participating media in front of the sample dims it.
			radianceOut *= HRL_SampleMedia(p).a;
			float st = exp(-density * dt);
			scattered += transmittance * (radianceOut - radianceOut * st) / density;
			float absorbed = transmittance * (1.0 - st);
			weightedDepth += absorbed * t;
			weight += absorbed;
			transmittance *= st;
		}
	}
	vec4 current = vec4(scattered, transmittance);
	float smokeDistance = weight > 1e-4 ? weightedDepth / weight : tEnd;

	// Temporal accumulation at the place the smoke was in the previous frame.
	vec2 previousUV;
	if (hrlPass.frame.w > 0.5 && HRL_PreviousUV(ro + rd * smokeDistance, previousUV))
	{
		vec4 history = textureLod(SmokeHistory, previousUV, 0.0);
		current = mix(history, current, TemporalBlend);
	}
	imageStore(Smoke, px, current);
	imageStore(SmokeDepth, px, vec4(sceneViewDepth, smokeDistance * dot(rd, HRL_CameraForward()), 0.0, 0.0));
}
)GLSL";

// ---------------------------------------------------------------------------
// Smoke volumes: depth aware upsampling into the scene
// ---------------------------------------------------------------------------
inline constexpr const char* kSmokeComposite = R"GLSL(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
HRL_TEXTURE2D(Smoke);
HRL_TEXTURE2D(SmokeDepth);

uniform float DepthSharpness = 12.0;

void main()
{
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, HRL_ViewportSize()))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(HRL_ViewportSize());
	float depth = HRL_SceneDepth(uv);
	float viewDepth = HRL_IsSky(depth) ? 65000.0 : HRL_ViewDepth(HRL_WorldPosition(uv, depth));

	ivec2 halfSize = textureSize(Smoke, 0);
	vec2 c = uv * vec2(halfSize) - 0.5;
	ivec2 base = ivec2(floor(c));
	vec2 f = c - vec2(base);
	vec4 sum = vec4(0.0);
	float total = 0.0;
	vec4 nearest = vec4(0.0, 0.0, 0.0, 1.0);
	float nearestError = 1e9;
	for (int y = 0; y < 2; ++y)
		for (int x = 0; x < 2; ++x)
		{
			ivec2 q = clamp(base + ivec2(x, y), ivec2(0), halfSize - 1);
			float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
			float sampleDepth = texelFetch(SmokeDepth, q, 0).r;
			float error = abs(sampleDepth - viewDepth) / max(viewDepth, 1e-3);
			float w = bilinear * exp(-error * DepthSharpness * 4.0);
			vec4 s = texelFetch(Smoke, q, 0);
			sum += s * w;
			total += w;
			if (error < nearestError) { nearestError = error; nearest = s; }
		}
	vec4 smoke = total > 1e-4 ? sum / total : nearest;
	ivec2 scenePixel = HRL_ScenePixel(px);
	vec4 scene = imageLoad(hrl_SceneImage, scenePixel);
	imageStore(hrl_SceneImage, scenePixel, vec4(scene.rgb * smoke.a + smoke.rgb, scene.a));
}
)GLSL";

// ---------------------------------------------------------------------------
// Lens and grade (HDR, before the built-in bloom / tone mapping / vignette / grain)
// ---------------------------------------------------------------------------
inline constexpr const char* kLens = R"GLSL(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform float HazeDistortion = 0.003;   // viewport UV
uniform float HazeFrequency = 4.0;
uniform float EdgeBlur = 6.0;           // pixels, at the corners
uniform float EdgeStart = 0.3;
uniform float Aberration = 3.5;         // pixels, at the corners
uniform float HalationStrength = 0.18;
uniform float HalationRadius = 22.0;    // pixels
uniform float HalationThreshold = 1.2;
uniform vec3 HalationTint = vec3(1.0, 0.42, 0.18);
uniform vec3 Grade = vec3(1.0, 0.82, 0.62);
uniform vec3 ShadowTint = vec3(0.010, 0.005, 0.0025);
uniform float Crush = 1.15;
uniform float Desaturate = 0.2;
uniform float Breathing = 0.04;

vec3 Scene(vec2 viewportUV) { return textureLod(hrl_SceneColor, HRL_SceneUV(clamp(viewportUV, 0.0, 1.0)), 0.0).rgb; }

void main()
{
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = HRL_ViewportSize();
	if (any(greaterThanEqual(px, size))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	float time = hrlPass.frame.y;
	float aspect = float(size.x) / float(size.y);
	vec2 c = uv - 0.5;
	float r = length(c * vec2(aspect, 1.0));
	vec2 dir = length(c) > 1e-4 ? normalize(c) : vec2(0.0);
	vec2 texel = 1.0 / vec2(size);

	// Heat haze: slow wobble, stronger on the edges and low in the image.
	vec4 n = hv_Noise(vec3(uv * HazeFrequency * vec2(aspect, 1.0) * 0.25, time * 0.02));
	float hazeMask = 0.35 + smoothstep(0.15, 0.8, r) + uv.y * 0.5;
	vec2 p = uv + (n.ar - 0.5) * HazeDistortion * hazeMask;

	// Soft focus with colour fringes, growing towards the corners.
	float edge = smoothstep(EdgeStart, 0.95, r);
	float radius = EdgeBlur * edge;
	float ca = Aberration * edge;
	float rotation = HRL_FrameNoise(vec2(px)) * 6.2831853;
	vec3 color = vec3(0.0);
	const int TAPS = 12;
	for (int k = 0; k < TAPS; ++k)
	{
		float fk = float(k) + 0.5;
		float a = fk * 2.39996 + rotation;
		vec2 o = vec2(cos(a), sin(a)) * sqrt(fk / float(TAPS)) * radius * texel;
		vec2 shift = dir * ca * texel;
		color += vec3(Scene(p + o + shift).r, Scene(p + o).g, Scene(p + o - shift).b);
	}
	color /= float(TAPS);

	// Halation: a warm halo around the brightest parts.
	vec3 halo = vec3(0.0);
	for (int k = 0; k < 10; ++k)
	{
		float fk = float(k) + 0.5;
		float a = fk * 2.39996 + rotation * 0.5;
		vec2 o = vec2(cos(a), sin(a)) * sqrt(fk / 10.0) * HalationRadius * texel;
		halo += max(Scene(p + o) - HalationThreshold, 0.0);
	}
	color += halo / 10.0 * HalationTint * HalationStrength;

	// Grade.
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	color = mix(color, vec3(luma), Desaturate);
	color *= Grade;
	color = pow(max(color, 0.0), vec3(Crush));
	color += ShadowTint * (1.0 - smoothstep(0.0, 0.06, luma));
	color *= 1.0 + Breathing * (hv_Noise(vec3(time * 0.03, 0.31, 0.77)).a - 0.5) * 2.0;
	imageStore(hrl_SceneImage, HRL_ScenePixel(px), vec4(color, 1.0));
}
)GLSL";

// ---------------------------------------------------------------------------
// Surfaces: smooth back lit forms
// ---------------------------------------------------------------------------
inline constexpr const char* kFormVertex = R"GLSL(
uniform float Breath = 0.02;       // soft swelling of the surface (metres)
uniform float BreathFrequency = 0.6;
uniform float BreathSpeed = 0.05;
uniform float Sway = 0.0;          // sway growing with the height (trees, figures)
uniform float SwayBase = 0.0;      // height where the sway starts
uniform float Flap = 0.0;          // wing flap (uv.x = 0.5 on the body, 0 / 1 at the tips)
uniform float FlapSpeed = 9.0;

void Vertex(inout HRL_VertexData v)
{
	float id = float(v.objectId);
	vec3 p = v.worldPosition;
	float n = hv_Noise(p * BreathFrequency * 0.2 + vec3(v.time * BreathSpeed, 0.0, id * 0.137)).r - 0.5;
	v.worldPosition += v.normal * n * 2.0 * Breath;
	if (Sway > 0.0)
	{
		float h = max(p.y - SwayBase, 0.0);
		float s = sin(v.time * 0.55 + id * 0.9 + p.y * 0.15) + 0.4 * sin(v.time * 1.31 + id);
		v.worldPosition.x += s * Sway * h * h * 0.01;
		v.worldPosition.z += cos(v.time * 0.43 + id * 1.7) * Sway * h * h * 0.006;
	}
	if (Flap > 0.0)
	{
		float span = abs(v.uv.x - 0.5) * 2.0;
		v.worldPosition.y += sin(v.time * FlapSpeed + id * 2.1) * span * span * Flap;
	}
}
)GLSL";

inline constexpr const char* kFormSurface = R"GLSL(
uniform vec3 HvGlowPosition = vec3(0.0, 4.0, -30.0);
uniform vec3 HvGlowColor = vec3(9.0, 4.6, 2.0);
uniform vec3 FormColor = vec3(0.03, 0.02, 0.015);
uniform float FormRoughness = 0.9;
uniform float FormSpecular = 0.08;  // low: no glare of the low sun on the forms
uniform float RimPower = 5.0;
uniform float RimStrength = 0.3;
uniform float RimFlicker = 0.9;     // the outline moves like smoke
uniform float Dissolve = 0.0;       // 0 solid, 1 the silhouette tears into smoke (alpha tested / translucent material)
uniform float DissolveFrequency = 0.9;
uniform float DissolveHeight = 0.0; // the dissolve grows below this height (smoky feet), 0 : everywhere
uniform float Glow = 0.0;

void Surface(HRL_SurfaceInput i, inout HRL_Surface s)
{
	vec3 N = normalize(s.normal);
	vec3 V = normalize(i.viewDirection);
	float ndv = clamp(dot(N, V), 0.0, 1.0);
	vec3 toGlow = HvGlowPosition - i.worldPosition;
	float dGlow = length(toGlow);
	vec3 L = toGlow / max(dGlow, 1e-3);
	float behind = clamp(dot(-V, L) * 0.5 + 0.5, 0.0, 1.0);
	behind *= behind;
	float atten = 1.0 / (1.0 + dGlow * dGlow * 0.0025);
	vec4 n = hv_Noise(i.worldPosition * DissolveFrequency * 0.25 + vec3(0.0, -i.time * 0.04, 0.0));

	float fres = pow(1.0 - ndv, RimPower);
	float flicker = mix(1.0, 0.4 + 1.2 * n.r, RimFlicker);
	s.baseColor = FormColor;
	s.roughness = FormRoughness;
	s.metallic = 0.0;
	s.specular = FormSpecular;
	s.emissive = HvGlowColor * fres * (0.05 + 1.95 * behind) * atten * RimStrength * flicker + Glow * HvGlowColor * atten;

	if (Dissolve > 0.0)
	{
		float amount = Dissolve;
		if (DissolveHeight != 0.0) amount *= smoothstep(DissolveHeight, DissolveHeight - 1.5, i.worldPosition.y);
		// Edges first: grazing angles tear before the faces looking at the camera.
		float tear = n.r * 0.7 + n.g * 0.3 + (1.0 - ndv) * 0.35;
		s.opacity = clamp((1.0 - amount * 1.35) + (1.0 - tear) * amount * 1.2, 0.0, 1.0) * s.opacity;
		s.emissive += HvGlowColor * atten * behind * amount * (1.0 - s.opacity) * 0.4;
	}
}
)GLSL";

inline constexpr const char* kEmberVertex = R"GLSL(
uniform float EmberRise = 0.45;
uniform float EmberHeight = 10.0;
uniform vec3 EmberWind = vec3(0.25, 0.0, 0.08);
uniform float EmberWander = 0.6;
uniform float EmberRadius = 0.02;   // radius of the ember mesh

void Vertex(inout HRL_VertexData v)
{
	float id = float(v.objectId);
	float h = hv_Hash12(vec2(id, 1.7));
	float y = mod(v.time * EmberRise * (0.5 + h) + h * EmberHeight, EmberHeight);
	vec3 offset = vec3(
		sin(v.time * (0.7 + h) + id) * EmberWander + y * EmberWind.x * 4.0,
		y,
		cos(v.time * (0.5 + h * 0.8) + id * 1.7) * EmberWander + y * EmberWind.z * 4.0);
	float size = 1.0 - smoothstep(0.55, 1.0, y / EmberHeight);
	vec3 centre = v.worldPosition - v.normal * EmberRadius;
	v.worldPosition = centre + (v.worldPosition - centre) * size + offset;
}
)GLSL";

inline constexpr const char* kEmberSurface = R"GLSL(
uniform vec3 EmberColor = vec3(14.0, 4.2, 1.1);

void Surface(HRL_SurfaceInput i, inout HRL_Surface s)
{
	float id = float(i.objectId);
	float flicker = 0.55 + 0.45 * sin(i.time * (6.0 + mod(id, 5.0)) + id * 3.1);
	s.baseColor = vec3(0.0);
	s.roughness = 1.0;
	s.specular = 0.0;
	s.emissive = EmberColor * flicker;
}
)GLSL";
} // namespace hrl::heaven::glsl

#endif
