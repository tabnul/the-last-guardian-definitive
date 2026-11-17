#version 460
#extension GL_EXT_ray_query : require
// RT sun beams: per pixel a walk along the view line from the camera to the surface; at every step one ray toward the sun tells whether that bit of air is
// in sunlight. Lit air sends light to the camera (most when looking toward the sun). Scene and spaces as in rt_gi.frag.
layout(binding = 0) uniform sampler2D depth_tex;
layout(binding = 1) uniform accelerationStructureEXT tlas;
layout(push_constant) uniform P {
    vec4 sun;     // xyz: direction to the sun in view space, w: steps
    vec4 fit;     // x: A, y: B (z_clip = A * w + B), z: y flip, w: furthest distance
    vec4 misc;    // x: frame number, yz: 1 / extent, w: strength
    vec4 lens;    // x: fx, y: fy, z: forward share (g), w: density
} pc;
layout(location = 0) out vec4 o;
void main() {
    const vec2 scale = pc.misc.yz * vec2(textureSize(depth_tex, 0));
    const float d = texelFetch(depth_tex, ivec2(gl_FragCoord.xy * scale), 0).r;
    float w = pc.fit.y / (d - pc.fit.x);
    if (!(w > 0.0 && w < 1e6 && d > 0.0 && d < 1.0)) {
        w = pc.fit.w;
    }
    w = min(w, pc.fit.w);
    vec2 ndc = gl_FragCoord.xy * pc.misc.yz * 2.0 - 1.0;
    ndc.y *= pc.fit.z;
    const vec3 v = vec3(ndc.x / pc.lens.x, ndc.y / pc.lens.y, 1.0); // the point at depth t is v * t
    const vec3 L = normalize(pc.sun.xyz);
    const vec3 dir = vec3(L.x * pc.lens.x, L.y * pc.lens.y, pc.fit.x * L.z);
    const uint steps = uint(clamp(pc.sun.w, 4.0, 128.0));
    const float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + pc.misc.x * vec2(5.588238, 3.1415), vec2(0.06711056, 0.00583715))));
    float lit = 0.0;
    for (uint i = 0; i < steps; ++i) {
        const float t = max(w * (float(i) + noise) / float(steps), 0.15);
        const vec3 p = v * t;
        const vec3 org = vec3(p.x * pc.lens.x, p.y * pc.lens.y, pc.fit.x * p.z + pc.fit.y);
        rayQueryEXT q;
        rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xff, org, 0.0, dir, 5000.0);
        rayQueryProceedEXT(q);
        if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
            lit += 1.0;
        }
    }
    const float len = lit / float(steps) * w * length(v);
    const float g = pc.lens.z, c = dot(normalize(v), L);
    const float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * c, 1.5) * (1.0 - g) * (1.0 - g) / (1.0 + g); // 1 toward the sun, weak sideways
    const float a = (1.0 - exp(-pc.lens.w * len)) * phase * pc.misc.w;
    o = vec4(clamp(vec3(1.0, 0.9, 0.72) * a, 0.0, 1.0), 1.0);
}
