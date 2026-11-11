#version 460
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
layout(push_constant) uniform Push { uint texture; } pc; // 0xFFFFFFFF : no texture
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId; // not written (write mask 0): particles are not pickable
void main()
{
	vec4 texel = pc.texture == 0xFFFFFFFFu ? vec4(1.0) : texture(uTextures[nonuniformEXT(pc.texture * 2u + 1u)], vUV);
	vec4 result = texel * vColor;
	if (result.a <= 0.001) discard;
	outColor = result;
	outObjectId = 0u;
}
