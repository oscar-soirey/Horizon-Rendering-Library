/**
 * Ce shader ne supporte pas les caracteres non - ASCII, donc il ne faut pas utiliser les accents,
 * meme dans les commentaires
 */

#version 330 core

//vertex/point
layout(location = 0) in vec2 apos;
layout(location = 1) in vec2 auv;

//entite/objet
uniform mat4 model;

//vue/ecran
uniform mat4 view;
uniform mat4 projection;

out vec3 fragPos;
out vec2 uv;

void main()
{
    vec4 pos = vec4(apos, 0.0, 1.0);

    //on passe les coordonees fragment et uv au fragment shader
    fragPos = vec3(model * pos);
    uv = auv;

    //garder cet ordre
    gl_Position = projection * view * model * pos;
}