#version 450
layout(location=0) in vec3 aPosition;
layout(location=5) in uvec4 aBoneIndices;
layout(location=6) in vec4 aBoneWeights;
layout(location=0) out vec3 vWorldPos;
uniform mat4 lightSpaceMatrix;
uniform mat4 model;
uniform mat4 boneMatrices[128];
void main(){
    mat4 skin=mat4(0.0);
    for(int i=0;i<4;++i) skin += boneMatrices[aBoneIndices[i]]*aBoneWeights[i];
    if(aBoneWeights.x+aBoneWeights.y+aBoneWeights.z+aBoneWeights.w<=0.0) skin=mat4(1.0);
    vec4 w=model*(skin*vec4(aPosition,1.0));
    vWorldPos=w.xyz;
    gl_Position=lightSpaceMatrix*w;
}
