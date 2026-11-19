#version 450
// M9 render back end: the executor's screen-space vertices to clip space.
// Port of Project Oogie's d3d11_backend.c `vs` (s_hlsl): x, y in surface
// pixels, z as the title produced it (scaled by pc.rt.z), rhw = 1/w kept
// homogeneous so interpolation stays perspective-correct. The second stream
// (OogieVertexExtra, port/patches/backend_vertex_extra_*) carries what the
// register combiners read: specular, fog factor, stage-0 q, stages 1-3.
layout(location = 0) in vec4 in_p;      // x, y, z, rhw
layout(location = 1) in vec4 in_c;      // diffuse (B8G8R8A8_UNORM -> rgba)
layout(location = 2) in vec2 in_t;      // texels
layout(location = 3) in vec4 in_s;      // specular
layout(location = 4) in vec2 in_fq;     // fog factor, stage-0 q
layout(location = 5) in vec4 in_t1;
layout(location = 6) in vec4 in_t2;
layout(location = 7) in vec4 in_t3;

layout(push_constant) uniform PC {
    vec4 rt;    // 1/W, 1/H, z scale, -
    vec4 tx;    // 1/texW, 1/texH, textured, yuv (1, 2 = swapped chroma)
    vec4 at;    // alpha test enable, func (0..7), ref (0..255), -
    vec4 tas;   // per stage: sampled alpha times this
} pc;

layout(location = 0) out vec4 o_c;
layout(location = 1) out vec2 o_t;
layout(location = 2) out vec4 o_s;
layout(location = 3) out vec2 o_fq;
layout(location = 4) out vec4 o_t1;
layout(location = 5) out vec4 o_t2;
layout(location = 6) out vec4 o_t3;

void main()
{
    float w = in_p.w > 1e-30 ? 1.0 / in_p.w : 1.0;
    float x = in_p.x * pc.rt.x * 2.0 - 1.0;
    float y = in_p.y * pc.rt.y * 2.0 - 1.0;     // Vulkan clip space is y-down
    float z = in_p.z * abs(pc.rt.z);
    gl_Position = vec4(x * w, y * w, z * w, w);
    o_c = in_c;
    o_t = in_t * pc.tx.xy;
    o_s = in_s;
    o_fq = in_fq;
    o_t1 = in_t1;
    o_t2 = in_t2;
    o_t3 = in_t3;
}
