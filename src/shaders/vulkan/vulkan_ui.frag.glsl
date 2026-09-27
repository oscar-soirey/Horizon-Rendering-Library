#version 450
layout(location=0) in vec2 vUV; layout(location=1) in vec4 vColor;
layout(location=0) out vec4 outColor; layout(location=1) out vec4 outBright; layout(location=2) out vec4 outPicking;
uniform sampler2D uTexture; uniform int uHasTexture; uniform int uSDF;
void main(){vec4 c=vColor;if(uHasTexture!=0){vec4 t=texture(uTexture,vUV); if(uSDF!=0){float d=t.r;float smoothing=max(fwidth(d),0.001);c.a*=smoothstep(0.5-smoothing,0.5+smoothing,d);} else c*=t;} if(c.a<0.001)discard;outColor=c;outBright=vec4(0);outPicking=vec4(0);}
