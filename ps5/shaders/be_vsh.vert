#version 450
// M9 step 5: the title's NV2A vertex program, executed on the GPU.
//
// The executor hands a whole vertex-program batch over (port/src/oogie_gpu_vsh.h,
// patch backend_gpu_vsh): the raw attribute inputs, the program words, the
// constant file and a triangle list. This shader runs that program once per
// vertex, with the semantics of the runtime's interpreter (nv2a_vsh_interp.c):
// the same field layout, the same MAC/ILU operations, R1 for an ILU paired
// with a MAC, reads before writes, and the interpreter's defaults for the
// output registers. It replaces ~35% of the draw thread's samples, which the
// profiler put inside nv2a_vsh_run.
//
// A program is small and the number of distinct programs is small, so this is
// one uber-shader over the program words rather than a generated shader per
// program: the console has no shader compiler in the title. If it proves too
// slow, generating SPIR-V on the host is the next step (M9 plan, step 5).
//
// Buffers, per batch, in the upload ring:
//   binding 5  vin[]   nverts * ninputs float4s, in attribute order
//   binding 6  vc[]    c[0..191], then fogf, anti-aliasing scale, stage scales
//   binding 7  vp[]    [0] meta (ninsn, ninputs, input_mask, -), then the words
//
// The output side is be.vert's, so be.frag and its register combiners run
// unchanged: what the program wrote to oD0/oD1/oT0..oT3, the fog factor the
// executor would have computed, and the stage-0 q.

layout(set = 0, binding = 5) readonly buffer VInputs { vec4 vin[]; };
layout(set = 0, binding = 6) readonly buffer VConsts { vec4 vc[]; };
layout(set = 0, binding = 7) readonly buffer VProg   { uvec4 vp[]; };

layout(push_constant) uniform PC {
    vec4 rt;    // 1/W, 1/H, z scale, -
    vec4 tx;    // 1/texW, 1/texH, textured, yuv
    vec4 at;    // alpha test
    vec4 tas;
} pc;

layout(location = 0) out vec4 o_c;
layout(location = 1) out vec2 o_t;
layout(location = 2) out vec4 o_s;
layout(location = 3) out vec2 o_fq;
layout(location = 4) out vec4 o_t1;
layout(location = 5) out vec4 o_t2;
layout(location = 6) out vec4 o_t3;

uint fld(uvec4 i, int w, int lo, int n)
{
    uint v = w == 0 ? i.x : (w == 1 ? i.y : (w == 2 ? i.z : i.w));
    return (v >> uint(lo)) & ((1u << uint(n)) - 1u);
}

vec4 splat4(float f) { return vec4(f, f, f, f); }

vec4 wmask(vec4 d, vec4 s, uint m)
{
    if ((m & 8u) != 0u) d.x = s.x;
    if ((m & 4u) != 0u) d.y = s.y;
    if ((m & 2u) != 0u) d.z = s.z;
    if ((m & 1u) != 0u) d.w = s.w;
    return d;
}

// One source operand (0 A, 1 B, 2 C), read exactly as read_src does.
vec4 vsrc(uvec4 ins, int which, vec4 R[12], vec4 opos, int a0,
          uint mask, uint vid, uint ninputs)
{
    uint neg, swz, mux, reg;
    vec4 s;

    if (which == 0) {
        neg = fld(ins, 1, 8, 1); swz = fld(ins, 1, 0, 8);
        mux = fld(ins, 2, 26, 2); reg = fld(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = fld(ins, 2, 25, 1); swz = fld(ins, 2, 17, 8);
        mux = fld(ins, 2, 11, 2); reg = fld(ins, 2, 13, 4);
    } else {
        neg = fld(ins, 2, 10, 1); swz = fld(ins, 2, 2, 8);
        mux = fld(ins, 3, 28, 2);
        reg = (fld(ins, 2, 0, 2) << 2) | fld(ins, 3, 30, 2);
    }
    if (mux == 1u) {
        s = reg == 12u ? opos : R[reg < 12u ? reg : 0u];
    } else if (mux == 2u) {
        // Input registers are the batch's attributes in slot order, so the
        // slot's index is how many lower bits of the mask are set.
        uint slot = fld(ins, 1, 9, 4);
        uint k = bitCount(mask & ((1u << slot) - 1u));
        s = vin[vid * ninputs + (ninputs != 0u ? k : 0u)];
    } else {
        int ci = int(fld(ins, 1, 13, 8));
        if (fld(ins, 3, 1, 1) != 0u)
            ci += a0;
        if (ci < 0 || ci >= 192)
            ci = 0;
        s = vc[ci];
    }
    s = vec4(s[int((swz >> 6) & 3u)], s[int((swz >> 4) & 3u)],
             s[int((swz >> 2) & 3u)], s[int(swz & 3u)]);
    return neg != 0u ? -s : s;
}

vec4 vmac(uint op, vec4 a, vec4 b, vec4 c, inout int a0)
{
    switch (op) {
    case 2u:  return a * b;
    case 3u:  return a + c;
    case 4u:  return a * b + c;
    case 5u:  return splat4(dot(a.xyz, b.xyz));
    case 6u:  return splat4(dot(a.xyz, b.xyz) + b.w);       /* dph */
    case 7u:  return splat4(dot(a, b));
    case 8u:  return vec4(1.0, a.y * b.y, a.z, b.w);        /* dst */
    case 9u:  return min(a, b);
    case 10u: return max(a, b);
    case 11u: return vec4(lessThan(a, b));                  /* slt */
    case 12u: return vec4(greaterThanEqual(a, b));          /* sge */
    case 13u: a0 = int(floor(a.x + 0.001)); return a;       /* arl */
    default:  return a;                                     /* mov, nop */
    }
}

vec4 vilu(uint op, vec4 c)
{
    float x = c.x;

    switch (op) {
    case 1u: return c;
    case 2u: return splat4(x == 0.0 ? uintBitsToFloat(0x7F800000u) : 1.0 / x);
    case 3u: {
        float f = 1.0 / x, af = abs(f);
        if (af < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;
        if (af > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;
        return splat4(f);
    }
    case 4u: return splat4(1.0 / sqrt(abs(x)));
    case 5u: {                                              /* exp */
        float fl = floor(x);
        return vec4(exp2(fl), x - fl, exp2(x), 1.0);
    }
    case 6u: {                                              /* log */
        float ax = abs(x);
        if (ax == 0.0)
            return vec4(-uintBitsToFloat(0x7F800000u), 1.0,
                        -uintBitsToFloat(0x7F800000u), 1.0);
        {
            float e = floor(log2(ax));
            return vec4(e, ax / exp2(e), log2(ax), 1.0);
        }
    }
    case 7u: {                                              /* lit */
        float nl = max(c.x, 0.0);
        float nh = max(c.y, 0.0);
        float p = clamp(c.w, -127.9961, 127.9961);
        float pw = nh == 0.0 ? (p == 0.0 ? 1.0
                              : (p > 0.0 ? 0.0 : uintBitsToFloat(0x7F800000u)))
                             : pow(nh, p);
        return vec4(1.0, nl, c.x > 0.0 ? pw : 0.0, 1.0);
    }
    default: return c;
    }
}

// The executor's fog_factor (nv2a_pb_exec.c), so the fog factor here is the
// one the CPU path would have put in the vertex.
float fogf(float d)
{
    vec4 f = vc[192];
    uint m;
    float px, py, r;

    if (f.x == 0.0)
        return 1.0;
    px = f.z; py = f.w;
    m = uint(f.y);
    if (m == 0x800u || m == 0x802u)             /* EXP, EXP_ABS */
        r = px + exp2(d * py * 16.0) - 1.5;
    else if (m == 0x801u || m == 0x803u)        /* EXP2, EXP2_ABS */
        r = px + exp2(-d * d * py * py * 32.0) - 1.5;
    else                                        /* LINEAR, LINEAR_ABS */
        r = px + d * py - 1.0;
    if (m == 0x802u || m == 0x803u || m == 0x804u)
        r = abs(r);
    if (isnan(r))
        r = 1.0;
    return clamp(r, 0.0, 1.0);
}

void main()
{
    uint ninsn = vp[0].x;
    uint ninputs = vp[0].y;
    uint mask = vp[0].z;
    uint vid = uint(gl_VertexIndex);
    vec4 R[12];
    vec4 o[13];
    vec4 opos = vec4(0.0);
    int a0 = 0;
    uint s;

    for (int i = 0; i < 12; i++)
        R[i] = vec4(0.0);
    for (int i = 0; i < 13; i++)
        o[i] = vec4(0.0);
    // Colours default opaque, unwritten texture coordinates (0,0,0,1): a
    // projective stage divides by q, and q = 0 would send every texel of an
    // unwritten stage to infinity.
    o[3].w = 1.0;
    o[4].w = 1.0;
    o[9].w = 1.0;
    o[10].w = 1.0;
    o[11].w = 1.0;
    o[12].w = 1.0;

    for (s = 0u; s < ninsn; s++) {
        uvec4 ins = vp[1u + s];
        uint mac = fld(ins, 1, 21, 4), ilu = fld(ins, 1, 25, 3);
        uint mac_mask = fld(ins, 3, 24, 4), ilu_mask = fld(ins, 3, 16, 4);
        uint tdst = fld(ins, 3, 20, 4);
        uint omask = fld(ins, 3, 12, 4), oaddr = fld(ins, 3, 3, 8);
        // Opcode fields are uniform across the draw. Load each source only
        // when its MAC or ILU consumes it. Both see a0 before an ARL write.
        vec4 A = mac != 0u
            ? vsrc(ins, 0, R, opos, a0, mask, vid, ninputs) : vec4(0.0);
        vec4 B = mac >= 2u && mac <= 12u && mac != 3u
            ? vsrc(ins, 1, R, opos, a0, mask, vid, ninputs) : vec4(0.0);
        vec4 C = ilu != 0u || mac == 3u || mac == 4u
            ? vsrc(ins, 2, R, opos, a0, mask, vid, ninputs) : vec4(0.0);
        vec4 M = vec4(0.0), L = vec4(0.0);

        if (mac != 0u)
            M = vmac(mac, A, B, C, a0);
        if (ilu != 0u)
            L = vilu(ilu, C);

        if (mac != 0u && mac != 13u && mac_mask != 0u) {
            if (tdst == 12u)
                opos = wmask(opos, M, mac_mask);
            else if (tdst < 12u)
                R[tdst] = wmask(R[tdst], M, mac_mask);
        }
        if (ilu != 0u && ilu_mask != 0u) {
            // Paired with a MAC the ILU can only write R1: the D3D epilogue
            // is `mul` beside `rcc R1.x`, then `mad` with R1.x.
            uint it = mac != 0u ? 1u : tdst;
            if (it == 12u)
                opos = wmask(opos, L, ilu_mask);
            else if (it < 12u)
                R[it] = wmask(R[it], L, ilu_mask);
        }
        if (omask != 0u && fld(ins, 3, 11, 1) != 0u) {
            vec4 S = fld(ins, 3, 2, 1) != 0u ? L : M;
            if (oaddr == 0u)
                opos = wmask(opos, S, omask);
            else if (oaddr < 13u)
                o[oaddr] = wmask(o[oaddr], S, omask);
        }
        if (fld(ins, 3, 0, 1) != 0u)
            break;
    }

    // The executor's hold on a program that ended on w = +0, then the
    // surface scale (s_gpu.aa_sx/sy) the CPU path applies after the program.
    vec4 P = opos;
    // Bink quads can produce +infinity from a reciprocal of zero.
    if (floatBitsToUint(P.w) == 0u || P.w >= 1e30)
        P.w = 1.0;
    P.x *= vc[193].x;
    P.y *= vc[193].y;

    // be.vert's mapping, with rhw = 1/w as the CPU path stores it.
    float w = P.w;
    float x = P.x * pc.rt.x * 2.0 - 1.0;
    if (pc.rt.w > 0.0) x = x * pc.rt.w + pc.at.w;
    float y = P.y * pc.rt.y * 2.0 - 1.0;
    float z = P.z * abs(pc.rt.z);
    gl_Position = vec4(x * w, y * w, z * w, w);

    o_c = clamp(o[3], 0.0, 1.0);
    o_s = o[4];
    o_t  = o[9].xy  * vc[194].xy;                 // ts[0]
    o_t1 = vec4(o[10].xy * vc[194].zw, o[10].zw); // ts[1]
    o_t2 = vec4(o[11].xy * vc[195].xy, o[11].zw); // ts[2]
    o_t3 = vec4(o[12].xy * vc[195].zw, o[12].zw); // ts[3]
    o_fq = vec2(fogf(o[5].x), o[9].w);
}
