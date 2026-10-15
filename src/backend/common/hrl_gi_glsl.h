// Global illumination shaders (GLSL 3.30), shared by the OpenGL 3.3 and
// Vulkan backends. A program is assembled as:
//   "#version 330 core" + kGICommon + <pass source>
//
// Conventions:
//  * "indirect" = mean incoming radiance over the cosine-weighted hemisphere,
//    i.e. irradiance / PI. A diffuse surface outputs albedo * indirect.
//  * The scene is described by a BVH stored in two RGBA32F textures (see
//    core/gi_scene.h) and by the LightBlock uniform buffer of the renderer.
#ifndef HRL_GI_GLSL_H
#define HRL_GI_GLSL_H

#include <string>

namespace hrl_gi_glsl
{
inline constexpr const char* kVersion = "#version 330 core\n";

inline constexpr const char* kVertex = R"GLSL(#version 330 core
layout(location = 0) in vec2 apos;
layout(location = 1) in vec2 auv;
out vec2 uv;
void main()
{
    uv = auv;
    gl_Position = vec4(apos, 0.0, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Common library : BVH traversal, lights, sky, sampling, volumes, DDGI.
// ---------------------------------------------------------------------------
inline constexpr const char* kCommon = R"GLSL(
#define GI_PI 3.14159265359
#define GI_MAX_LIGHTS 32
#define GI_PointLight       (uint(0x0011))
#define GI_DirectionalLight (uint(0x0012))
#define GI_SpotLight        (uint(0x0013))
#define GI_SkyLight         (uint(0x0014))

struct Light
{
    uint type;
    float intensity;
    float attenuation;
    float innerCutoff;
    vec3 position;
    float outerCutoff;
    vec3 rotation;
    float padding3;
    vec3 color;
    float shadowStrength;
    mat4 shadowMatrix;
    vec4 shadowParams;
};
layout(std140) uniform LightBlock
{
    Light lights[GI_MAX_LIGHTS];
};

uniform sampler2D uGINodes;
uniform sampler2D uGITris;
uniform float uGIBias;

uniform int uGISkyEnabled;
uniform int uGISkyTextureEnabled;
uniform sampler2D uGISkyTexture;
uniform vec3 uGISkyTop;
uniform vec3 uGISkyHorizon;
uniform vec3 uGISkyBottom;

uniform int uGIFrame;

// ---- random numbers ----
uint GIHash(uint x)
{
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
uint GISeed(ivec2 pixel, int frame, int stream)
{
    return GIHash(uint(pixel.x) * 1973u + GIHash(uint(pixel.y) * 9277u + GIHash(uint(frame) * 26699u + uint(stream))));
}
float GIRand(inout uint s)
{
    s = GIHash(s);
    return float(s >> 8u) * (1.0 / 16777216.0);
}

void GIBasis(vec3 n, out vec3 t, out vec3 b)
{
    t = normalize(abs(n.y) < 0.999 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    b = cross(n, t);
}
vec3 GICosineSample(vec3 n, vec2 u)
{
    vec3 t, b;
    GIBasis(n, t, b);
    float r = sqrt(u.x);
    float phi = 2.0 * GI_PI * u.y;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u.x)));
}

// ---- octahedral mapping ----
vec2 GISignNotZero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 GIOctEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 p = n.xy;
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * GISignNotZero(p);
    return p;
}
vec3 GIOctDecode(vec2 p)
{
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * GISignNotZero(n.xy);
    return normalize(n);
}

// ---- BVH ----
ivec2 GIAddr(int i, int w) { return ivec2(i - (i / w) * w, i / w); }
vec4 GINode(int i) { return texelFetch(uGINodes, GIAddr(i, textureSize(uGINodes, 0).x), 0); }
vec4 GITri(int i) { return texelFetch(uGITris, GIAddr(i, textureSize(uGITris, 0).x), 0); }

vec3 GISafeInverse(vec3 d)
{
    vec3 s = vec3(d.x < 0.0 ? -1.0 : 1.0, d.y < 0.0 ? -1.0 : 1.0, d.z < 0.0 ? -1.0 : 1.0);
    return 1.0 / (s * max(abs(d), vec3(1e-9)));
}
bool GIBox(vec3 o, vec3 invD, vec3 bmin, vec3 bmax, float tmax)
{
    vec3 t0 = (bmin - o) * invD;
    vec3 t1 = (bmax - o) * invD;
    vec3 tn = min(t0, t1);
    vec3 tf = max(t0, t1);
    float a = max(max(tn.x, tn.y), max(tn.z, 0.0));
    float b = min(min(tf.x, tf.y), min(tf.z, tmax));
    return a <= b;
}
bool GITriangle(vec3 o, vec3 d, int tri, float tmax, out float t, out vec2 bary)
{
    vec3 p0 = GITri(tri * 3).xyz;
    vec3 e1 = GITri(tri * 3 + 1).xyz - p0;
    vec3 e2 = GITri(tri * 3 + 2).xyz - p0;
    vec3 pv = cross(d, e2);
    float det = dot(e1, pv);
    t = 0.0; bary = vec2(0.0);
    if (abs(det) < 1e-12) return false;
    float inv = 1.0 / det;
    vec3 tv = o - p0;
    float u = dot(tv, pv) * inv;
    if (u < 0.0 || u > 1.0) return false;
    vec3 qv = cross(tv, e1);
    float v = dot(d, qv) * inv;
    if (v < 0.0 || u + v > 1.0) return false;
    t = dot(e2, qv) * inv;
    bary = vec2(u, v);
    return t > 0.0 && t < tmax;
}
// Closest hit. Returns the triangle index or -1.
int GITrace(vec3 o, vec3 d, float tmax, out float tHit)
{
    vec3 invD = GISafeInverse(d);
    int stack[48];
    int sp = 0;
    stack[sp++] = 0;
    int hit = -1;
    tHit = tmax;
    for (int iter = 0; iter < 8192 && sp > 0; ++iter)
    {
        int n = stack[--sp];
        vec4 a = GINode(n * 2);
        vec4 b = GINode(n * 2 + 1);
        if (!GIBox(o, invD, a.xyz, b.xyz, tHit)) continue;
        if (b.w < 0.0)
        {
            int first = int(a.w + 0.5);
            int count = int(-b.w + 0.5);
            for (int k = 0; k < count; ++k)
            {
                float t; vec2 bary;
                if (GITriangle(o, d, first + k, tHit, t, bary)) { tHit = t; hit = first + k; }
            }
        }
        else if (sp < 46)
        {
            stack[sp++] = int(b.w + 0.5);
            stack[sp++] = int(a.w + 0.5);
        }
    }
    return hit;
}
// Any hit (shadow rays).
bool GIOccluded(vec3 o, vec3 d, float tmax)
{
    vec3 invD = GISafeInverse(d);
    int stack[48];
    int sp = 0;
    stack[sp++] = 0;
    for (int iter = 0; iter < 8192 && sp > 0; ++iter)
    {
        int n = stack[--sp];
        vec4 a = GINode(n * 2);
        vec4 b = GINode(n * 2 + 1);
        if (!GIBox(o, invD, a.xyz, b.xyz, tmax)) continue;
        if (b.w < 0.0)
        {
            int first = int(a.w + 0.5);
            int count = int(-b.w + 0.5);
            for (int k = 0; k < count; ++k)
            {
                float t; vec2 bary;
                if (GITriangle(o, d, first + k, tmax, t, bary)) return true;
            }
        }
        else if (sp < 46)
        {
            stack[sp++] = int(b.w + 0.5);
            stack[sp++] = int(a.w + 0.5);
        }
    }
    return false;
}
vec3 GITriAlbedo(int tri) { return vec3(GITri(tri * 3).w, GITri(tri * 3 + 1).w, GITri(tri * 3 + 2).w); }
vec3 GITriNormal(int tri)
{
    vec3 p0 = GITri(tri * 3).xyz;
    return normalize(cross(GITri(tri * 3 + 1).xyz - p0, GITri(tri * 3 + 2).xyz - p0));
}

// ---- sky and lights ----
vec2 GIEquirect(vec3 d)
{
    d = normalize(d);
    return vec2(atan(d.z, d.x) / (2.0 * GI_PI) + 0.5, clamp(0.5 + asin(clamp(d.y, -1.0, 1.0)) / GI_PI, 0.001, 0.999));
}
// Radiance of the sky sphere (zero when the scene has no sky sphere).
vec3 GISky(vec3 d)
{
    if (uGISkyEnabled == 0) return vec3(0.0);
    if (uGISkyTextureEnabled != 0) return texture(uGISkyTexture, GIEquirect(d)).rgb;
    float h = clamp(normalize(d).y, -1.0, 1.0);
    if (h >= 0.0) return mix(uGISkyHorizon, uGISkyTop, smoothstep(0.0, 1.0, h));
    return mix(uGISkyBottom, uGISkyHorizon, smoothstep(-1.0, 0.0, h));
}
// Ambient term of the sky lights (same as the mesh shader).
vec3 GISkyAmbient()
{
    vec3 c = vec3(0.0);
    for (int i = 0; i < GI_MAX_LIGHTS; ++i)
        if (lights[i].intensity > 0.0 && lights[i].type == GI_SkyLight) c += lights[i].color * lights[i].intensity;
    return c;
}
// Direct irradiance from the scene lights, shadowed by ray tracing for the
// lights that cast shadows (like the shadow maps of the rasterizer).
vec3 GIDirectIrradiance(vec3 p, vec3 n)
{
    vec3 e = vec3(0.0);
    for (int i = 0; i < GI_MAX_LIGHTS; ++i)
    {
        Light l = lights[i];
        if (l.intensity <= 0.0 || l.type == GI_SkyLight) continue;
        vec3 dir;
        float att = 1.0;
        float maxT = 1e6;
        if (l.type == GI_DirectionalLight)
            dir = normalize(-l.rotation);
        else
        {
            vec3 toLight = l.position - p;
            float dist = length(toLight);
            dir = dist > 1e-4 ? toLight / dist : vec3(0.0, 1.0, 0.0);
            att = 1.0 / (1.0 + l.attenuation * dist * dist);
            maxT = max(dist - uGIBias, uGIBias);
            if (l.type == GI_SpotLight)
                att *= smoothstep(l.outerCutoff, l.innerCutoff, dot(dir, normalize(-l.rotation)));
            else if (l.type != GI_PointLight)
                continue;
        }
        float ndl = dot(n, dir);
        if (ndl <= 0.0 || att <= 0.0) continue;
        if (l.shadowParams.z > -0.5 && GIOccluded(p + n * uGIBias, dir, maxT)) continue;
        e += l.color * l.intensity * att * ndl;
    }
    return e;
}
// Radiance leaving a diffuse surface lit by the direct lights.
vec3 GISurfaceRadiance(vec3 p, vec3 n, vec3 albedo)
{
    return albedo * (GIDirectIrradiance(p, n) / GI_PI + GISkyAmbient());
}

// ---- volumes stored as 2D atlases of Z slices ----
ivec2 GIVolumeTexel(ivec3 c, int res, int tilesPerRow)
{
    return ivec2(c.x + (c.z - (c.z / tilesPerRow) * tilesPerRow) * res, c.y + (c.z / tilesPerRow) * res);
}
ivec3 GIVolumeCell(ivec2 texel, int res, int tilesPerRow)
{
    ivec2 tile = texel / res;
    return ivec3(texel - tile * res, tile.y * tilesPerRow + tile.x);
}
// xOffset selects one region of an atlas holding several volumes side by side.
vec4 GIVolumeFetchAt(sampler2D s, ivec3 c, int res, int tilesPerRow, int xOffset)
{
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(res)))) return vec4(0.0);
    return texelFetch(s, GIVolumeTexel(c, res, tilesPerRow) + ivec2(xOffset, 0), 0);
}
vec4 GIVolumeFetch(sampler2D s, ivec3 c, int res, int tilesPerRow)
{
    return GIVolumeFetchAt(s, c, res, tilesPerRow, 0);
}
// Trilinear sample, g in cells (cell centers at +0.5).
vec4 GIVolumeSampleAt(sampler2D s, vec3 g, int res, int tilesPerRow, int xOffset)
{
    vec3 f = g - 0.5;
    ivec3 b = ivec3(floor(f));
    vec3 w = f - vec3(b);
    vec4 r = vec4(0.0);
    for (int i = 0; i < 8; ++i)
    {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        vec3 ww = mix(1.0 - w, w, vec3(o));
        r += GIVolumeFetchAt(s, b + o, res, tilesPerRow, xOffset) * (ww.x * ww.y * ww.z);
    }
    return r;
}
vec4 GIVolumeSample(sampler2D s, vec3 g, int res, int tilesPerRow)
{
    vec3 f = g - 0.5;
    ivec3 b = ivec3(floor(f));
    vec3 w = f - vec3(b);
    vec4 r = vec4(0.0);
    for (int i = 0; i < 8; ++i)
    {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        vec3 ww = mix(1.0 - w, w, vec3(o));
        r += GIVolumeFetch(s, b + o, res, tilesPerRow) * (ww.x * ww.y * ww.z);
    }
    return r;
}

// ---- DDGI probe volume ----
#define DDGI_IRRADIANCE_TEXELS 8
#define DDGI_DISTANCE_TEXELS 16
uniform sampler2D uDDGIIrradiance;
uniform sampler2D uDDGIDistance;
uniform vec3 uDDGICountsF;
#define uDDGICounts ivec3(uDDGICountsF + 0.5)
uniform vec3 uDDGIOrigin;
uniform vec3 uDDGISpacing;
uniform int uDDGITilesPerRow;
uniform int uDDGIRays;
uniform mat3 uDDGIRayRotation;

vec3 DDGIProbePosition(ivec3 c) { return uDDGIOrigin + uDDGISpacing * vec3(c); }
int DDGIProbeIndex(ivec3 c) { return c.x + c.y * uDDGICounts.x + c.z * uDDGICounts.x * uDDGICounts.y; }
ivec3 DDGIProbeCoords(int i)
{
    int xy = uDDGICounts.x * uDDGICounts.y;
    int z = i / xy;
    int r = i - z * xy;
    return ivec3(r - (r / uDDGICounts.x) * uDDGICounts.x, r / uDDGICounts.x, z);
}
vec3 DDGIRayDirection(int ray)
{
    // Spherical Fibonacci directions, randomly rotated every frame.
    float golden = 2.39996322972865332;
    float y = 1.0 - 2.0 * (float(ray) + 0.5) / float(uDDGIRays);
    float r = sqrt(max(0.0, 1.0 - y * y));
    float a = golden * float(ray);
    return normalize(uDDGIRayRotation * vec3(cos(a) * r, y, sin(a) * r));
}
vec2 DDGITexCoord(int probe, vec3 dir, int texels, sampler2D tex)
{
    ivec2 tile = ivec2(probe - (probe / uDDGITilesPerRow) * uDDGITilesPerRow, probe / uDDGITilesPerRow);
    vec2 oct = GIOctEncode(normalize(dir)) * 0.5 + 0.5;
    vec2 px = vec2(tile * (texels + 2)) + 1.0 + oct * float(texels);
    return px / vec2(textureSize(tex, 0));
}
// Irradiance / PI at a surface, from the 8 surrounding probes with the
// visibility test of DDGI (Chebyshev on the probe depth moments).
vec3 DDGIIrradiance(vec3 p, vec3 n, vec3 toViewer)
{
    float minSpacing = min(uDDGISpacing.x, min(uDDGISpacing.y, uDDGISpacing.z));
    vec3 biased = p + (n * 0.2 + toViewer * 0.8) * (0.3 * minSpacing);
    vec3 g = clamp((biased - uDDGIOrigin) / uDDGISpacing, vec3(0.0), vec3(uDDGICounts - 1));
    ivec3 base = min(ivec3(floor(g)), max(uDDGICounts - 2, ivec3(0)));
    vec3 alpha = g - vec3(base);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 pc = min(base + off, uDDGICounts - 1);
        int index = DDGIProbeIndex(pc);
        vec3 pp = DDGIProbePosition(pc);
        vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        vec3 dirToProbe = normalize(pp - p);
        float w = (dot(dirToProbe, n) + 1.0) * 0.5;
        w = w * w + 0.2;
        vec3 probeToPoint = biased - pp;
        float dist = length(probeToPoint);
        vec2 moments = texture(uDDGIDistance, DDGITexCoord(index, probeToPoint / max(dist, 1e-4), DDGI_DISTANCE_TEXELS, uDDGIDistance)).rg;
        if (dist > moments.x)
        {
            float variance = abs(moments.y - moments.x * moments.x);
            float d = dist - moments.x;
            float cheb = variance / (variance + d * d);
            w *= max(cheb * cheb * cheb, 0.0);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / (0.2 * 0.2);
        w *= tri.x * tri.y * tri.z;
        sum += texture(uDDGIIrradiance, DDGITexCoord(index, n, DDGI_IRRADIANCE_TEXELS, uDDGIIrradiance)).rgb * w;
        wsum += w;
    }
    return wsum > 0.0 ? sum / wsum : vec3(0.0);
}

// ---- screen helpers (G-buffer passes) ----
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform sampler2D uAlbedo;
uniform mat4 uInvViewProjection;
uniform mat4 uViewProjection;
uniform vec3 uCameraPos;
uniform vec4 uViewportRect;   // x, y, width, height in full resolution pixels
uniform float uResolutionScale; // GI target resolution / full resolution

vec3 GIWorldFromPixel(vec2 pixel, float depth)
{
    vec2 local = (pixel - uViewportRect.xy) / max(uViewportRect.zw, vec2(1.0));
    vec4 w = uInvViewProjection * vec4(local * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return w.xyz / w.w;
}
// Full resolution pixel used by a GI target texel.
ivec2 GIFullPixel(vec2 fragCoord)
{
    return ivec2(floor((fragCoord - 0.5) / uResolutionScale + 0.5));
}
bool GISurface(ivec2 pixel, out vec3 p, out vec3 n, out vec3 albedo)
{
    float depth = texelFetch(uDepth, pixel, 0).r;
    vec4 nn = texelFetch(uNormal, pixel, 0);
    p = vec3(0.0); n = vec3(0.0, 1.0, 0.0); albedo = vec3(0.0);
    if (depth >= 0.99999 || dot(nn.xyz, nn.xyz) < 0.01) return false;
    n = normalize(nn.xyz);
    p = GIWorldFromPixel(vec2(pixel) + 0.5, depth);
    albedo = texelFetch(uAlbedo, pixel, 0).rgb;
    return true;
}
)GLSL";

// ---------------------------------------------------------------------------
// DDGI : probe rays (one texel per ray, one row per probe).
// ---------------------------------------------------------------------------
inline constexpr const char* kDDGITrace = R"GLSL(
out vec4 frag;
uniform int uDDGIHasHistory;
void main()
{
    int ray = int(gl_FragCoord.x);
    int probe = int(gl_FragCoord.y);
    vec3 o = DDGIProbePosition(DDGIProbeCoords(probe));
    vec3 d = DDGIRayDirection(ray);
    float t;
    int tri = GITrace(o, d, 1e6, t);
    if (tri < 0) { frag = vec4(GISky(d), 1e6); return; }
    vec3 n = GITriNormal(tri);
    if (dot(n, d) > 0.0) { frag = vec4(0.0, 0.0, 0.0, -0.2 * t); return; } // back face : probe in a wall
    vec3 p = o + d * t;
    vec3 albedo = GITriAlbedo(tri);
    vec3 radiance = GISurfaceRadiance(p, n, albedo);
    if (uDDGIHasHistory != 0) radiance += albedo * DDGIIrradiance(p, n, -d); // infinite bounces
    frag = vec4(radiance, t);
}
)GLSL";

// Irradiance (uTexels = 8) or depth moments (uTexels = 16) of the probes.
inline constexpr const char* kDDGIUpdate = R"GLSL(
out vec4 frag;
uniform sampler2D uRays;
uniform sampler2D uPrevious;
uniform int uTexels;
uniform int uDistance;
uniform float uHysteresis;
uniform float uMaxDistance;
void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    int tileSize = uTexels + 2;
    ivec2 tile = texel / tileSize;
    int probe = tile.y * uDDGITilesPerRow + tile.x;
    int probeCount = uDDGICounts.x * uDDGICounts.y * uDDGICounts.z;
    if (probe >= probeCount) { frag = vec4(0.0); return; }
    // Border texels take the direction of the mirrored interior texel.
    vec2 local = vec2(texel - tile * tileSize) - 1.0 + 0.5;
    vec2 p = local / float(uTexels) * 2.0 - 1.0;
    if (abs(p.x) > 1.0) { p.x = sign(p.x) * 2.0 - p.x; p.y = -p.y; }
    if (abs(p.y) > 1.0) { p.y = sign(p.y) * 2.0 - p.y; p.x = -p.x; }
    vec3 dir = GIOctDecode(p);
    vec4 sum = vec4(0.0);
    for (int r = 0; r < uDDGIRays; ++r)
    {
        vec4 ray = texelFetch(uRays, ivec2(r, probe), 0);
        vec3 rd = DDGIRayDirection(r);
        float c = max(0.0, dot(dir, rd));
        if (uDistance != 0)
        {
            float w = pow(c, 50.0);
            float d = min(abs(ray.a), uMaxDistance);
            sum += vec4(d * w, d * d * w, 0.0, w);
        }
        else
        {
            vec3 radiance = ray.a < 0.0 ? vec3(0.0) : ray.rgb;
            sum += vec4(radiance * c, c);
        }
    }
    vec3 value = sum.w > 1e-5 ? sum.rgb / sum.w : vec3(0.0);
    vec3 previous = texelFetch(uPrevious, texel, 0).rgb;
    if (sum.w <= 1e-5) value = previous;
    frag = vec4(mix(value, previous, uHysteresis), 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Voxel volume : direct light injection (level 0) and mip chain.
// ---------------------------------------------------------------------------
// Level 0 of the voxel volume holds six directional volumes side by side
// (region 2 * axis + (direction < 0)), each one the radiance leaving the voxel
// towards +axis or -axis : a surface only emits (and occludes) towards its own
// side, so a wall neither lights nor shadows itself.
inline constexpr const char* kVoxelInject = R"GLSL(
out vec4 frag;
uniform sampler2D uVoxelAlbedo;
uniform sampler2D uVoxelNormal;
uniform int uVoxelRes;
uniform int uVoxelTiles;
uniform int uVoxelWidth;   // width of one region
uniform vec3 uVoxelOrigin;
uniform float uVoxelSize;
uniform int uBounce; // 1 : add the light of the previous iteration (multiple bounces)
vec3 VoxelIndirect(vec3 p, vec3 n)
{
    vec3 t, b;
    GIBasis(n, t, b);
    vec3 origin = p + n * (uVoxelSize * 1.5);
    vec3 sum = VCTCone(origin, n, 0.577) * 0.25;
    for (int i = 0; i < 5; ++i)
    {
        float a = float(i) * (2.0 * GI_PI / 5.0);
        sum += VCTCone(origin, normalize(n * 0.5 + (t * cos(a) + b * sin(a)) * 0.866), 0.577) * 0.15;
    }
    return sum;
}
void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    int region = texel.x / uVoxelWidth;
    ivec2 local = ivec2(texel.x - region * uVoxelWidth, texel.y);
    ivec3 c = GIVolumeCell(local, uVoxelRes, uVoxelTiles);
    if (c.z >= uVoxelRes || region > 5) { frag = vec4(0.0); return; }
    vec4 albedo = texelFetch(uVoxelAlbedo, local, 0);
    if (albedo.a <= 0.0) { frag = vec4(0.0); return; }
    vec4 nn = texelFetch(uVoxelNormal, local, 0);
    if (nn.a <= 0.0) { frag = vec4(0.0, 0.0, 0.0, albedo.a); return; }
    vec3 n = normalize(nn.rgb * 2.0 - 1.0);
    int axis = region / 2;
    float side = (region - axis * 2) == 0 ? 1.0 : -1.0;
    // A surface is only seen (and only occludes) from its front side : light
    // flowing towards -n comes from in front of it and does not cross it
    // for a receiver on that side. This removes self occlusion of the cones.
    if (n[axis] * side <= 0.0) { frag = vec4(0.0); return; }
    vec3 p = uVoxelOrigin + (vec3(c) + 0.5) * uVoxelSize;
    vec3 radiance = albedo.rgb * GISkyAmbient();
    radiance += albedo.rgb * GIDirectIrradiance(p + n * (uVoxelSize * 0.75), n) / GI_PI;
    if (uBounce != 0) radiance += albedo.rgb * VoxelIndirect(p, n);
    frag = vec4(radiance * albedo.a, albedo.a);
}
)GLSL";

// Mip of the six directional volumes : each one is composited front to back
// along its direction (the viewer is downstream), so thin surfaces keep their
// full coverage when seen face on.
inline constexpr const char* kVoxelDownsample = R"GLSL(
out vec4 frag;
uniform sampler2D uSource;
uniform int uSourceRes;
uniform int uSourceTiles;
uniform int uSourceWidth;       // width of one region of the source
uniform int uRes;
uniform int uTiles;
uniform int uWidth;             // width of one region of the destination
void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    int region = texel.x / uWidth;
    ivec3 c = GIVolumeCell(ivec2(texel.x - region * uWidth, texel.y), uRes, uTiles);
    if (c.z >= uRes || region > 5) { frag = vec4(0.0); return; }
    int axis = region / 2;
    bool positive = (region - axis * 2) == 0;
    int offset = region * uSourceWidth;
    ivec3 along = axis == 0 ? ivec3(1, 0, 0) : axis == 1 ? ivec3(0, 1, 0) : ivec3(0, 0, 1);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 4; ++i)
    {
        ivec2 l = ivec2(i & 1, (i >> 1) & 1);
        ivec3 lateral = axis == 0 ? ivec3(0, l.x, l.y) : axis == 1 ? ivec3(l.x, 0, l.y) : ivec3(l.x, l.y, 0);
        vec4 a = GIVolumeFetchAt(uSource, c * 2 + lateral, uSourceRes, uSourceTiles, offset);
        vec4 b = GIVolumeFetchAt(uSource, c * 2 + lateral + along, uSourceRes, uSourceTiles, offset);
        vec4 front = positive ? b : a;
        vec4 back = positive ? a : b;
        sum += vec4(front.rgb + (1.0 - front.a) * back.rgb, 1.0 - (1.0 - front.a) * (1.0 - back.a));
    }
    frag = sum * 0.25;
}
)GLSL";

// Voxel volume sampling (cone tracing).
inline constexpr const char* kVoxelSampling = R"GLSL(
#define VCT_LEVELS 6
uniform sampler2D uVCTLevel0;
uniform sampler2D uVCTLevel1;
uniform sampler2D uVCTLevel2;
uniform sampler2D uVCTLevel3;
uniform sampler2D uVCTLevel4;
uniform sampler2D uVCTLevel5;
uniform int uVCTRes;
uniform int uVCTTiles[VCT_LEVELS];
uniform vec3 uVCTOrigin;
uniform float uVCTVoxelSize;
uniform float uVCTGain;
vec4 VCTSampleRegion(int level, vec3 g, int res, int tiles, int offset)
{
    if (level == 0) return GIVolumeSampleAt(uVCTLevel0, g, res, tiles, offset);
    if (level == 1) return GIVolumeSampleAt(uVCTLevel1, g, res, tiles, offset);
    if (level == 2) return GIVolumeSampleAt(uVCTLevel2, g, res, tiles, offset);
    if (level == 3) return GIVolumeSampleAt(uVCTLevel3, g, res, tiles, offset);
    if (level == 4) return GIVolumeSampleAt(uVCTLevel4, g, res, tiles, offset);
    return GIVolumeSampleAt(uVCTLevel5, g, res, tiles, offset);
}
// Light travelling back along a cone of direction dir.
vec4 VCTSampleLevel(int level, vec3 p, vec3 dir)
{
    int res = uVCTRes >> level;
    vec3 g = (p - uVCTOrigin) / (uVCTVoxelSize * float(1 << level));
    int width = res * uVCTTiles[level];
    vec3 w = dir * dir;
    vec4 r = vec4(0.0);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (w[axis] < 1e-4) continue;
        int region = axis * 2 + (dir[axis] > 0.0 ? 1 : 0); // flow = -dir
        r += VCTSampleRegion(level, g, res, uVCTTiles[level], region * width) * w[axis];
    }
    return r;
}
vec4 VCTSample(vec3 p, vec3 dir, float lod)
{
    lod = clamp(lod, 0.0, float(VCT_LEVELS - 1));
    int l0 = int(floor(lod));
    int l1 = min(l0 + 1, VCT_LEVELS - 1);
    return mix(VCTSampleLevel(l0, p, dir), VCTSampleLevel(l1, p, dir), lod - float(l0));
}
// Radiance arriving along a cone (front-to-back accumulation).
vec3 VCTCone(vec3 p, vec3 dir, float aperture)
{
    float voxel = uVCTVoxelSize;
    float maxDistance = voxel * float(uVCTRes) * 1.5;
    float dist = voxel * 1.5;
    vec3 color = vec3(0.0);
    float alpha = 0.0;
    for (int i = 0; i < 64 && dist < maxDistance && alpha < 0.97; ++i)
    {
        float diameter = max(voxel, 2.0 * aperture * dist);
        vec4 s = VCTSample(p + dir * dist, dir, log2(diameter / voxel));
        // Opacity correction : samples are spaced by half a cone diameter.
        float a = 1.0 - pow(max(1.0 - s.a, 0.0), 0.5);
        vec3 c = s.a > 1e-4 ? s.rgb / s.a * a : vec3(0.0);
        color += (1.0 - alpha) * c;
        alpha += (1.0 - alpha) * a;
        dist += diameter * 0.5;
    }
    // uVCTGain compensates the coverage lost by thin surfaces in the mips.
    return color * uVCTGain + (1.0 - alpha) * GISky(dir);
}
)GLSL";

// ---------------------------------------------------------------------------
// Light propagation volume (L1 spherical harmonics, one texture per color).
// ---------------------------------------------------------------------------
inline constexpr const char* kLPVCommon = R"GLSL(
#define SH_C0 0.282094792
#define SH_C1 0.488602512
#define SH_COS_C0 0.886226925
#define SH_COS_C1 1.023326707
vec4 SHDirection(vec3 d) { return vec4(SH_C0, -SH_C1 * d.y, SH_C1 * d.z, -SH_C1 * d.x); }
vec4 SHCosineLobe(vec3 d) { return vec4(SH_COS_C0, -SH_COS_C1 * d.y, SH_COS_C1 * d.z, -SH_COS_C1 * d.x); }
uniform sampler2D uLPVR;
uniform sampler2D uLPVG;
uniform sampler2D uLPVB;
uniform int uLPVRes;
uniform int uLPVTiles;
uniform vec3 uLPVOrigin;
uniform float uLPVCellSize;
)GLSL";

// Each surface cell is a virtual point light emitting in the hemisphere of
// its normal; it is injected in the cells in front of it (half cell shift of
// the LPV papers) so that a surface does not light itself.
inline constexpr const char* kLPVInject = R"GLSL(
layout(location = 0) out vec4 outR;
layout(location = 1) out vec4 outG;
layout(location = 2) out vec4 outB;
uniform sampler2D uRadiance;  // voxel radiance, level 1 (directional regions, one texel per LPV cell)
uniform int uRadianceWidth;
uniform sampler2D uVoxelNormal;
uniform int uVoxelRes;
uniform int uVoxelTiles;
bool CellLight(ivec3 c, out vec3 flux, out vec3 n)
{
    flux = vec3(0.0);
    n = vec3(0.0);
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(uLPVRes)))) return false;
    for (int i = 0; i < 8; ++i)
    {
        vec4 nn = GIVolumeFetch(uVoxelNormal, c * 2 + ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1), uVoxelRes, uVoxelTiles);
        if (nn.a > 0.0) n += nn.rgb * 2.0 - 1.0;
    }
    if (dot(n, n) < 1e-4) return false;
    n = normalize(n);
    vec3 w = n * n;
    for (int axis = 0; axis < 3; ++axis)
        flux += GIVolumeFetchAt(uRadiance, c, uLPVRes, uLPVTiles, (axis * 2 + (n[axis] >= 0.0 ? 0 : 1)) * uRadianceWidth).rgb * w[axis];
    return dot(flux, flux) > 0.0;
}
void main()
{
    ivec3 c = GIVolumeCell(ivec2(gl_FragCoord.xy), uLPVRes, uLPVTiles);
    outR = vec4(0.0); outG = vec4(0.0); outB = vec4(0.0);
    if (c.z >= uLPVRes) return;
    const ivec3 dirs[6] = ivec3[](ivec3(1,0,0), ivec3(-1,0,0), ivec3(0,1,0), ivec3(0,-1,0), ivec3(0,0,1), ivec3(0,0,-1));
    for (int i = 0; i < 6; ++i)
    {
        vec3 flux, n;
        if (!CellLight(c - dirs[i], flux, n)) continue;
        float w = max(0.0, dot(n, vec3(dirs[i])));
        if (w <= 0.0) continue;
        vec4 lobe = SHCosineLobe(n) / GI_PI * (w * w);
        outR += lobe * flux.r;
        outG += lobe * flux.g;
        outB += lobe * flux.b;
    }
}
)GLSL";

inline constexpr const char* kLPVPropagate = R"GLSL(
layout(location = 0) out vec4 outR;
layout(location = 1) out vec4 outG;
layout(location = 2) out vec4 outB;
uniform sampler2D uOpacity; // voxel radiance level 1 (anisotropic, alpha = opacity)
uniform int uOpacityWidth;
const float kDirectFace = 0.4006696846 / GI_PI;
const float kSideFace = 0.4234413544 / GI_PI;
void main()
{
    ivec3 c = GIVolumeCell(ivec2(gl_FragCoord.xy), uLPVRes, uLPVTiles);
    outR = vec4(0.0); outG = vec4(0.0); outB = vec4(0.0);
    if (c.z >= uLPVRes) return;
    const ivec3 dirs[6] = ivec3[](ivec3(1,0,0), ivec3(-1,0,0), ivec3(0,1,0), ivec3(0,-1,0), ivec3(0,0,1), ivec3(0,0,-1));
    for (int i = 0; i < 6; ++i)
    {
        vec3 mainDir = vec3(dirs[i]);
        ivec3 src = c - dirs[i];
        vec4 r = GIVolumeFetch(uLPVR, src, uLPVRes, uLPVTiles);
        vec4 g = GIVolumeFetch(uLPVG, src, uLPVRes, uLPVTiles);
        vec4 b = GIVolumeFetch(uLPVB, src, uLPVRes, uLPVTiles);
        // Light entering a cell that contains geometry is blocked (geometry volume).
        // Geometry between the two cells blocks the light (opacity along the axis).
        int offset = i * uOpacityWidth; // region of the propagation direction
        float blocked = GIVolumeFetchAt(uOpacity, c, uLPVRes, uLPVTiles, offset).a;
        float transmit = 1.0 - clamp(blocked, 0.0, 1.0);
        vec3 t, bt;
        GIBasis(mainDir, t, bt);
        // Face facing the source cell.
        vec4 shDir = SHDirection(mainDir);
        vec4 lobe = SHCosineLobe(mainDir);
        float f = kDirectFace * transmit;
        outR += f * max(0.0, dot(r, shDir)) * lobe;
        outG += f * max(0.0, dot(g, shDir)) * lobe;
        outB += f * max(0.0, dot(b, shDir)) * lobe;
        // Four side faces.
        for (int s = 0; s < 4; ++s)
        {
            vec2 side = s == 0 ? vec2(1.0, 0.0) : s == 1 ? vec2(0.0, 1.0) : s == 2 ? vec2(-1.0, 0.0) : vec2(0.0, -1.0);
            vec3 evalDir = normalize(mainDir * 0.894427191 + (t * side.x + bt * side.y) * 0.447213595);
            vec3 reproj = t * side.x + bt * side.y;
            vec4 shEval = SHDirection(evalDir);
            vec4 sideLobe = SHCosineLobe(reproj);
            float fs = kSideFace * transmit;
            outR += fs * max(0.0, dot(r, shEval)) * sideLobe;
            outG += fs * max(0.0, dot(g, shEval)) * sideLobe;
            outB += fs * max(0.0, dot(b, shEval)) * sideLobe;
        }
    }
}
)GLSL";

// Copies the three propagation textures (accumulated with additive blending).
inline constexpr const char* kLPVAccumulate = R"GLSL(
layout(location = 0) out vec4 outR;
layout(location = 1) out vec4 outG;
layout(location = 2) out vec4 outB;
void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    outR = texelFetch(uLPVR, texel, 0);
    outG = texelFetch(uLPVG, texel, 0);
    outB = texelFetch(uLPVB, texel, 0);
}
)GLSL";

inline constexpr const char* kLPVSampling = R"GLSL(
uniform float uLPVGain;
vec3 LPVSampleIrradiance(vec3 g, vec4 lobe)
{
    return vec3(
        dot(GIVolumeSample(uLPVR, g, uLPVRes, uLPVTiles), lobe),
        dot(GIVolumeSample(uLPVG, g, uLPVRes, uLPVTiles), lobe),
        dot(GIVolumeSample(uLPVB, g, uLPVRes, uLPVTiles), lobe));
}
vec3 LPVSampleDC(vec3 g)
{
    return vec3(GIVolumeSample(uLPVR, g, uLPVRes, uLPVTiles).x,
                GIVolumeSample(uLPVG, g, uLPVRes, uLPVTiles).x,
                GIVolumeSample(uLPVB, g, uLPVRes, uLPVTiles).x);
}
vec3 LPVIrradiance(vec3 p, vec3 n)
{
    vec3 g = (p + n * (0.5 * uLPVCellSize) - uLPVOrigin) / uLPVCellSize;
    // Light reaching the surface flows towards -n.
    vec3 e = max(LPVSampleIrradiance(g, SHCosineLobe(-n)), vec3(0.0));
    // Self illumination damping : light emitted by the surface itself fades
    // away from it, light coming from elsewhere does not.
    vec3 nearDC = max(LPVSampleDC(g), vec3(0.0));
    vec3 farDC = max(LPVSampleDC(g + n), vec3(0.0));
    vec3 damping = clamp((farDC + 1e-4) / (nearDC + 1e-4), vec3(0.0), vec3(1.0));
    return e * damping * uLPVGain;
}
)GLSL";

// ---------------------------------------------------------------------------
// Per pixel indirect lighting passes (GI target resolution).
// Output : rgb = indirect (irradiance / PI), a = 1.
// ---------------------------------------------------------------------------

// Screen space GI : rays marched against the depth buffer, radiance read
// from the lit scene color.
inline constexpr const char* kSSGI = R"GLSL(
out vec4 frag;
uniform sampler2D uSceneColor;
uniform int uRays;
uniform int uSteps;
uniform float uMaxDistance;
uniform float uThickness;
void main()
{
    ivec2 pixel = GIFullPixel(gl_FragCoord.xy);
    vec3 p, n, albedo;
    if (!GISurface(pixel, p, n, albedo)) { frag = vec4(0.0); return; }
    uint seed = GISeed(ivec2(gl_FragCoord.xy), uGIFrame, 1);
    float viewDistance = length(p - uCameraPos);
    float rayLength = max(uMaxDistance, viewDistance * 1.5);
    vec3 sum = vec3(0.0);
    for (int r = 0; r < uRays; ++r)
    {
        vec3 d = GICosineSample(n, vec2(GIRand(seed), GIRand(seed)));
        vec3 radiance = GISky(d);
        float jitter = GIRand(seed);
        vec3 origin = p + n * (0.02 * viewDistance * 0.05 + uGIBias);
        for (int s = 0; s < uSteps; ++s)
        {
            float t = rayLength * pow((float(s) + jitter) / float(uSteps), 1.6);
            vec3 q = origin + d * t;
            vec4 clip = uViewProjection * vec4(q, 1.0);
            if (clip.w <= 0.0) break;
            vec3 ndc = clip.xyz / clip.w;
            if (any(greaterThan(abs(ndc.xy), vec2(1.0)))) break;
            vec2 px = uViewportRect.xy + (ndc.xy * 0.5 + 0.5) * uViewportRect.zw;
            ivec2 ipx = ivec2(px);
            float depth = texelFetch(uDepth, ipx, 0).r;
            if (depth >= 0.99999) continue;
            vec3 sp = GIWorldFromPixel(vec2(ipx) + 0.5, depth);
            float behind = length(q - uCameraPos) - length(sp - uCameraPos);
            float thickness = uThickness + t * 0.1;
            if (behind > 0.0 && behind < thickness)
            {
                vec3 hn = texelFetch(uNormal, ipx, 0).xyz;
                if (dot(hn, hn) > 0.01 && dot(normalize(hn), d) > 0.2) { radiance = vec3(0.0); break; }
                radiance = texelFetch(uSceneColor, ipx, 0).rgb;
                break;
            }
        }
        sum += radiance;
    }
    frag = vec4(sum / float(uRays), 1.0);
}
)GLSL";

// Voxel cone tracing : 6 cones of 60 degrees around the normal.
inline constexpr const char* kVCTGather = R"GLSL(
out vec4 frag;
void main()
{
    ivec2 pixel = GIFullPixel(gl_FragCoord.xy);
    vec3 p, n, albedo;
    if (!GISurface(pixel, p, n, albedo)) { frag = vec4(0.0); return; }
    vec3 t, b;
    GIBasis(n, t, b);
    vec3 origin = p + n * (uVCTVoxelSize * 2.0);
    const float aperture = 0.577;
    vec3 sum = VCTCone(origin, n, aperture) * 0.25;
    for (int i = 0; i < 5; ++i)
    {
        float a = float(i) * (2.0 * GI_PI / 5.0);
        vec3 d = normalize(n * 0.5 + (t * cos(a) + b * sin(a)) * 0.866);
        sum += VCTCone(origin, d, aperture) * 0.15;
    }
    frag = vec4(sum, 1.0);
}
)GLSL";


// Path tracing with next event estimation. Used by HRL_GI_PATH_TRACING (4+
// bounces, progressive accumulation) and by HRL_GI_RAY_TRACING (2 bounces,
// one path per pixel and per frame, temporal reprojection + denoising).
inline constexpr const char* kPathTracing = R"GLSL(
out vec4 frag;
uniform int uMaxBounces;
uniform int uSamples;
void main()
{
    ivec2 pixel = GIFullPixel(gl_FragCoord.xy);
    vec3 p, n, albedo;
    if (!GISurface(pixel, p, n, albedo)) { frag = vec4(0.0); return; }
    uint seed = GISeed(ivec2(gl_FragCoord.xy), uGIFrame, 3);
    vec3 total = vec3(0.0);
    for (int sampleIndex = 0; sampleIndex < uSamples; ++sampleIndex)
    {
    vec3 throughput = vec3(1.0);
    vec3 result = vec3(0.0);
    vec3 origin = p + n * uGIBias;
    vec3 normal = n;
    for (int bounce = 0; bounce < uMaxBounces; ++bounce)
    {
        vec3 d = GICosineSample(normal, vec2(GIRand(seed), GIRand(seed)));
        float t;
        int tri = GITrace(origin, d, 1e6, t);
        if (tri < 0) { result += throughput * GISky(d); break; }
        vec3 hn = GITriNormal(tri);
        if (dot(hn, d) > 0.0) hn = -hn;
        vec3 hp = origin + d * t;
        vec3 a = GITriAlbedo(tri);
        result += throughput * GISurfaceRadiance(hp, hn, a);
        throughput *= a;
        if (bounce >= 1)
        {
            float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
            if (GIRand(seed) > q) break;
            throughput /= q;
        }
        origin = hp + hn * uGIBias;
        normal = hn;
    }
    total += result;
    }
    frag = vec4(total / float(uSamples), 1.0);
}
)GLSL";

// Temporal accumulation (GI target resolution).
// uMode 0 : exponential history with reprojection (SSGI, ray traced GI)
// uMode 1 : progressive average without reprojection (path tracing)
inline constexpr const char* kTemporal = R"GLSL(
out vec4 frag;
uniform sampler2D uCurrent;
uniform sampler2D uHistory;
uniform mat4 uPrevViewProjection;
uniform vec3 uPrevCameraPos;
uniform vec4 uPrevViewportRect;
uniform vec2 uTargetSize;
uniform int uMode;
uniform int uHistoryValid;
uniform float uBlend;
uniform float uSampleCount;
void main()
{
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec3 current = texelFetch(uCurrent, texel, 0).rgb;
    ivec2 pixel = GIFullPixel(gl_FragCoord.xy);
    float depth = texelFetch(uDepth, pixel, 0).r;
    if (depth >= 0.99999) { frag = vec4(current, 0.0); return; }
    vec3 p = GIWorldFromPixel(vec2(pixel) + 0.5, depth);
    float dist = length(p - uCameraPos);
    if (uMode == 1)
    {
        vec3 history = texelFetch(uHistory, texel, 0).rgb;
        frag = vec4(uHistoryValid != 0 ? (history * uSampleCount + current) / (uSampleCount + 1.0) : current, dist);
        return;
    }
    vec3 result = current;
    if (uHistoryValid != 0)
    {
        vec4 clip = uPrevViewProjection * vec4(p, 1.0);
        if (clip.w > 0.0)
        {
            vec2 ndc = clip.xy / clip.w;
            if (all(lessThanEqual(abs(ndc), vec2(1.0))))
            {
                vec2 prevPixel = uPrevViewportRect.xy + (ndc * 0.5 + 0.5) * uPrevViewportRect.zw;
                prevPixel = clamp(prevPixel, uPrevViewportRect.xy + 1.0, uPrevViewportRect.xy + uPrevViewportRect.zw - 1.0);
                vec4 history = texture(uHistory, prevPixel * uResolutionScale / uTargetSize);
                float expected = length(p - uPrevCameraPos);
                if (history.a > 0.0 && abs(history.a - expected) < 0.05 * expected + 0.02)
                    result = mix(history.rgb, current, uBlend);
            }
        }
    }
    frag = vec4(result, dist);
}
)GLSL";

// Final composite (full resolution) : scene color + albedo * indirect.
// uSource 0 : GI target texture (joint bilateral upsampling / denoise)
// uSource 1 : DDGI probes, uSource 2 : light propagation volume
inline constexpr const char* kComposite = R"GLSL(
in vec2 uv;
out vec4 frag;
uniform sampler2D uScene;
uniform sampler2D uIndirect;
uniform int uSource;
uniform int uFilterRadius;
uniform vec2 uTargetSize;
uniform float uIntensity;
uniform int uDebugIndirect;
uniform int uFogEnabled;
uniform int uFogMode;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogDensity;
float FogFactor(float dist)
{
    if (uFogEnabled == 0) return 1.0;
    float f;
    if (uFogMode == 0x0090) f = (uFogEnd - dist) / (uFogEnd - uFogStart);
    else if (uFogMode == 0x0091) f = exp(-uFogDensity * dist);
    else f = exp(-uFogDensity * uFogDensity * dist * dist);
    return clamp(f, 0.0, 1.0);
}
vec3 FilteredIndirect(ivec2 pixel, vec3 p, vec3 n)
{
    vec2 center = (vec2(pixel) + 0.5) * uResolutionScale - 0.5;
    ivec2 base = ivec2(floor(center + 0.5));
    float dist = length(p - uCameraPos);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    int radius = max(uFilterRadius, 1);
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
        {
            ivec2 t = base + ivec2(x, y);
            if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(uTargetSize)))) continue;
            ivec2 sp = ivec2(floor((vec2(t) + 0.5) / uResolutionScale));
            vec4 nn = texelFetch(uNormal, sp, 0);
            float d = texelFetch(uDepth, sp, 0).r;
            if (d >= 0.99999 || dot(nn.xyz, nn.xyz) < 0.01) continue;
            vec3 q = GIWorldFromPixel(vec2(sp) + 0.5, d);
            float wn = pow(max(dot(normalize(nn.xyz), n), 0.0), 16.0);
            float wd = exp(-abs(length(q - uCameraPos) - dist) / (0.03 * dist + 0.01));
            vec2 o = vec2(t) - center;
            float ws = exp(-dot(o, o) / (0.5 * float(radius * radius) + 0.5));
            float w = wn * wd * ws;
            sum += texelFetch(uIndirect, t, 0).rgb * w;
            wsum += w;
        }
    if (wsum < 1e-4)
        return texelFetch(uIndirect, clamp(base, ivec2(0), ivec2(uTargetSize) - 1), 0).rgb;
    return sum / wsum;
}
void main()
{
    vec2 globalUV = (uViewportRect.xy + uv * uViewportRect.zw) / vec2(textureSize(uScene, 0));
    ivec2 pixel = ivec2(uViewportRect.xy + uv * uViewportRect.zw);
    vec4 base = texelFetch(uScene, pixel, 0);
    vec3 p, n, albedo;
    if (!GISurface(pixel, p, n, albedo)) { frag = base; return; }
    vec3 indirect;
    if (uSource == 1) indirect = DDGIIrradiance(p, n, normalize(uCameraPos - p));
#ifdef HRL_GI_LPV
    else if (uSource == 2) indirect = LPVIrradiance(p, n);
#endif
    else indirect = FilteredIndirect(pixel, p, n);
    float fog = FogFactor(length(uCameraPos - p));
    if (uDebugIndirect != 0) { frag = vec4(max(indirect, vec3(0.0)), 1.0); return; }
    frag = vec4(base.rgb + albedo * max(indirect, vec3(0.0)) * uIntensity * fog, base.a);
}
)GLSL";
// Fragment source of a GI program (vertex shader : kVertex).
inline std::string AssembleFragment(const std::string& k)
{
    std::string src = kVersion;
    if (k == "composite") src += "#define HRL_GI_LPV\n";
    src += kCommon;
    if (k == "vct" || k == "voxel_inject") src += kVoxelSampling;
    if (k.rfind("lpv", 0) == 0 || k == "composite") src += kLPVCommon;
    if (k == "composite") src += kLPVSampling;
    if (k == "ddgi_trace") src += kDDGITrace;
    else if (k == "ddgi_update") src += kDDGIUpdate;
    else if (k == "voxel_inject") src += kVoxelInject;
    else if (k == "voxel_down") src += kVoxelDownsample;
    else if (k == "lpv_inject") src += kLPVInject;
    else if (k == "lpv_propagate") src += kLPVPropagate;
    else if (k == "lpv_accumulate") src += kLPVAccumulate;
    else if (k == "ssgi") src += kSSGI;
    else if (k == "vct") src += kVCTGather;
    else if (k == "rt" || k == "pt") src += kPathTracing;
    else if (k == "temporal") src += kTemporal;
    else if (k == "composite") src += kComposite;
    return src;
}
} // namespace hrl_gi_glsl

#endif
