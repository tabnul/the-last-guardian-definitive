#version 460
#extension GL_EXT_ray_query : require
// RT sun shadow, drawn right after the game's own sun shadow step into the same picture (green channel = lit share). Per pixel a bundle of rays toward the
// sun through the geometry the camera drew this frame (clip space before the divide, see rt_gi.frag); the bundle's width gives edges that are sharp at the
// contact point and softer further away.
layout(binding = 0) uniform sampler2D depth_tex;
layout(binding = 1) uniform accelerationStructureEXT tlas;
layout(push_constant) uniform P {
    vec4 sun;     // xyz: direction to the sun in view space (x right, y as clip y, z = w), w: bias
    vec4 fit;     // x: A, y: B (z_clip = A * w + B), z: y flip (1 or -1), w: cone radius (tangent)
    vec4 misc;    // x: frame number, yz: 1 / extent, w: rays per pixel
    vec4 lens;    // x: fx, y: fy, z: check view (1: red no depth)
} pc;
layout(location = 0) out vec4 o;
float W(ivec2 p) {
    const float d = texelFetch(depth_tex, p, 0).r;
    return pc.fit.y / (d - pc.fit.x);
}
vec3 View(vec2 frag, float w) {
    vec2 ndc = frag * pc.misc.yz * 2.0 - 1.0;
    ndc.y *= pc.fit.z;
    return vec3(ndc.x * w / pc.lens.x, ndc.y * w / pc.lens.y, w);
}
void main() {
    const vec2 scale = pc.misc.yz * vec2(textureSize(depth_tex, 0));
    const ivec2 px = ivec2(gl_FragCoord.xy * scale);
    const float d = texelFetch(depth_tex, px, 0).r;
    const float w = pc.fit.y / (d - pc.fit.x);
    const bool depth_ok = w > 0.0 && w < 1e6 && d > 0.0 && d < 1.0;
    float lit = 1.0;
    if (depth_ok) {
        const vec3 v = View(gl_FragCoord.xy, w);
        const vec2 step = 1.0 / scale;
        const float wl = W(px - ivec2(1, 0)), wr = W(px + ivec2(1, 0)), wu = W(px - ivec2(0, 1)), wd = W(px + ivec2(0, 1));
        const vec3 dx = abs(wl - w) < abs(wr - w) ? v - View(gl_FragCoord.xy - vec2(step.x, 0.0), wl) : View(gl_FragCoord.xy + vec2(step.x, 0.0), wr) - v;
        const vec3 dy = abs(wu - w) < abs(wd - w) ? v - View(gl_FragCoord.xy - vec2(0.0, step.y), wu) : View(gl_FragCoord.xy + vec2(0.0, step.y), wd) - v;
        vec3 n = cross(dx, dy);
        const float nl = length(n);
        n = nl > 1e-20 ? n / nl : -normalize(v);
        if (dot(n, v) > 0.0) {
            n = -n;
        }
        const vec3 L = normalize(pc.sun.xyz);
        const vec3 t1 = normalize(abs(L.z) < 0.9 ? cross(L, vec3(0.0, 0.0, 1.0)) : cross(L, vec3(1.0, 0.0, 0.0))), t2 = cross(L, t1);
        const uint rays = uint(clamp(pc.misc.w, 1.0, 64.0));
        const float bias = pc.sun.w * (1.0 + w * 0.2);
        const vec3 org_v = v + n * bias + L * bias;
        const vec3 org = vec3(org_v.x * pc.lens.x, org_v.y * pc.lens.y, pc.fit.x * org_v.z + pc.fit.y);
        const float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + pc.misc.x * vec2(5.588238, 3.1415), vec2(0.06711056, 0.00583715))));
        float shade = 0.0;
        for (uint i = 0; i < rays; ++i) {
            const float u = fract(0.5 + float(i) * 0.7548777), a = 6.2831853 * (noise + float(i) * 0.618034);
            const float r = sqrt(u) * pc.fit.w;
            const vec3 dir_v = normalize(L + t1 * (r * cos(a)) + t2 * (r * sin(a)));
            const vec3 dir = vec3(dir_v.x * pc.lens.x, dir_v.y * pc.lens.y, pc.fit.x * dir_v.z);
            rayQueryEXT q;
            rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | (pc.lens.z > 0.999 ? gl_RayFlagsOpaqueEXT : 0u), 0xff, org, 0.0, dir, 5000.0);
            uint guard = 0u; // at most a few cards per ray are looked at; after that the ray counts as blocked (keeps the cost bounded)
            // cut-out surfaces (leaves, grass: drawn as flat cards with holes) stop the ray only in a share of small cells on the card: light patches between
            // the leaves. pc.lens.z = the share that blocks (1 = solid card, 0 = no shadow), pc.lens.w = cells per card edge
            while (rayQueryProceedEXT(q)) {
                if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
                    const vec2 cell = floor(rayQueryGetIntersectionBarycentricsEXT(q, false) * pc.lens.w);
                    const float prim = float(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
                    if (guard++ >= 6u || fract(sin(dot(cell + prim * 0.37, vec2(12.9898, 78.233))) * 43758.5453) < pc.lens.z) {
                        rayQueryConfirmIntersectionEXT(q);
                    }
                }
            }
            if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
                shade += 1.0;
            }
            // the first few rays agree (all free or all blocked): no shadow edge here, the rest is not needed
            if (i == 3u && rays > 4u && (shade == 0.0 || shade == 4.0)) {
                shade *= float(rays) / 4.0;
                break;
            }
        }
        lit = 1.0 - shade / float(rays);
    }
    o = vec4(0.0, lit, 0.0, 1.0);
}
