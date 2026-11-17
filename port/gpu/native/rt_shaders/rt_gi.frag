#version 460
#extension GL_EXT_ray_query : require
// RT ambient shading (first half of GI): per pixel a few short rays over the hemisphere of the surface, through the geometry the camera drew this frame.
// The scene is in camera clip space before the divide (x_clip, y_clip, z_clip), which is an affine image of view space: view = (x_clip / fx, y_clip / fy, w),
// z_clip = A * w + B. Directions are made in view space (true angles) and carried over to clip space for the trace.
layout(binding = 0) uniform sampler2D depth_tex;
layout(binding = 1) uniform accelerationStructureEXT tlas;
layout(binding = 3) uniform sampler2D color_tex; // the frame's picture: the colour a ray's hit point sends back
layout(push_constant) uniform P {
    vec4 gi;      // x: radius (view units), y: strength, z: rays per pixel, w: bias
    vec4 fit;     // x: A, y: B (z_clip = A * w + B), z: y flip (1 or -1), w: mode (0 multiply, 1 check view)
    vec4 misc;    // x: frame number, yz: 1 / extent, w: geometry of this frame present
    vec4 lens;    // x: fx, y: fy
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
    const bool scene_ok = pc.misc.w > 0.5;
    float f = 1.0;
    vec3 bounce = vec3(0.0);
    if (depth_ok && scene_ok) {
        const vec3 v = View(gl_FragCoord.xy, w);
        // surface direction from the depth neighbours, per axis the neighbour closest in depth (so edges do not bend it)
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
        const vec3 t1 = normalize(abs(n.z) < 0.9 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0))), t2 = cross(n, t1);
        const uint rays = uint(clamp(pc.gi.z, 1.0, 16.0));
        const float radius = pc.gi.x, bias = pc.gi.w * (1.0 + w * 0.2);
        const vec3 org_v = v + n * bias;
        const vec3 org = vec3(org_v.x * pc.lens.x, org_v.y * pc.lens.y, pc.fit.x * org_v.z + pc.fit.y);
        const float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + pc.misc.x * vec2(5.588238, 3.1415), vec2(0.06711056, 0.00583715))));
        float shade = 0.0;
        for (uint i = 0; i < rays; ++i) {
            // cosine-weighted directions, a golden-angle spiral turned by the pixel's noise
            const float u = (float(i) + fract(noise * 7.31 + 0.5)) / float(rays), a = 6.2831853 * (noise + float(i) * 0.618034);
            const float r = sqrt(u), h = sqrt(1.0 - u);
            const vec3 dir_v = t1 * (r * cos(a)) + t2 * (r * sin(a)) + n * h;
            const vec3 dir = vec3(dir_v.x * pc.lens.x, dir_v.y * pc.lens.y, pc.fit.x * dir_v.z);
            rayQueryEXT q;
            rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xff, org, 0.0, dir, radius);
            rayQueryProceedEXT(q);
            if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
                const float tt = rayQueryGetIntersectionTEXT(q, true), t = tt / radius, fall = (1.0 - t) * (1.0 - t);
                shade += fall;
                // bounce: where the hit point sits on screen, and only when that pixel really is the hit surface
                const vec3 hc = org + dir * tt;
                const float hw = (hc.z - pc.fit.y) / pc.fit.x;
                if (hw > 0.05) {
                    vec2 hn = hc.xy / hw;
                    hn.y *= pc.fit.z;
                    const vec2 uv = hn * 0.5 + 0.5;
                    if (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
                        const float sw = W(ivec2(uv * vec2(textureSize(depth_tex, 0))));
                        if (abs(sw - hw) < 0.1 * hw + 0.05) {
                            bounce += texture(color_tex, uv).rgb * fall;
                        }
                    }
                }
            }
        }
        f = 1.0 - pc.gi.y * shade / float(rays);
        bounce /= float(rays);
    }
    o = vec4(bounce, f);
    if (pc.fit.w > 0.5 && !depth_ok) {
        o = vec4(1.0, 0.0, 0.0, 1.0);
    } else if (pc.fit.w > 0.5 && !scene_ok) {
        o = vec4(0.0, 0.0, 1.0, 1.0);
    }
}
