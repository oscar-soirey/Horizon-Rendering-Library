#version 450
layout(location=0) out vec2 vUV;
void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));vUV=p[gl_VertexIndex]*0.5+0.5;gl_Position=vec4(p[gl_VertexIndex],0,1);}
