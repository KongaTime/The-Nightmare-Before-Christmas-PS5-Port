#version 450
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Parameters { vec2 size; float gamma; float warmth; float coolReduction; } parameters;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec2 texel = clamp(uv * parameters.size, vec2(0.5), parameters.size - 0.5);
    vec3 rgb = texture(picture, texel / vec2(textureSize(picture, 0))).rgb;
    rgb = pow(rgb, vec3(parameters.gamma));
    // Shift yellow-orange toward red. Neutral and cool colors receive no shift.
    float chroma = max(min(rgb.r, rgb.g) - rgb.b, 0.0);
    float warmHue = smoothstep(-0.02, 0.02, rgb.r - rgb.g);
    rgb.g -= parameters.warmth * warmHue * chroma;
    // Reduce excess blue in muted cool tones while preserving saturated accents.
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    float low = min(rgb.r, min(rgb.g, rgb.b));
    float saturation = (peak - low) / max(peak, 0.001);
    float muted = 1.0 - smoothstep(0.18, 0.55, saturation);
    float blueExcess = max(rgb.b - max(rgb.r, rgb.g), 0.0);
    float coolHue = smoothstep(0.015, 0.15, blueExcess / max(peak, 0.001));
    float shift = parameters.coolReduction * muted * coolHue * rgb.b;
    rgb.b -= shift;
    rgb.r += shift * (0.0722 / 0.2126); // Preserve Rec. 709 luma.
    color = vec4(rgb, 1.0);
}
