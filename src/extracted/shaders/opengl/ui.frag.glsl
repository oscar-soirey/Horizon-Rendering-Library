#version 330 core

in vec2 uv;

uniform vec4 uTintColor;
uniform sampler2D uTexture;
uniform bool uSDFText;

out vec4 FragColor;

void main()
{
    vec4 texel = texture(uTexture, uv);

    if (uSDFText)
    {
        // SDF edge is encoded at 0.5. fwidth keeps the transition stable
        // when the widget is enlarged, reduced, or resized.
        float distance = texel.r;
        float smoothing = max(fwidth(distance), 0.001);
        float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance);
        FragColor = vec4(uTintColor.rgb, uTintColor.a * alpha);
    }
    else
    {
        FragColor = texel * uTintColor;
    }
}
