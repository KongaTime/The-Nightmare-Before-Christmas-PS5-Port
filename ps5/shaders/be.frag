#version 450
// M9 render back end: the pixel side.
//
// RC_USE == 0: texture stage 0 x diffuse, alpha test, and Bink's YUV frames
// (d3d11_backend.c `ps`). RC_USE == 1: the NV2A register combiners -- the
// rules of d3d11_backend.c's rc_generate (itself after the runtime's
// nv2a_combiner.c and xemu's psh.c). The program's words are specialization
// constants, so each pipeline compiles to that program's straight-line code,
// as the PC build's generated shaders are, without a shader compiler in the
// title. Oogie stores colours at half intensity and doubles them in the
// combiners (KNOWN_ISSUES #36): without them things draw dark.
//
// Registers: 0 zero 1 c0 2 c1 3 fog 4 v0 5 v1 8-11 t0-t3 12 r0 13 r1
// 14 v1+r0 (final) 15 E*F (final). An input byte is reg | alpha<<4 | map<<5.

layout(constant_id = 0) const uint RC_USE = 0u;
layout(constant_id = 1) const uint RC_CONTROL = 0u;
layout(constant_id = 2) const uint RC_SP = 0u;
layout(constant_id = 3) const uint RC_F0 = 0u;
layout(constant_id = 4) const uint RC_F1 = 0u;
// per stage: colour ICW, alpha ICW, colour OCW, alpha OCW
layout(constant_id = 5)  const uint W0 = 0u;  layout(constant_id = 6)  const uint W1 = 0u;
layout(constant_id = 7)  const uint W2 = 0u;  layout(constant_id = 8)  const uint W3 = 0u;
layout(constant_id = 9)  const uint W4 = 0u;  layout(constant_id = 10) const uint W5 = 0u;
layout(constant_id = 11) const uint W6 = 0u;  layout(constant_id = 12) const uint W7 = 0u;
layout(constant_id = 13) const uint W8 = 0u;  layout(constant_id = 14) const uint W9 = 0u;
layout(constant_id = 15) const uint W10 = 0u; layout(constant_id = 16) const uint W11 = 0u;
layout(constant_id = 17) const uint W12 = 0u; layout(constant_id = 18) const uint W13 = 0u;
layout(constant_id = 19) const uint W14 = 0u; layout(constant_id = 20) const uint W15 = 0u;
layout(constant_id = 21) const uint W16 = 0u; layout(constant_id = 22) const uint W17 = 0u;
layout(constant_id = 23) const uint W18 = 0u; layout(constant_id = 24) const uint W19 = 0u;
layout(constant_id = 25) const uint W20 = 0u; layout(constant_id = 26) const uint W21 = 0u;
layout(constant_id = 27) const uint W22 = 0u; layout(constant_id = 28) const uint W23 = 0u;
layout(constant_id = 29) const uint W24 = 0u; layout(constant_id = 30) const uint W25 = 0u;
layout(constant_id = 31) const uint W26 = 0u; layout(constant_id = 32) const uint W27 = 0u;
layout(constant_id = 33) const uint W28 = 0u; layout(constant_id = 34) const uint W29 = 0u;
layout(constant_id = 35) const uint W30 = 0u; layout(constant_id = 36) const uint W31 = 0u;

layout(location = 0) in vec4 i_c;
layout(location = 1) in vec2 i_t;
layout(location = 2) in vec4 i_s;
layout(location = 3) in vec2 i_fq;
layout(location = 4) in vec4 i_t1;
layout(location = 5) in vec4 i_t2;
layout(location = 6) in vec4 i_t3;

layout(set = 0, binding = 0) uniform sampler2D T0;
layout(set = 0, binding = 1) uniform sampler2D T1;
layout(set = 0, binding = 2) uniform sampler2D T2;
layout(set = 0, binding = 3) uniform sampler2D T3;
layout(set = 0, binding = 4) uniform RC {
    vec4 k0[8];
    vec4 k1[8];
    vec4 fk0, fk1, fogc;
} rc;

layout(push_constant) uniform PC {
    vec4 rt;
    vec4 tx;
    vec4 at;
    vec4 tas;
} pc;

layout(location = 0) out vec4 o;

vec4 atest(vec4 c)
{
    if (pc.at.x > 0.5) {
        float a = floor(c.a * 255.0 + 0.5);
        float r = pc.at.z;
        int f = int(pc.at.y);
        bool ok = true;
        if (f == 0) ok = false;
        else if (f == 1) ok = a < r;
        else if (f == 2) ok = a == r;
        else if (f == 3) ok = a <= r;
        else if (f == 4) ok = a > r;
        else if (f == 5) ok = a != r;
        else if (f == 6) ok = a >= r;
        if (!ok) discard;
    }
    return c;
}

vec4 R[16];

vec3 map3(vec3 s, uint m)
{
    if (m == 0u) return max(s, 0.0);
    if (m == 1u) return 1.0 - clamp(s, 0.0, 1.0);
    if (m == 2u) return 2.0 * max(s, 0.0) - 1.0;
    if (m == 3u) return 1.0 - 2.0 * max(s, 0.0);
    if (m == 4u) return max(s, 0.0) - 0.5;
    if (m == 5u) return 0.5 - max(s, 0.0);
    if (m == 6u) return s;
    return -s;
}

float map1(float s, uint m)
{
    return map3(vec3(s), m).x;
}

vec3 in_rgb(uint b)
{
    vec4 r = R[b & 15u];
    return map3((b & 16u) != 0u ? r.aaa : r.rgb, (b >> 5) & 7u);
}

float in_a(uint b)
{
    vec4 r = R[b & 15u];
    return map1((b & 16u) != 0u ? r.a : r.b, (b >> 5) & 7u);
}

vec3 oscale3(vec3 x, uint m)
{
    if (m == 0x08u) return x - 0.5;
    if (m == 0x10u) return x * 2.0;
    if (m == 0x18u) return (x - 0.5) * 2.0;
    if (m == 0x20u) return x * 4.0;
    if (m == 0x30u) return x * 0.5;
    return x;
}

float oscale1(float x, uint m)
{
    return oscale3(vec3(x), m).x;
}

vec4 ptex(sampler2D s, vec4 c)
{
    float q = abs(c.w) > 1e-20 ? c.w : 1.0;
    return texture(s, c.xy / q);
}

vec4 stage_tex(uint st, uint mode, vec4 crd)
{
    if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    if (mode == 4u) return clamp(crd, 0.0, 1.0);
    if (mode >= 1u && mode <= 3u) {            // cube approximated as 2D
        vec4 t;
        if (st == 0u) t = ptex(T0, crd);
        else if (st == 1u) t = ptex(T1, crd);
        else if (st == 2u) t = ptex(T2, crd);
        else t = ptex(T3, crd);
        t.a = pc.tas[st] < 0.0 ? 1.0 : clamp(t.a * pc.tas[st], 0.0, 1.0);
        return t;
    }
    return vec4(0.0);                          // bump-env, dot product: zero
}

void main()
{
    if (RC_USE == 0u) {
        vec4 c = i_c;
        if (pc.tx.w > 0.5) {
            // Bink YUV: Y on stage 0, the chroma planes on 1 and 2 (BT.601).
            float y = 1.164 * (texture(T0, i_t).r - 0.0627);
            float cr = texture(T1, i_t).r - 0.502, cb = texture(T2, i_t).r - 0.502;
            if (pc.tx.w > 1.5) { float sw = cr; cr = cb; cb = sw; }
            o = vec4(clamp(y + 1.596 * cr, 0.0, 1.0), clamp(y - 0.813 * cr - 0.391 * cb, 0.0, 1.0),
                     clamp(y + 2.018 * cb, 0.0, 1.0), 1.0);
            return;
        }
        if (pc.tx.z > 0.5) {
            vec4 s0 = texture(T0, i_t);
            s0.a = pc.tas.x < 0.0 ? 1.0 : clamp(s0.a * pc.tas.x, 0.0, 1.0);
            c *= s0;
        }
        o = atest(clamp(c, 0.0, 1.0));
        return;
    }

    const uint WORDS[32] = uint[](W0, W1, W2, W3, W4, W5, W6, W7, W8, W9, W10, W11, W12, W13, W14,
                                  W15, W16, W17, W18, W19, W20, W21, W22, W23, W24, W25, W26, W27,
                                  W28, W29, W30, W31);
    const uint N = RC_CONTROL & 0xFFu;      // the loop stops at 8 either way
    const uint FL = RC_CONTROL >> 8;

    for (int i = 0; i < 16; i++) R[i] = vec4(0.0);
    R[1] = rc.k0[0];
    R[2] = rc.k1[0];
    R[3] = vec4(rc.fogc.rgb, i_fq.x);
    R[4] = i_c;
    R[5] = i_s;
    R[8] = stage_tex(0u, RC_SP & 31u, vec4(i_t, 0.0, i_fq.y));
    R[9] = stage_tex(1u, (RC_SP >> 5) & 31u, i_t1);
    R[10] = stage_tex(2u, (RC_SP >> 10) & 31u, i_t2);
    R[11] = stage_tex(3u, (RC_SP >> 15) & 31u, i_t3);
    // r0.a starts as texture 0's alpha, or 1 with stage 0 off.
    R[12] = vec4(0.0, 0.0, 0.0, (RC_SP & 31u) != 0u ? R[8].a : 1.0);

    for (uint st = 0u; st < 8u; st++) {
        if (st >= N) break;
        uint icw = WORDS[st * 4u + 0u], aicw = WORDS[st * 4u + 1u];
        uint ocw = WORDS[st * 4u + 2u], aocw = WORDS[st * 4u + 3u];
        uint f = ocw >> 12, af = aocw >> 12, d;
        R[1] = rc.k0[(FL & 0x010u) != 0u ? st : 0u];
        R[2] = rc.k1[(FL & 0x100u) != 0u ? st : 0u];
        bool mux = (FL & 1u) != 0u ? R[12].a >= 0.5 : ((uint(R[12].a * 255.0)) & 1u) != 0u;

        vec3 A = in_rgb(icw >> 24), B = in_rgb((icw >> 16) & 255u);
        vec3 C = in_rgb((icw >> 8) & 255u), D = in_rgb(icw & 255u);
        vec3 pab = (f & 2u) != 0u ? vec3(dot(A, B)) : A * B;
        vec3 pcd = (f & 1u) != 0u ? vec3(dot(C, D)) : C * D;
        vec3 sm = (f & 4u) != 0u ? (mux ? pcd : pab) : pab + pcd;

        float a = in_a(aicw >> 24), b = in_a((aicw >> 16) & 255u);
        float c = in_a((aicw >> 8) & 255u), dd = in_a(aicw & 255u);
        float apab = a * b, apcd = c * dd;
        float asm1 = (af & 4u) != 0u ? (mux ? apcd : apab) : apab + apcd;

        vec3 ab = clamp(oscale3(pab, f & 0x38u), -1.0, 1.0);
        vec3 cd = clamp(oscale3(pcd, f & 0x38u), -1.0, 1.0);
        vec3 ms = clamp(oscale3(sm, f & 0x38u), -1.0, 1.0);
        float aab = clamp(oscale1(apab, af & 0x38u), -1.0, 1.0);
        float acd = clamp(oscale1(apcd, af & 0x38u), -1.0, 1.0);
        float ams = clamp(oscale1(asm1, af & 0x38u), -1.0, 1.0);

        // All reads happened above; now the writes, in the C evaluator's order.
        d = (ocw >> 4) & 15u;
        if (d != 0u) { R[d].rgb = ab; if ((f & 0x80u) != 0u) R[d].a = ab.z; }
        d = ocw & 15u;
        if (d != 0u) { R[d].rgb = cd; if ((f & 0x40u) != 0u) R[d].a = cd.z; }
        d = (ocw >> 8) & 15u;
        if (d != 0u) R[d].rgb = ms;
        d = (aocw >> 4) & 15u;
        if (d != 0u) R[d].a = aab;
        d = aocw & 15u;
        if (d != 0u) R[d].a = acd;
        d = (aocw >> 8) & 15u;
        if (d != 0u) R[d].a = ams;
        R[0] = vec4(0.0);
    }

    if (RC_F0 != 0u || RC_F1 != 0u) {
        uint ff = RC_F1 & 0xFFu;
        R[1] = rc.fk0;
        R[2] = rc.fk1;
        vec3 va = (ff & 0x40u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;
        vec3 vb = (ff & 0x20u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;
        R[14] = vec4((ff & 0x80u) != 0u ? clamp(va + vb, 0.0, 1.0) : va + vb, 0.0);
        vec3 E = in_rgb(RC_F1 >> 24), F = in_rgb((RC_F1 >> 16) & 255u);
        R[15] = vec4(E * F, 0.0);
        vec3 A = in_rgb(RC_F0 >> 24), B = in_rgb((RC_F0 >> 16) & 255u);
        vec3 C = in_rgb((RC_F0 >> 8) & 255u), D = in_rgb(RC_F0 & 255u);
        float Ea = in_a((RC_F1 >> 8) & 255u);
        o = atest(clamp(vec4(D + C * (1.0 - A) + B * A, Ea), 0.0, 1.0));
    } else {
        o = atest(clamp(R[12], 0.0, 1.0));
    }
}
