#version 330 core

layout(location = 0) in vec3 aPosition;

uniform mat4 projection;
uniform mat4 view;
uniform mat4 rotation;

out vec3 skyDirection;

void main()
{
    vec3 direction = mat3(rotation) * aPosition;
    skyDirection = normalize(direction);
    gl_Position = projection * view * rotation * vec4(aPosition, 1.0);
}
