#version 330 core

in vec3 worldPosition;

uniform vec3 lightPosition;
uniform float farPlane;

void main()
{
    // For a point-light cube shadow map we store true per-fragment radial
    // distance. Computing this in the fragment shader is essential: interpolating
    // a per-vertex distance across a triangle does not produce the correct
    // distance to the light at each fragment.
    gl_FragDepth = clamp(length(worldPosition - lightPosition) / max(farPlane, 0.0001), 0.0, 1.0);
}
