#version 460
// RT ambient shading, second pass: the raw per-pixel result is noisy (a few rays per pixel); it is averaged over the neighbours that lie on the same
// surface (close in depth), and the result goes onto the picture.
layout(binding = 0) uniform sampler2D depth_tex;
layout(binding = 3) uniform sampler2D ao_tex;
layout(push_constant) uniform P {
    vec4 gi;
    vec4 fit;     // x: A, y: B (z_clip = A * w + B), w: mode (0 multiply, 1 check view)
    vec4 misc;    // yz: 1 / extent, w: geometry present
    vec4 lens;    // z: unused here, w: blur reach in pixels
} pc;
layout(location = 0) out vec4 o;
float W(ivec2 p) {
    const float d = texelFetch(depth_tex, p, 0).r;
    return pc.fit.y / (d - pc.fit.x);
}
void main() {
    const vec2 scale = pc.misc.yz * vec2(textureSize(depth_tex, 0));
    const vec2 ao_scale = pc.misc.yz * vec2(textureSize(ao_tex, 0)); // the ray picture can be smaller than this one
    const ivec2 here = ivec2(gl_FragCoord.xy * ao_scale), top = textureSize(ao_tex, 0) - 1;
    const float d = texelFetch(depth_tex, ivec2(gl_FragCoord.xy * scale), 0).r;
    const float w = pc.fit.y / (d - pc.fit.x);
    const bool depth_ok = w > 0.0 && w < 1e6 && d > 0.0 && d < 1.0;
    vec4 f = vec4(0.0, 0.0, 0.0, 1.0);
    if (depth_ok) {
        const int reach = int(clamp(pc.lens.w, 0.0, 6.0));
        vec4 sum = vec4(0.0);
        float weight = 0.0;
        for (int y = -reach; y <= reach; ++y) {
            for (int x = -reach; x <= reach; ++x) {
                const ivec2 q = clamp(here + ivec2(x, y), ivec2(0), top);
                const float wq = W(ivec2((vec2(q) + 0.5) / ao_scale * scale));
                const float k = exp(-abs(wq - w) / (0.03 * w)) + 1e-4;
                sum += texelFetch(ao_tex, q, 0) * k;
                weight += k;
            }
        }
        f = sum / weight;
    }
    if (pc.fit.w > 1.5) { // the sun beams: light that is added to the picture
        if (!depth_ok) { // the sky
            f = texelFetch(ao_tex, clamp(here, ivec2(0), top), 0);
        }
        o = vec4(f.rgb, 1.0);
        return;
    }
    // shade plus bounce light; the picture is multiplied by twice this value (so it can also get brighter)
    const vec3 m = vec3(f.a) + pc.lens.z * f.rgb;
    o = vec4(pc.fit.w > 0.5 ? m : m * 0.5, 1.0);
    if (pc.fit.w > 0.5 && !depth_ok) {
        o = vec4(1.0, 0.0, 0.0, 1.0);
    } else if (pc.fit.w > 0.5 && pc.misc.w < 0.5) {
        o = vec4(0.0, 0.0, 1.0, 1.0);
    }
}
