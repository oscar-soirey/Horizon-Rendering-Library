//common.hrls
%{
    typedef struct { float x; float y; }             float2;
    typedef struct { float x; float y; float z; }    float3;
    typedef struct { float x; float y; float z; float w; } float4;
    typedef struct { float4 a; float4 b; float4 c; float4 d; } mat4; // column major
}



//vertex.hrls
#define HRL_VERSION "0.3"
#include "common.hrls"

buffer {
    float2 apos;
    float2 auv;
}
uniforms {
    mat4 model;
    mat4 proj;
    mat4 view;
}
out {
    float2 o_uv;
}
void main()
{
    o_uv = auv;
    float4 pos       = float4(apos.x, apos.y, 0.0, 1.0);
    float4 transform = proj * view * model * pos;
    HRL_VERTEX_LOCATION(transform.x, transform.y, transform.z);
}


//fragment.hrls
#define HRL_VERSION "0.3"
#include "common.hrls"

in {
    float2 i_uv;
}
uniforms {
    float3 u_tint = {1.f, 1.f, 1.f};
    sampler2D u_texture;
}
void main()
{
    float4 color = HRL_SAMPLE(u_texture, i_uv);
    HRL_FRAG_COLOR(color * float4(u_tint.x, u_tint.y, u_tint.z, 1.0));
}