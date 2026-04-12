//Sprite fragment shader

#version 330 core

#define MAX_LIGHTS              32

#define HRL_PointLight			(uint(0x0011))
#define HRL_DirectionalLight	(uint(0x0012))
#define HRL_SpotLight			(uint(0x0013))

out vec4 FragColor;

//on récupere les infos passées par le vertex shader
in vec3 fragPos;
in vec2 uv;

//Textures pour sprite
uniform sampler2D T_Albedo;
uniform sampler2D T_Normal;
uniform sampler2D T_Specular;
uniform sampler2D T_Roughness;
uniform sampler2D T_Metalic;
uniform sampler2D T_Alpha;

uniform vec3 TintColor;


//lights passées par ubo
struct Light
{
    uint type;
    float intensity;
    float attenuation;
    float padding1;

    vec3 position;
    float padding2;

    vec3 rotation;
    float padding3;

    vec3 color;
    float padding4;
};
//chaque lumiere = 4 * 16 octets
layout(std140) uniform LightBlock
{
    //Nombre maximal de lights passées au shader
    Light lights[MAX_LIGHTS];
};


void main()
{
    //recuperer l'albedo / diffuse / basecolor
    vec3 albedotex = texture(T_Albedo, uv).rgb;

    //recuperer la normal map et la transformer de [0;1] à [-1;1]
    vec3 normaltex = texture(T_Normal, uv).rgb;
    normaltex = normaltex * 2.0 - 1.0;

    // Atténuer la force des normales

    vec3 result = vec3(0.0);


    //parcourir les lights
    for(int i = 0; i < MAX_LIGHTS; i++)
    {
        Light light = lights[i];

        //on skip les lumieres éteintes
        if(light.intensity <= 0.0)
        {
            continue;
        }

        if(light.type == HRL_PointLight)
        {
            vec3 lightDir = light.position - fragPos;

            //on calcule la distance entre le fragment et la lumiere
            float distance = length(lightDir);

            lightDir = normalize(lightDir);

            //on calcule l'atenuation de la lumiere
            // 1/ (1 + attenuation * distance au carré)
            float attenuation = 1.0 / (1.0 + light.attenuation * distance * distance);

            //diffuse phong (lumiere qui eclaire la surface)
            float diff = max(dot(normaltex, lightDir), 0.0);
            vec3 diffuse = diff * light.color * light.intensity;

            result += (diffuse * attenuation);
        }
    }

    result *= albedotex;

    float alpha = texture(T_Albedo, uv).a;
    FragColor = vec4(result-0.8, alpha);
}