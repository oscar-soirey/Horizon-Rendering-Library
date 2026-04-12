#version 330 core

layout(location = 0) out vec4 FragColor;    //COLOR_ATTACHMENT0
layout(location = 1) out vec4 BrightColor;  //COLOR_ATTACHMENT1

#define MAX_LIGHTS              32

#define HRL_PointLight          (uint(0x0011))
#define HRL_DirectionalLight    (uint(0x0012))
#define HRL_SpotLight           (uint(0x0013))

// ── Fog modes ─────────────────────────────────────────────────────────────────
const int FOG_LINEAR = 0x0090; // transition linéaire entre FogStart et FogEnd
const int FOG_EXP    = 0x0091; // exponentiel doux,  contrôlé par FogDensity
const int FOG_EXP2   = 0x0092; // exponentiel carré, plus réaliste

in vec3 fragPos;
in vec2 uv;

uniform sampler2D T_Albedo;
uniform sampler2D T_Normal;
uniform sampler2D T_Specular;
uniform sampler2D T_Roughness;
uniform sampler2D T_Metalic;
uniform sampler2D T_Alpha;

uniform vec3 TintColor;
uniform vec3 CamPos;

uniform float BrightThreshold;

// ── Fog uniforms (passer a un UBO) ──────────────────────────────────────────────────────────────
uniform int   FogEnabled;   // active / désactive le fog
uniform int   FogMode;      // FOG_LINEAR, FOG_EXP ou FOG_EXP2
uniform vec4  FogColor;     // couleur + alpha du brouillard
uniform float FogStart;     // distance de début  (LINEAR uniquement)
uniform float FogEnd;       // distance de fin    (LINEAR uniquement)
uniform float FogDensity;   // densité            (EXP / EXP2 uniquement)

struct Light
{
    uint  type;
    float intensity;
    float attenuation;
    float padding1;

    vec3  position;
    float padding2;

    vec3  rotation;
    float padding3;

    vec3  color;
    float padding4;
};

//passer un uniform lightCount pour eviter de faire 32 passes à chaque fois
layout(std140) uniform LightBlock
{
    Light lights[MAX_LIGHTS];
};

// ── Fog : calcule le facteur de mélange [0=fog total .. 1=couleur originale] ──
//   dist     : distance caméra → fragment
//   Retourne : fogFactor clamped [0, 1]
float computeFogFactor(float dist)
{
    float f;

    if (FogMode == FOG_LINEAR)
    f = (FogEnd - dist) / (FogEnd - FogStart);
    else if (FogMode == FOG_EXP)
    f = exp(-FogDensity * dist);
    else // FOG_EXP2
    f = exp(-FogDensity * FogDensity * dist * dist);

    return clamp(f, 0.0, 1.0);
}

// ── Fog : applique le fog sur une couleur déjà calculée ──────────────────────
//   litColor : couleur éclairée du fragment
//   dist     : distance caméra → fragment
//   Retourne : couleur finale mixée avec FogColor
vec4 applyFog(vec4 litColor, float dist)
{
    if (FogEnabled == 0)
    return litColor;

    float fogFactor = computeFogFactor(dist);

    // mix(a, b, t) → t=0 : 100% fog / t=1 : 100% couleur originale
    return mix(FogColor, litColor, fogFactor);
}


void main()
{
    // ── Textures ──────────────────────────────────────────────────────────────
    vec3  albedo    = texture(T_Albedo,    uv).rgb;
    float metallic  = texture(T_Metalic,   uv).r;
    float roughness = clamp(texture(T_Roughness, uv).r, 0.05, 1.0);
    float specMap   = texture(T_Specular,  uv).r;

    vec3  normalTex = texture(T_Normal, uv).rgb * 2.0 - 1.0;

    // ── PBR simplifié (Blinn-Phong paramétré) ─────────────────────────────────
    // F0 : diélectrique = 0.04, métal = couleur albedo
    vec3  F0        = mix(vec3(0.04), albedo, metallic);

    // roughness → shininess  (plus lisse = highlight plus net)
    float shininess = pow(2.0, (1.0 - roughness) * 11.0);   // [2, 2048]

    vec3  viewDir   = normalize(CamPos - fragPos);

    vec3  result    = vec3(0.0);

    // ── Lights ────────────────────────────────────────────────────────────────
    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        Light light = lights[i];

        if (light.intensity <= 0.0)
        continue;

        if (light.type == HRL_PointLight)
        {
            vec3  toLight     = light.position - fragPos;
            float distance    = length(toLight);
            vec3  lightDir    = toLight / distance;

            float attenuation = 1.0 / (1.0 + light.attenuation * distance * distance);

            // Diffuse — réduit sur les surfaces métalliques
            float NdotL  = max(dot(normalTex, lightDir), 0.0);
            vec3  kD     = (1.0 - metallic) * NdotL * light.color * light.intensity;

            // Spéculaire Blinn-Phong
            vec3  halfDir = normalize(lightDir + viewDir);
            float NdotH   = max(dot(normalTex, halfDir), 0.0);
            float spec    = pow(NdotH, shininess) * specMap;
            vec3  kS      = spec * F0 * light.color * light.intensity;

            result += (kD + kS) * attenuation;
        }
    }

    // ── Composition finale ────────────────────────────────────────────────────
    result *= albedo * TintColor;

    float alpha = texture(T_Albedo, uv).a
    * texture(T_Alpha,  uv).r;

    // ── Fog ───────────────────────────────────────────────────────────────────
    // distance calculée ici pour ne pas la recalculer dans applyFog
    float dist = length(CamPos - fragPos);


    vec4 color = applyFog(vec4(result, alpha), dist);
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));

    FragColor = color;  //complete scene

    if (brightness > BrightThreshold)
        BrightColor = color;
    else
        BrightColor = vec4(0.0, 0.0, 0.0, 1.0);
}