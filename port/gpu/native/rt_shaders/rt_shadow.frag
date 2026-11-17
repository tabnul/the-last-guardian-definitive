#version 460
#extension GL_EXT_ray_query : require
// RT sun shadow: per pixel one ray toward the sun through the geometry of the sun view (the shadow pass of the game, this frame), traced in the clip space
// of that view, where the light direction is the z axis. The pixel goes camera (x_clip, y_clip, w) -> sun view through the affine map fitted in this frame.
layout(binding = 0) uniform sampler2D depth_tex;
layout(binding = 1) uniform accelerationStructureEXT tlas;
layout(binding = 2) uniform M { vec4 row[3]; vec4 stat; } m;
layout(push_constant) uniform P {
    vec4 sun;     // x: sign of the way to the light along sun z; w: bias (times w)
    vec4 fit;     // x: A, y: B (z_clip = A * w + B), z: y flip (1 or -1), w: mode (0 multiply, 1 mask)
    vec4 misc;    // x: dark factor, yz: 1 / extent, w: geometry of this frame present
} pc;
layout(location = 0) out vec4 o;
void main() {
    const ivec2 px = ivec2(gl_FragCoord.xy * pc.misc.yz * vec2(textureSize(depth_tex, 0)));
    const float d = texelFetch(depth_tex, px, 0).r;
    float f = 1.0;
    const float w = pc.fit.y / (d - pc.fit.x);
    const bool depth_ok = w > 0.0 && w < 1e6 && d > 0.0 && d < 1.0;
    const bool map_ok = m.stat.w > 0.5 && pc.misc.w > 0.5;
    if (depth_ok && map_ok) {
        vec2 ndc = gl_FragCoord.xy * pc.misc.yz * 2.0 - 1.0;
        ndc.y *= pc.fit.z;
        const vec4 pos = vec4(ndc * w, w, 1.0);
        const vec3 sp = vec3(dot(m.row[0], pos), dot(m.row[1], pos), dot(m.row[2], pos));
        // the surface direction is taken in a version of sun space with the same scale on all axes
        const float sxy = length(m.row[0].xyz), k = sxy / max(length(m.row[2].xyz), 1e-12);
        const vec3 si = sp * vec3(1.0, 1.0, k);
        vec3 n = cross(dFdx(si), dFdy(si));
        const float nl = length(n);
        const vec3 cam = vec3(m.row[0].w, m.row[1].w, m.row[2].w * k);
        n = nl > 1e-20 ? n / nl : normalize(cam - si);
        if (dot(n, cam - si) < 0.0) {
            n = -n;
        }
        const vec3 light = vec3(0.0, 0.0, pc.sun.x);
        if (max(abs(sp.x), abs(sp.y)) > 1.0) {
            // outside the sun view (sky, far distance): no ray-traced shadow
        } else if (dot(n, light) < -0.15) {
            f = pc.misc.x; // faces away from the sun
        } else {
            const float b = pc.sun.w * w * sxy;
            vec3 org = si + n * b;
            org.z /= k;
            rayQueryEXT q;
            rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xff, org, b * 0.5 / k, light, 100.0);
            rayQueryProceedEXT(q);
            if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
                f = pc.misc.x;
            }
        }
    }
    o = vec4(f, f, f, 1.0);
    if (pc.fit.w > 0.5 && !depth_ok) {
        o = vec4(1.0, 0.0, 0.0, 1.0);
    } else if (pc.fit.w > 0.5 && !map_ok) {
        o = vec4(0.0, 0.0, 1.0, 1.0);
    }
}
