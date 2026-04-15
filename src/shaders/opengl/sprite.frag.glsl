#version 330 core

// ─────────────────────────────────────────────────────────────────────────────
//  HRL — Fragment shader principal
//  Sorties :
//    • FragColor   (COLOR_ATTACHMENT0) — scène complète
//    • BrightColor (COLOR_ATTACHMENT1) — zones brillantes pour le bloom
// ─────────────────────────────────────────────────────────────────────────────

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 BrightColor;
layout(location = 2) out vec4 ColorPickingBuffer;   //writes the ID of the sprite using the color

// ─────────────────────────────────────────────────────────────────────────────
//  Constantes
// ─────────────────────────────────────────────────────────────────────────────

#define MAX_LIGHTS 32

// Types de lumières
#define HRL_PointLight       (uint(0x0011))
#define HRL_DirectionalLight (uint(0x0012))
#define HRL_SpotLight        (uint(0x0013))

// Modes de brouillard
const int FOG_LINEAR = 0x0090; // interpolation linéaire entre FogStart et FogEnd
const int FOG_EXP    = 0x0091; // décroissance exponentielle douce (FogDensity)
const int FOG_EXP2   = 0x0092; // décroissance gaussienne, plus réaliste

// ─────────────────────────────────────────────────────────────────────────────
//  Entrées interpolées
// ─────────────────────────────────────────────────────────────────────────────

in vec3 fragPos;   // position du fragment en world-space
in vec2 uv;        // coordonnées de texture
flat in uint sprite_id; //id du sprite, utile pour le color picking buffer

// ─────────────────────────────────────────────────────────────────────────────
//  Textures PBR
// ─────────────────────────────────────────────────────────────────────────────

uniform sampler2D T_Albedo;     // couleur de base (rgb) + opacité (a)
uniform sampler2D T_Normal;     // normal map (tangent-space, encodée [0,1])
uniform sampler2D T_Specular;   // intensité spéculaire par texel
uniform sampler2D T_Roughness;  // rugosité de surface [0=lisse, 1=mat]
uniform sampler2D T_Metallic;   // metalness  [0=diélectrique, 1=métal]
uniform sampler2D T_Alpha;      // masque d'opacité supplémentaire

// ─────────────────────────────────────────────────────────────────────────────
//  Uniforms généraux
// ─────────────────────────────────────────────────────────────────────────────

uniform vec3  TintColor;        // teinte multiplicative globale
uniform vec3  CamPos;           // position de la caméra en world-space
uniform float BrightThreshold;  // luminance seuil pour l'extraction bloom

// ─────────────────────────────────────────────────────────────────────────────
//  Uniforms brouillard  (TODO : migrer vers un UBO FogBlock)
// ─────────────────────────────────────────────────────────────────────────────

uniform int   FogEnabled;  // 0 = désactivé, 1 = activé
uniform int   FogMode;     // FOG_LINEAR | FOG_EXP | FOG_EXP2
uniform vec4  FogColor;    // couleur + alpha du brouillard
uniform float FogStart;    // distance de début (LINEAR uniquement)
uniform float FogEnd;      // distance de fin   (LINEAR uniquement)
uniform float FogDensity;  // densité           (EXP / EXP2 uniquement)

// ─────────────────────────────────────────────────────────────────────────────
//  Structure Light
//
//  Champs réutilisés selon le type :
//
//  PointLight
//    position   → origine de la lumière
//    attenuation→ coefficient quadratique  (1 / (1 + att * d²))
//
//  DirectionalLight
//    rotation   → direction du rayon lumineux (world-space, normalisée)
//                 lightDir = -rotation  (on remonte vers la source)
//    position / attenuation ignorés
//
//  SpotLight
//    position   → origine du spot
//    rotation   → direction principale du cône (normalisée, world-space)
//    attenuation→ coefficient quadratique de distance
//    padding1   → cos(innerCutoff) : angle intérieur (plein éclairage)
//    padding2   → cos(outerCutoff) : angle extérieur (extinction totale)
//                 interpolation douce entre les deux via smoothstep
// ─────────────────────────────────────────────────────────────────────────────

struct Light
{
    uint  type;
    float intensity;
    float attenuation;
    float innerCutoff;  // (anciennement padding1) cos(angle intérieur) — SpotLight

    vec3  position;
    float outerCutoff;  // (anciennement padding2) cos(angle extérieur) — SpotLight

    vec3  rotation;     // direction (DirectionalLight / SpotLight)
    float padding3;

    vec3  color;
    float padding4;
};

layout(std140) uniform LightBlock
{
    Light lights[MAX_LIGHTS];
};

// ─────────────────────────────────────────────────────────────────────────────
//  Brouillard
// ─────────────────────────────────────────────────────────────────────────────

// Retourne le facteur de mélange [0 = brouillard total, 1 = couleur originale].
float computeFogFactor(float dist)
{
    float f;
    if      (FogMode == FOG_LINEAR) f = (FogEnd - dist) / (FogEnd - FogStart);
    else if (FogMode == FOG_EXP)    f = exp(-FogDensity * dist);
    else /* FOG_EXP2 */             f = exp(-FogDensity * FogDensity * dist * dist);
    return clamp(f, 0.0, 1.0);
}

// Applique le brouillard sur litColor en fonction de la distance caméra→fragment.
vec4 applyFog(vec4 litColor, float dist)
{
    if (FogEnabled == 0)
    return litColor;

    float fogFactor = computeFogFactor(dist);
    // t=0 → 100 % brouillard | t=1 → 100 % couleur originale
    return mix(FogColor, litColor, fogFactor);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Calcul PBR simplifié (Blinn-Phong paramétré)
//
//  Données communes pré-calculées, passées en paramètre pour éviter de
//  rééchantillonner les textures à chaque itération de lumière.
// ─────────────────────────────────────────────────────────────────────────────

// Retourne la contribution (diffuse + spéculaire) d'une lumière directionnelle.
//   lightDir  : vecteur unitaire allant du fragment vers la source
//   normalTex : normale interpolée (tangent-space décodée)
//   viewDir   : vecteur unitaire allant du fragment vers la caméra
//   F0        : reflectance à incidence nulle
//   shininess : exposant Blinn-Phong
//   specMap   : masque spéculaire du texel
//   lightColor: couleur * intensité (pré-multiplié par l'appelant)
vec3 evalBRDF(vec3 lightDir,
              vec3 normalTex, vec3 viewDir,
              vec3 F0, float shininess, float specMap,
              float metallic, vec3 lightColor)
{
    // Diffuse — atténuée sur les surfaces métalliques (conservation énergie approx.)
    float NdotL = max(dot(normalTex, lightDir), 0.0);
    vec3  kD    = (1.0 - metallic) * NdotL * lightColor;

    // Spéculaire Blinn-Phong
    vec3  halfDir = normalize(lightDir + viewDir);
    float NdotH   = max(dot(normalTex, halfDir), 0.0);
    float spec    = pow(NdotH, shininess) * specMap;
    vec3  kS      = spec * F0 * lightColor;

    return kD + kS;
}



//DEBUG
vec3 DebugColorFromID(uint id)
{
    return vec3(
    fract(float(id) * 0.618),
    fract(float(id) * 0.381),
    fract(float(id) * 0.173)
    );
}
uint hash(uint x)
{
    x ^= x >> 16;
    x *= uint(0x7feb352d);
    x ^= x >> 15;
    x *= uint(0x846ca68b);
    x ^= x >> 16;
    return x;
}
vec3 ColorFromID(uint id)
{
    uint h = hash(id);

    float r = float((h      ) & 255u);
    float g = float((h >> 8 ) & 255u);
    float b = float((h >> 16) & 255u);

    vec3 c = vec3(r, g, b) / 255.0;

    // boost contraste (gamma simple)
    c = pow(c, vec3(0.75));

    return c;
}



// ─────────────────────────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────────────────────────

void main()
{
    // ── Échantillonnage des textures ──────────────────────────────────────────
    vec3  albedo    = texture(T_Albedo,    uv).rgb;
    float metallic  = texture(T_Metallic,  uv).r;
    float roughness = clamp(texture(T_Roughness, uv).r, 0.05, 1.0);
    float specMap   = texture(T_Specular,  uv).r;
    vec3  normalTex = texture(T_Normal,    uv).rgb * 2.0 - 1.0; // [0,1] → [-1,1]

    // ── Paramètres PBR dérivés ────────────────────────────────────────────────
    // F0 : diélectrique ≈ 0.04, métal ≈ albedo
    vec3  F0        = mix(vec3(0.04), albedo, metallic);
    // roughness → shininess : plus la surface est lisse, plus le lobe est étroit
    float shininess = pow(2.0, (1.0 - roughness) * 11.0); // [2, 2048]

    vec3 viewDir = normalize(CamPos - fragPos);
    vec3 result  = vec3(0.0);

    // ── Boucle d'éclairage ────────────────────────────────────────────────────
    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        Light light = lights[i];
        if (light.intensity <= 0.0)
        continue; // slot vide, on skip

        vec3 lightColor = light.color * light.intensity;

        // ── Point Light ───────────────────────────────────────────────────────
        // Lumière omnidirectionnelle émise depuis light.position.
        // L'atténuation quadratique simule la loi de l'inverse du carré.
        if (light.type == HRL_PointLight)
        {
            vec3  toLight     = light.position - fragPos;
            float dist        = length(toLight);
            vec3  lightDir    = toLight / dist; // normalisé sans recalculer length()

            float attenuation = 1.0 / (1.0 + light.attenuation * dist * dist);

            result += evalBRDF(lightDir, normalTex, viewDir,
                               F0, shininess, specMap, metallic,
                               lightColor) * attenuation;
        }

        // ── Directional Light ─────────────────────────────────────────────────
        // Lumière infiniment distante (soleil, lune…).
        // La direction est uniforme sur toute la scène : light.rotation contient
        // le vecteur allant DE la source VERS la scène, on l'inverse pour obtenir
        // lightDir (fragment → source).
        // Pas d'atténuation — la source est à distance infinie.
        else if (light.type == HRL_DirectionalLight)
        {
            vec3 lightDir = normalize(-light.rotation);

            result += evalBRDF(lightDir, normalTex, viewDir,
                               F0, shininess, specMap, metallic,
                               lightColor);
        }

        // ── Spot Light ────────────────────────────────────────────────────────
        // Lumière conique positionnée dans la scène.
        // Le cône est défini par deux angles (stockés sous forme de cosinus) :
        //   innerCutoff : au-dedans → intensité maximale
        //   outerCutoff : au-dehors → intensité nulle
        // smoothstep assure une transition douce entre les deux bornes,
        // évitant le bord dur caractéristique du spot "sans pénombre".
        else if (light.type == HRL_SpotLight)
        {
            vec3  toLight     = light.position - fragPos;
            float dist        = length(toLight);
            vec3  lightDir    = toLight / dist;

            // Angle entre la direction principale du spot et le rayon fragment→source
            float cosTheta = dot(lightDir, normalize(-light.rotation));

            // Facteur de pénombre : 0 hors cône, 1 dans le cône intérieur
            float spotFactor = smoothstep(light.outerCutoff,
                                          light.innerCutoff,
                                          cosTheta);

            // Pas la peine d'évaluer le BRDF si le fragment est hors cône
            if (spotFactor <= 0.0)
            continue;

            float attenuation = 1.0 / (1.0 + light.attenuation * dist * dist);

            result += evalBRDF(lightDir, normalTex, viewDir,
                               F0, shininess, specMap, metallic,
                               lightColor) * attenuation * spotFactor;
        }
    }

    // ── Composition finale ────────────────────────────────────────────────────
    result *= albedo * TintColor;

    float alpha = texture(T_Albedo, uv).a
    * texture(T_Alpha,  uv).r;

    // ── Brouillard ────────────────────────────────────────────────────────────
    float dist  = length(CamPos - fragPos);
    vec4  color = applyFog(vec4(result, alpha), dist);

    // ── Sorties ───────────────────────────────────────────────────────────────
    FragColor = color;

    // Extraction des zones lumineuses pour le bloom (COLOR_ATTACHMENT1)
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722)); // luminance Rec.709
    BrightColor = (brightness > BrightThreshold)
    ? color
    : vec4(0.0, 0.0, 0.0, 1.0);


    //Color picking buffer
    ColorPickingBuffer = vec4(
        float((sprite_id >> 16u) & 255u) / 255.0,
        float((sprite_id >> 8u)  & 255u) / 255.0,
        float(sprite_id & 255u) / 255.0,
        alpha
    );
}