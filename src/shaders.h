// GLSL for the renderer. kVsWorld/kFsWorld replace the game's fixed-function pipeline during the
// world pass; everything else is a full-screen pass drawing one triangle.
#pragma once

// ================================================================ Phase 3: world material shader
// Replaces fixed-function drawing for every world draw. Output 0 reproduces the game's colour
// exactly (texture env + alpha test); outputs 1-3 carry what the lighting needs.
static const char* kVsWorld = R"(#version 330 compatibility
out vec2 vTex;
out vec4 vCol;
flat out vec4 vColFlat;
void main() {
    gl_Position = ftransform();
    vTex = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
    vCol = gl_Color;
    vColFlat = gl_Color;
}
)";

static const char* kFsWorld = R"(#version 330 compatibility
in vec2 vTex;
in vec4 vCol;
flat in vec4 vColFlat;
layout(location = 0) out vec4 oScene;
layout(location = 1) out vec4 oEmis;     // rgb emission, a playfield coverage
layout(location = 2) out vec4 oSolid;    // a solid coverage
layout(location = 3) out vec4 oNormal;   // rg screen-space normal xy (0.5 = flat), b specular mask
uniform sampler2D uTex;
uniform ivec4 uMode;   // x texture env (0 off,1 modulate,2 replace,3 add,4 decal), y flat shading,
                       // z alpha test func (0 off, else GL_NEVER..GL_ALWAYS - 0x200 + 1), w alpha-only texture
uniform ivec4 uClass;  // x playfield, y solid, z emitter, w write normals
uniform vec4 uAlpha;   // x alpha ref, y emission weight (soft glow sprites light less)
uniform vec4 uMat;     // bevel strength, bevel radius (screen px), detail strength, specular
uniform vec4 uFx;      // laser glow (0 = off), time, front surface (rain and snow land on it), full coverage (terrain, back wall)
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
bool alphaPass(float a, float r) {
    int f = uMode.z;
    return f == 1 ? false : f == 2 ? a < r : f == 3 ? a == r : f == 4 ? a <= r :
           f == 5 ? a > r : f == 6 ? a != r : f == 7 ? a >= r : true;
}
vec4 tap(vec2 uv) {
    vec4 t = texture(uTex, uv);
    if (uMode.w != 0) t.rgb = vec3(1.0);
    return t;
}
void main() {
    vec4 col = uMode.y != 0 ? vColFlat : vCol;
    vec4 t = vec4(1.0);
    vec4 c = col;
    if (uMode.x != 0) {
        t = tap(vTex);
        if (uMode.x == 1) c = t * col;
        else if (uMode.x == 2) c = uMode.w != 0 ? vec4(col.rgb, t.a) : t;
        else if (uMode.x == 3) c = vec4(col.rgb + t.rgb, col.a * t.a);
        else c = vec4(mix(col.rgb, t.rgb, t.a), col.a);
    }
    if (uMode.z != 0 && !alphaPass(c.a, uAlpha.x)) discard;
    if (uFx.x > 0.0 && uMode.x != 0) {
        // lasers and beams: a white-hot core, energy flowing along the beam, a richer glow
        float lum = max(c.r, max(c.g, c.b));
        float core = smoothstep(0.55, 0.97, lum * max(t.a, 0.3));
        float flow = 0.78 + 0.22 * sin((vTex.x + vTex.y) * 70.0 - uFx.y * 28.0) * sin((vTex.x - vTex.y) * 29.0 + uFx.y * 10.0);
        vec3 hue = c.rgb / max(lum, 1e-3);
        c.rgb = mix(c.rgb * flow, mix(hue, vec3(1.0), 0.75), core * 0.85 * min(uFx.x, 1.0)) * (1.0 + 0.35 * uFx.x);
    }
    oScene = c;

    bool pf = uClass.x != 0;
    oEmis = uClass.z != 0 ? vec4(c.rgb * uAlpha.y, c.a) : vec4(0.0, 0.0, 0.0, pf ? c.a : 0.0);
    // a = solid coverage (light), r = front surfaces only (not the fort's back wall): rain and snow
    // terrain (display lists) counts as fully solid wherever it is drawn, whatever its texture alpha
    oSolid = (pf && uClass.y != 0 && uClass.z == 0) ? vec4(uFx.z, 0.0, 0.0, uFx.w > 0.5 ? step(0.05, c.a) : c.a) : vec4(0.0);
    oNormal = vec4(0.5, 0.5, 0.0, 0.0);
    if (uClass.w != 0 && uMode.x != 0) {
        // Height = soft bevel from the sprite's alpha + gentle relief from its painted luminance.
        vec2 ts = 1.0 / vec2(textureSize(uTex, 0));
        float uvPerPx = max(length(dFdx(vTex)), length(dFdy(vTex)));     // so the bevel is uMat.y screen px wide
        float r = max(uMat.y * uvPerPx, 1.5 * max(ts.x, ts.y));
        vec2 rx = vec2(r, 0.0), ry = vec2(0.0, r);
        vec2 dx = vec2(ts.x * 1.5, 0.0), dy = vec2(0.0, ts.y * 1.5);
        float hx = (tap(vTex + rx).a - tap(vTex - rx).a) * uMat.x
                 + (luma(tap(vTex + dx).rgb) - luma(tap(vTex - dx).rgb)) * uMat.z;
        float hy = (tap(vTex + ry).a - tap(vTex - ry).a) * uMat.x
                 + (luma(tap(vTex + dy).rgb) - luma(tap(vTex - dy).rgb)) * uMat.z;
        vec3 nt = normalize(vec3(-hx, -hy, 1.0));
        // texture-space -> screen-space through the inverse UV Jacobian (handles rotation and flips)
        mat2 J = mat2(dFdx(vTex), dFdy(vTex));
        float det = determinant(J);
        vec2 n2 = vec2(0.0);
        if (abs(det) > 1e-12) {
            mat2 Ji = inverse(J);
            vec2 su = Ji * vec2(1.0, 0.0), sv = Ji * vec2(0.0, 1.0);
            float lu = length(su), lv = length(sv);
            if (lu > 0.0 && lv > 0.0) n2 = nt.x * su / lu + nt.y * sv / lv;
        }
        vec3 n = normalize(vec3(n2, nt.z));
        float sat = max(t.r, max(t.g, t.b)) - min(t.r, min(t.g, t.b));
        float tl = luma(t.rgb);   // metal = grey mid-tones; near-white paint (bone, highlights) is not metal
        float spec = (1.0 - sat) * (1.0 - sat) * smoothstep(0.15, 0.4, tl) * (1.0 - smoothstep(0.62, 0.88, tl)) * uMat.w;
        oNormal = vec4(n.xy * 0.5 + 0.5, spec, c.a);
    }
}
)";

static const char* kVsFullscreen = R"(#version 330
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Shared helpers prepended to every fragment shader.
static const char* kFsCommon = R"(#version 330
in vec2 vUV;
layout(location = 0) out vec4 o;
vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 toGamma(vec3 c)  { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
float luma(vec3 c)    { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of Duty")
vec3 down13(sampler2D t, vec2 uv, vec2 px, bool lin) {
    vec3 s[13];
    vec2 offs[13] = vec2[](vec2(-2, 2), vec2(0, 2), vec2(2, 2), vec2(-2, 0), vec2(0, 0), vec2(2, 0),
                           vec2(-2, -2), vec2(0, -2), vec2(2, -2), vec2(-1, 1), vec2(1, 1), vec2(-1, -1), vec2(1, -1));
    for (int i = 0; i < 13; i++) {
        vec3 c = texture(t, uv + offs[i] * px).rgb;
        s[i] = lin ? toLinear(c) : c;
    }
    return s[4] * 0.125 + (s[0] + s[2] + s[6] + s[8]) * 0.03125
         + (s[1] + s[3] + s[5] + s[7]) * 0.0625 + (s[9] + s[10] + s[11] + s[12]) * 0.125;
}
)";

// Full-res scene + emissive -> half-res linear bloom source.
static const char* kFsPrefilter = R"(
uniform sampler2D uEmis;
uniform sampler2D uScene;
uniform vec2 uTexel;       // full-res texel
uniform vec4 uP;           // emissive strength, scene strength, scene threshold, soft knee
void main() {
    vec3 e = min(down13(uEmis, vUV, uTexel, true), vec3(16.0)) * uP.x;
    vec3 s = down13(uScene, vUV, uTexel, true);
    float br = max(s.r, max(s.g, s.b));
    float soft = clamp(br - uP.z + uP.w, 0.0, 2.0 * uP.w);
    soft = soft * soft / (4.0 * uP.w + 1e-5);
    float w = max(soft, br - uP.z) / max(br, 1e-5);
    o = vec4(e + s * w * uP.y, 1.0);
}
)";

static const char* kFsDown = R"(
uniform sampler2D uSrc;
uniform vec2 uTexel;       // source texel
void main() { o = vec4(down13(uSrc, vUV, uTexel, false), 1.0); }
)";

// 3x3 tent upsample, added onto the next larger level with ONE/ONE blending.
static const char* kFsUp = R"(
uniform sampler2D uSrc;
uniform vec2 uTexel;       // source texel * radius
uniform float uGain;       // bloom spread: how much each wider level adds
void main() {
    vec3 c = texture(uSrc, vUV).rgb * 4.0;
    c += (texture(uSrc, vUV + vec2(uTexel.x, 0)).rgb + texture(uSrc, vUV - vec2(uTexel.x, 0)).rgb
        + texture(uSrc, vUV + vec2(0, uTexel.y)).rgb + texture(uSrc, vUV - vec2(0, uTexel.y)).rgb) * 2.0;
    c += texture(uSrc, vUV + uTexel).rgb + texture(uSrc, vUV - uTexel).rgb
       + texture(uSrc, vUV + vec2(uTexel.x, -uTexel.y)).rgb + texture(uSrc, vUV + vec2(-uTexel.x, uTexel.y)).rgb;
    o = vec4(c / 16.0 * uGain, 1.0);
}
)";

// How much of the screen glows (the coarsest bloom level, a 4x4 average), smoothed over time: rises in
// ~0.15 s, falls in ~0.8 s, so auto-dim follows a battle without pumping on every blast.
static const char* kFsBloomAvg = R"(
uniform sampler2D uSrc;    // coarsest bloom level
uniform sampler2D uPrev;   // last frame's average (1x1)
uniform vec2 uK;           // blend factor rising, falling
void main() {
    float a = 0.0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) a += luma(texture(uSrc, (vec2(x, y) + 0.5) / 4.0).rgb);
    a /= 16.0;
    float p = texture(uPrev, vec2(0.5)).r;
    o = vec4(mix(p, a, a > p ? uK.x : uK.y), 0.0, 0.0, 1.0);
}
)";

// ================================================================ Phase 2: lighting
// Working-resolution inputs. World targets: emissive (rgb) + playfield coverage (a), occluder coverage (a).
// Output: rgb = linear emission (scaled), a = 1 where rays stop (solid or emitting).
// The working grid is anchored to the world: cell j covers screen pixels [j*p - o, (j+1)*p - o),
// so panning moves content by whole cells instead of sliding it across them.
static const char* kFsGiPrep = R"(
uniform sampler2D uEmis;
uniform sampler2D uOcc;
uniform vec2 uFull;        // full-res size
uniform vec4 uGrid;        // cell size p (screen px), grid offset x, y
uniform vec2 uE;           // emission scale, emitter threshold
uniform int uLightCount;
uniform vec4 uLights[16];  // working-res x, y, radius, intensity
uniform vec4 uLightCol[16];
void main() {
    vec2 center = gl_FragCoord.xy * uGrid.x - uGrid.yz;
    vec3 e = vec3(0.0);
    float solid = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 off = vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0) * (uGrid.x * 0.25);
        vec2 uv = (center + off) / uFull;
        e += toLinear(texture(uEmis, uv).rgb);
        solid = max(solid, texture(uOcc, uv).a);
    }
    e = min(e * 0.25, vec3(8.0)) * uE.x;
    for (int i = 0; i < uLightCount; i++) {
        float d = length(gl_FragCoord.xy - uLights[i].xy) / max(uLights[i].z, 1e-3);
        float k = exp(-d * d * 2.2);   // soft falloff: a light, not a flat disc of radiance
        e += uLightCol[i].rgb * (uLights[i].w * k);
    }
    float surf = solid > 0.35 ? 1.0 : (max(e.r, max(e.g, e.b)) > uE.y ? 0.5 : 0.0);
    o = vec4(e, surf);
}
)";

// Jump-flood distance field over the ray-stopping surfaces.
static const char* kFsJfaSeed = R"(
uniform sampler2D uSrc;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    o = texelFetch(uSrc, c, 0).a > 0.25 ? vec4(vec2(c), 1.0, 0.0) : vec4(0.0);
}
)";

static const char* kFsJfaStep = R"(
uniform sampler2D uSrc;
uniform float uStep;
uniform vec2 uRes;
void main() {
    vec2 c = floor(gl_FragCoord.xy);
    vec4 best = vec4(0.0);
    float bd = 1e20;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 q = c + vec2(x, y) * uStep;
            if (q.x < 0.0 || q.y < 0.0 || q.x >= uRes.x || q.y >= uRes.y) continue;
            vec4 s = texelFetch(uSrc, ivec2(q), 0);
            if (s.z > 0.5) {
                vec2 d = s.xy - c;
                float dd = dot(d, d);
                if (dd < bd) { bd = dd; best = s; }
            }
        }
    o = best;
}
)";

static const char* kFsJfaDist = R"(
uniform sampler2D uSrc;
void main() {
    vec2 c = floor(gl_FragCoord.xy);
    vec4 s = texelFetch(uSrc, ivec2(c), 0);
    o = vec4(s.z > 0.5 ? length(s.xy - c) : 1e4, 0.0, 0.0, 0.0);
}
)";

// Radiance cascades (direction-first layout, 4 pre-averaged rays per texel, 4x rays and
// 2x probe spacing per cascade). rgb = emitted light gathered, a = sky visibility.
// Merging uses the "bilinear fix": each ray is traced separately to the interval start of each
// of the 4 surrounding upper probes, which removes the rings a plain bilinear merge leaves.
static const char* kFsCascade = R"(
layout(location = 1) out vec4 o1;   // luminance-weighted light direction (L1), meaningful at cascade 0
uniform sampler2D uGi;     // rgb emission, a stop
uniform sampler2D uSdf;    // r = distance to nearest surface (px)
uniform sampler2D uUpper;  // cascade i+1
uniform vec2 uRes;
uniform vec4 uC;           // index, count, base interval, unused
uniform vec2 uSky;         // sky weight looking up, looking down
uniform vec4 uSun;         // direction xy (screen, y up), cos(half width), strength
uniform vec4 uSunColor;     // rgb
vec4 skyRadiance(vec2 dir) {
    float lobe = smoothstep(uSun.z, mix(uSun.z, 1.0, 0.6), dot(dir, uSun.xy));
    return vec4(uSunColor.rgb * (lobe * uSun.w), mix(uSky.y, uSky.x, dir.y * 0.5 + 0.5));
}
uniform float uOpacity;    // absorption per cell inside light sources (lasers, fire): light passes through
// Marches the segment A->B. Returns the light gathered along it (rgb) and how much of what lies
// beyond still gets through (a): 0 at a solid, partial after passing through light sources.
vec4 march(vec2 A, vec2 B) {
    vec2 d = B - A;
    float len = length(d);
    vec3 L = vec3(0.0);
    float T = 1.0;
    if (len < 1e-3) return vec4(L, T);
    vec2 dir = d / len;
    float t = 0.0;
    for (int i = 0; i < 28; i++) {
        if (t >= len) break;
        vec2 p = A + dir * t;
        if (p.x < 0.0 || p.y < 0.0 || p.x >= uRes.x || p.y >= uRes.y) break;
        float dist = texture(uSdf, p / uRes).r;
        if (dist < 0.75) {
            vec4 g = texture(uGi, p / uRes);
            if (g.a > 0.75) return vec4(L + T * g.rgb, 0.0);      // solid: opaque
            float k = 1.0 - exp(-uOpacity);                      // light source: semi-transparent
            L += T * g.rgb * k;
            T *= 1.0 - k;
            if (T < 0.02) return vec4(L, 0.0);
            t += 1.0;
            continue;
        }
        t += max(dist - 0.75, 1.0);
    }
    return vec4(L, T);
}
void main() {
    vec2 coord = floor(gl_FragCoord.xy);
    float idx = uC.x;
    float s = exp2(idx);
    vec2 probes = uRes / s;
    vec2 tile = floor(coord / probes);
    vec2 probe = coord - tile * probes;
    float group = tile.x + tile.y * s;
    float rays = 4.0 * s * s;
    vec2 org = (probe + 0.5) * s;
    float a = uC.z * (pow(4.0, idx) - 1.0) / 3.0;
    float b = a + uC.z * pow(4.0, idx);
    bool top = idx >= uC.y - 1.0;
    float s2 = s * 2.0;
    vec2 probes2 = uRes / s2;
    vec2 g2 = org / s2 - 0.5;           // position in the upper probe grid
    vec2 base = floor(g2), fr = g2 - base;
    vec4 sum = vec4(0.0);
    vec2 l1 = vec2(0.0);
    for (int j = 0; j < 4; j++) {
        float k = group * 4.0 + float(j);
        float ang = (k + 0.5) * 6.28318531 / rays;
        vec2 dir = vec2(cos(ang), sin(ang));
        vec2 A = org + dir * a;
        if (top) {
            vec4 m = march(A, org + dir * b);
            vec4 sky = skyRadiance(dir);
            vec4 rad = vec4(m.rgb + m.a * sky.rgb, m.a * sky.a);
            sum += rad;
            l1 += dir * dot(rad.rgb, vec3(0.2126, 0.7152, 0.0722));
            continue;
        }
        vec2 tile2 = vec2(mod(k, s2), floor(k / s2));
        vec4 acc = vec4(0.0);
        for (int c = 0; c < 4; c++) {
            vec2 off = vec2(c & 1, c >> 1);
            vec2 q = clamp(base + off, vec2(0.0), probes2 - 1.0);
            float w = (off.x > 0.5 ? fr.x : 1.0 - fr.x) * (off.y > 0.5 ? fr.y : 1.0 - fr.y);
            vec4 m = march(A, (q + 0.5) * s2 + dir * b);
            vec4 up = texelFetch(uUpper, ivec2(tile2 * probes2 + q), 0);
            acc += vec4(m.rgb + m.a * up.rgb, m.a * up.a) * w;
        }
        sum += acc;
        l1 += dir * dot(acc.rgb, vec3(0.2126, 0.7152, 0.0722));
    }
    o = sum * 0.25;
    o1 = vec4(l1 * 0.25, 0.0, 1.0);
}
)";

// Temporal smoothing of the cascade result: reproject last frame's light through the exact camera
// change (both frames' world glOrtho + grid offsets), clamp it to the current 3x3 neighbourhood to
// avoid ghosting, and blend.
static const char* kFsTemporal = R"(
layout(location = 1) out vec4 o1;
uniform sampler2D uCur;    // light rgb, sky a
uniform sampler2D uCur2;   // light direction (L1)
uniform sampler2D uHist;
uniform sampler2D uHist2;
uniform vec2 uRes;         // working res
uniform vec4 uNow;         // grid offset x, y, cell size, history valid
uniform vec4 uNowW;        // world left, bottom, world units per px x, y
uniform vec4 uPrev;        // previous grid offset x, y, cell size, blend
uniform vec4 uPrevW;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(uCur, c, 0), cur2 = texelFetch(uCur2, c, 0);
    o = cur;
    o1 = cur2;
    if (uNow.w < 0.5) return;
    vec2 scr = gl_FragCoord.xy * uNow.z - uNow.xy;
    vec2 world = vec2(uNowW.x + scr.x * uNowW.z, uNowW.y - scr.y * uNowW.w);
    vec2 scrP = vec2((world.x - uPrevW.x) / uPrevW.z, (uPrevW.y - world.y) / uPrevW.w);
    vec2 uvP = (scrP + uPrev.xy) / uPrev.z / uRes;
    if (uvP.x < 0.0 || uvP.y < 0.0 || uvP.x > 1.0 || uvP.y > 1.0) return;
    vec4 mn = cur, mx = cur, mn2 = cur2, mx2 = cur2;
    ivec2 hi = ivec2(uRes) - 1;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(c + ivec2(x, y), ivec2(0), hi);
            vec4 n = texelFetch(uCur, q, 0), n2 = texelFetch(uCur2, q, 0);
            mn = min(mn, n); mx = max(mx, n);
            mn2 = min(mn2, n2); mx2 = max(mx2, n2);
        }
    o = mix(cur, clamp(texture(uHist, uvP), mn, mx), uPrev.w);
    o1 = mix(cur2, clamp(texture(uHist2, uvP), mn2, mx2), uPrev.w);
}
)";

// Pull/push inpainting: solid pixels take the light of the nearest open air, so structure and
// terrain faces are lit by what's around them. Level 0 of the pull pyramid:
// Second target carries (sky visibility, light direction xy).
static const char* kFsPull0 = R"(
layout(location = 1) out vec4 o1;
uniform sampler2D uRC;
uniform sampler2D uRC2;
uniform sampler2D uGi;
uniform float uEmisThreshold;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec4 rc = texelFetch(uRC, c, 0);
    vec2 l1 = texelFetch(uRC2, c, 0).xy;
    vec4 g = texelFetch(uGi, c, 0);
    float w = (g.a > 0.75 && max(g.r, max(g.g, g.b)) <= uEmisThreshold) ? 0.0 : 1.0;
    o = vec4(rc.rgb * w, w);
    o1 = vec4(vec3(rc.a, l1) * w, w);
}
)";

// Further pull levels: a bilinear tap at the shared corner averages the 2x2 block.
static const char* kFsPull = R"(
layout(location = 1) out vec4 o1;
uniform sampler2D uA;
uniform sampler2D uB;
void main() { o = texture(uA, vUV); o1 = texture(uB, vUV); }
)";

static const char* kFsPush = R"(
layout(location = 1) out vec4 o1;
uniform sampler2D uA;      // pull level (premultiplied by coverage)
uniform sampler2D uB;
uniform sampler2D uCA;     // coarser push level (normalised)
uniform sampler2D uCB;
uniform float uCoarsest;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec4 f = texelFetch(uA, c, 0);
    vec3 fb = texelFetch(uB, c, 0).rgb;
    if (uCoarsest > 0.5) {
        bool any = f.a > 1e-4;
        o = vec4(any ? f.rgb / f.a : vec3(0.0), 1.0);
        o1 = vec4(any ? fb / f.a : vec3(1.0, 0.0, 0.0), 1.0);
        return;
    }
    o = vec4(f.rgb + texture(uCA, vUV).rgb * (1.0 - f.a), 1.0);
    o1 = vec4(fb + texture(uCB, vUV).rgb * (1.0 - f.a), 1.0);
}
)";

// Backdrop-only blur pyramid for depth: level 1 weights the scene by (1 - playfield coverage) so
// forts and terrain never bleed into the softened backdrop; later levels average the premultiplied result.
static const char* kFsBgDown = R"(
uniform sampler2D uSrc;
uniform sampler2D uMask;    // emissive target: a = playfield coverage
uniform vec2 uTexel;        // source texel
uniform int uFirst;
void main() {
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 4; i++) {
        vec2 uv = vUV + vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0) * uTexel;
        if (uFirst != 0) {
            float w = 1.0 - clamp(texture(uMask, uv).a, 0.0, 1.0);
            acc += vec4(toLinear(texture(uSrc, uv).rgb) * w, w);
        } else {
            acc += texture(uSrc, uv);
        }
    }
    o = acc * 0.25;
}
)";

static const char* kFsCopy = R"(
uniform sampler2D uSrc;
uniform int uAlpha;
void main() { vec4 t = texture(uSrc, vUV); o = vec4(uAlpha != 0 ? vec3(t.a) : t.rgb, 1.0); }
)";

// Capture-only visualisations of the lighting inputs/outputs.
static const char* kFsDebug = R"(
uniform sampler2D uEmis;
uniform sampler2D uOcc;
uniform sampler2D uRC;
uniform sampler2D uFillE;
uniform sampler2D uSdf;
uniform sampler2D uNorm;
uniform int uMode;
uniform float uOpenSky;
uniform vec4 uGiMap;       // grid offset x, y, 1/(p*gw), 1/(p*gh)
void main() {
    vec2 gi = (gl_FragCoord.xy * 2.0 + uGiMap.xy) * uGiMap.zw;   // debug targets are half res
    if (uMode == 0) {        // masks: red = solid, green = playfield, blue = emissive
        vec4 e = texture(uEmis, vUV);
        o = vec4(texture(uOcc, vUV).a, e.a * 0.6, luma(e.rgb) * 4.0, 1.0);
    } else if (uMode == 1) { // ray-traced light (air) + filled light on solids
        vec3 l = texture(uRC, gi).rgb + texture(uFillE, gi).rgb * texture(uOcc, vUV).a;
        o = vec4(toGamma(l / (1.0 + l)), 1.0);
    } else if (uMode == 2) { // sky visibility
        o = vec4(vec3(clamp(texture(uRC, gi).a / uOpenSky, 0.0, 1.0)), 1.0);
    } else if (uMode == 3) { // distance field
        o = vec4(vec3(fract(texture(uSdf, gi).r / 32.0)), 1.0);
    } else {                 // surface normals (rg) + specular mask (b)
        o = vec4(texture(uNorm, vUV).rgb, 1.0);
    }
}
)";

// Scene + bloom + lighting + fog -> tonemap -> grade. Output RGBA8 with luma in alpha for FXAA.
static const char* kFsComposite = R"(
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uEmis;   // rgb emissive, a playfield coverage
uniform sampler2D uOcc;    // a solid coverage
uniform sampler2D uRC;     // ray-traced light (rgb) + sky visibility (a), working res
uniform sampler2D uFillE;  // inpainted light
uniform sampler2D uFillV;  // inpainted sky visibility
uniform sampler2D uFogE;   // blurred light for fog
uniform sampler2D uFogV;
uniform sampler2D uRC2;    // light direction (L1) in the air
uniform sampler2D uNorm;   // rg normal xy, b specular mask (world material shader)
uniform vec4 uM1;  // light height, toon banding, hemisphere, ambient level
uniform vec4 uM2;  // specular, shininess, silhouette bevel strength, silhouette bevel radius (px)
uniform vec2 uFull;
uniform vec4 uA;   // exposure, shoulder knee, bloom scale, lighting on
uniform vec4 uBl;  // bloom strength, limit (0 = none), auto-dim, unused
uniform sampler2D uBloomAvg;   // how much of the screen glows (1x1, smoothed)
uniform vec4 uG1;  // contrast, saturation, vibrance, temperature
uniform vec4 uG2;  // tint, lift, gamma, gain
uniform vec4 uL;   // light intensity, ambient occlusion, open-sky visibility, background light
uniform vec4 uWorld;  // world glOrtho: left, right, bottom, top
uniform vec4 uF1;  // fog density, backdrop haze, drifting banks, pooling in enclosed spaces
uniform vec4 uF4;  // optional height fog: start world y, full world y, amount, backdrop blur
uniform sampler2D uBgBlur;   // backdrop-only blur (premultiplied by backdrop coverage)
uniform vec4 uF2;  // in front of playfield, ambient, light scatter, noise scale (world units)
uniform vec4 uF3;  // fog colour rgb, wind (world units / s)
uniform vec4 uGiMap;  // lighting grid offset x, y, 1/(p*gw), 1/(p*gh)
uniform vec4 uRain;   // near-layer amount, wind lean, wetness, time
uniform vec4 uFlash;  // lightning sky flash amount, colour rgb
uniform vec4 uSnow;   // near flakes, snow cap thickness (px at 4K), white-out, wind
uniform vec4 uWet;    // puddle reflections, unused...
uniform vec4 uSkyC;   // sky colour (linear) for wet reflections
uniform sampler2D uSnowMap;   // simulated snow layer (r = fill)
uniform vec4 uSnowL;          // layer on, unused...
uniform vec4 uSnowR;          // world rect of the snow layer: left, right, bottom, top
uniform vec4 uSnowCol;        // what lies there: albedo rgb (snow, sand or ash), kind
uniform sampler2D uTerrainM;  // world terrain mask (same rect as the snow layer)
uniform vec4 uVeil;           // white-out / dust / ash haze colour rgb, sandstorm dust sheets
uniform vec4 uMood;   // weather: ambient scale, temperature shift, saturation scale, unused
uniform vec4 uDN;     // day / night: backdrop tint rgb, daylight 0..1
uniform vec4 uWater;  // on, surface (world y, y down), time, waves
uniform vec4 uWater2; // reflections, foam, our own waves (1 = no surface from the game), unused
uniform vec4 uWaterH[16];  // the surface (world y) in 64 screen columns, read from the game's wave mesh
uniform float uTime;
vec3 shoulder(vec3 x, float k) {
    vec3 over = max(x - k, 0.0);
    vec3 comp = k + (1.0 - k) * (1.0 - exp(-over / max(1.0 - k, 1e-3)));
    return mix(x, comp, step(vec3(k), x));
}
float hash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) { v += a * vnoise(p); p = p * 2.03 + 17.1; a *= 0.5; }
    return v;
}
)"
R"(void main() {
    vec3 c = toLinear(texture(uScene, vUV).rgb);
    vec4 em = texture(uEmis, vUV);
    float pf = clamp(em.a, 0.0, 1.0);
    float solid = clamp(texture(uOcc, vUV).a, 0.0, 1.0);
    vec4 bgb = texture(uBgBlur, vUV);
    vec3 bgCol = bgb.a > 1e-3 ? bgb.rgb / bgb.a : c;               // local backdrop colour, playfield excluded
    c = mix(c, bgCol, uF4.w * (1.0 - pf));                           // depth: soften the painted backdrop
    c *= mix(uDN.rgb, mix(vec3(1.0), sqrt(uDN.rgb), 0.6), pf);        // time of day: dusk tint, a dark blue night (forts a little less)
    float front = clamp(texture(uOcc, vUV).r, 0.0, 1.0);             // front surfaces (not the fort's back wall)

    // snow lying in the scene: the simulated layer, with soft lumpy edges
    float cap = 0.0, snowTop = 0.0;
    if (uSnowL.x > 0.0) {
        float wx = mix(uWorld.x, uWorld.y, vUV.x), wy = mix(uWorld.z, uWorld.w, vUV.y);
        vec2 suv = vec2((wx - uSnowR.x) / (uSnowR.y - uSnowR.x), (wy - uSnowR.z) / (uSnowR.w - uSnowR.z));
        vec2 cellUV = vec2(0.0, 1.0 / vec2(textureSize(uSnowMap, 0)).y);
        float sv = texture(uSnowMap, suv).r, sUp = texture(uSnowMap, suv + cellUV).r;
        cap = smoothstep(0.26, 0.5, sv + (vnoise(vec2(wx * 0.05, vUV.y * 120.0)) - 0.5) * 0.18);
        cap *= 1.0 - smoothstep(0.3, 0.7, texture(uTerrainM, suv - cellUV * 0.5).r);   // never inside terrain
        snowTop = clamp((sv - sUp) * 2.5, 0.0, 1.0);                 // the upper surface of a pile
        if (cap > 0.0) {
            c = mix(c, uSnowCol.rgb * (0.86 + 0.14 * snowTop), cap);
            pf = max(pf, cap);
        }
    }

    if (uA.w > 0.5) {
        vec2 gi = (gl_FragCoord.xy + uGiMap.xy) * uGiMap.zw;
        vec4 rc = texture(uRC, gi);
        vec3 fill2 = texture(uFillV, gi).rgb;            // sky, light direction on solids
        vec3 E = mix(rc.rgb, texture(uFillE, gi).rgb, solid);
        float V = mix(rc.a, fill2.x, solid);
        vec2 D = mix(texture(uRC2, gi).xy, fill2.yz, solid);
        float Vn = clamp(V / uL.z, 0.0, 1.0);
        float eL = luma(E);
        E *= 1.0 / (1.0 + eL * 0.25);                           // soft saturation: blasts light, not blind
        D *= 1.0 / (1.0 + eL * 0.25);

        // surface normal: per-sprite normal from the material shader + bevel along solid silhouettes
        vec4 nb = texture(uNorm, vUV);
        vec2 nxy = nb.rg * 2.0 - 1.0;
        if (uM2.z > 0.0 && solid > 0.01) {
            vec2 px = uM2.w / uFull;
            float sl = texture(uOcc, vUV - vec2(px.x, 0.0)).a, sr = texture(uOcc, vUV + vec2(px.x, 0.0)).a;
            float sd = texture(uOcc, vUV - vec2(0.0, px.y)).a, su = texture(uOcc, vUV + vec2(0.0, px.y)).a;
            nxy -= vec2(sr - sl, su - sd) * uM2.z * solid;
        }
        float n2 = dot(nxy, nxy);
        if (n2 > 0.81) nxy *= 0.9 / sqrt(n2);
        vec3 N = vec3(nxy, sqrt(max(1.0 - dot(nxy, nxy), 0.0)));

        // weather on surfaces: how far (px at 4K) this solid pixel is below the open air above it
        float pxs = uFull.y / 2160.0;
        float topD = 1e3;
        if (front > 0.5 && uRain.z > 0.0) {
            const float steps[9] = float[9](1.0, 2.0, 3.5, 5.0, 7.0, 10.0, 14.0, 20.0, 28.0);
            for (int k = 0; k < 9; k++)
                if (texture(uOcc, vUV + vec2(0.0, steps[k] * pxs / uFull.y)).r < 0.5) { topD = steps[k]; break; }
        }
        if (cap > 0.0) {   // snow: soft, facing up, not metal
            N = normalize(mix(N, vec3(0.0, 0.45, 0.89), cap));
            nb.b *= 1.0 - cap;
        }
        float wet = uRain.z * front * (1.0 - cap);
        if (wet > 0.0) c = mix(c, pow(c, vec3(1.2)), clamp(wet, 0.0, 1.0)) * (1.0 - 0.06 * wet);   // wet paint: darker, richer

        // directional part of the gathered light: how one-sided it is, and from where
        float dl = length(D);
        float dirAmt = clamp(dl / max(luma(E), 1e-4), 0.0, 1.0);
        vec2 Dn = dl > 1e-6 ? D / dl : vec2(0.0);
        vec3 L = normalize(vec3(Dn * dirAmt, mix(1.0, uM1.x, dirAmt)));
        // Solids are lit from the edge that faces the light and less further in, so light follows the
        // shape of struts and armour instead of brightening the whole sprite like a flat rectangle.
        float expo = 1.0;
        if (solid > 0.5 && dirAmt > 0.05) {
            vec2 st = Dn * (6.0 * pxs) / uFull;
            float a = 0.0;
            for (int k = 1; k <= 5; k++) a = max(a, (1.0 - texture(uOcc, vUV + st * float(k)).a) * (1.0 - float(k - 1) * 0.16));
            expo = mix(1.0, mix(0.4, 1.2, a), dirAmt);
        }
        float nl = max(dot(N, L), 0.0) / max(L.z, 0.2);              // 1 for a flat, camera-facing surface
        float toon = 0.25 + 0.75 * smoothstep(0.35, 0.5, nl) + 0.5 * smoothstep(1.15, 1.3, nl);
        nl = mix(nl, toon, uM1.y);
        float hemi = 1.0 + uM1.z * N.y;                               // sky above, ground bounce below

        vec3 sky = uFlash.yzw * uFlash.x;
        vec3 lightPf = vec3(mix(1.0, Vn, uL.y) * hemi * uM1.w * uMood.x) + E * uL.x * nl * expo + sky * Vn * (0.6 + 0.4 * max(N.y, 0.0));
        vec3 lightBg = vec3(1.0) + E * uL.x * uL.w + sky * 0.45;
        // light can at most triple a surface (and double the backdrop), so painted detail
        // always shows through even in the middle of a strong light
        lightPf = min(lightPf, vec3(3.0));
        lightBg = min(lightBg, vec3(2.0));
        vec3 m = mix(lightBg, lightPf, pf);
        // smoke, steam and dust puffs are not painted surfaces: the surface lighting (up to 3x) would turn
        // dark smoke pale; they take the light through lit smoke below instead
        m = mix(m, min(m, vec3(1.1)), clamp(texture(uOcc, vUV).b, 0.0, 1.0));
        m = mix(m, vec3(1.0), clamp(luma(toLinear(em.rgb)) * 4.0, 0.0, 1.0));   // emitters are already lit
        vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));
        vec3 spec = E * (pow(max(dot(N, H), 0.0), uM2.y) * nb.b * uM2.x * dirAmt * pf);
        spec *= 1.0 + wet * 3.0;
        spec += (E + sky * Vn) * wet * pow(max(N.y, 0.0), 3.0) * 0.35;   // wet sheen on upward faces
        if (wet > 0.0) {
            vec3 skyRefl = mix(uSkyC.rgb, bgCol, 0.3) * (0.45 + 0.55 * Vn);
            spec += skyRefl * wet * pow(max(N.y, 0.0), 2.0) * 0.14;                     // sky in the wet film
            spec += (skyRefl + E) * wet * (1.0 - smoothstep(1.0, 5.0, topD)) * 0.7;    // wet rim along the tops

        }
        // Lighting may brighten a surface, but the extra is rolled off by luminance (hue kept), so
        // light-coloured paint like bone does not wash out to pale white in the per-channel tonemap.
        vec3 lit = c * m;
        float l0 = luma(c), l1 = luma(lit), k = max(0.62, l0);
        if (l1 > k && pf > 0.0) {
            float over = l1 - k, room = max(1.0 - k, 0.08);
            float l2 = k + room * (1.0 - exp(-over / room));
            float e = clamp(luma(toLinear(em.rgb)) * 4.0, 0.0, 1.0);    // light sources keep their punch
            lit *= mix(1.0, l2 / l1, pf * (1.0 - e));
        }
        c = lit + spec;
)" R"(
        // lit smoke: steam and smoke scatter the ray-traced light passing through them (glowing orange
        // near fires and explosions) and thick smoke away from light is darker
        float smokeD = clamp(texture(uOcc, vUV).g, 0.0, 1.0);
        if (smokeD > 0.002) {
            c = mix(c, c * (0.7 + 0.3 * Vn), smokeD * 0.6);
            c += E * uL.x * smokeD * 0.9 * min(c * 1.8, vec3(1.0));   // scattered light goes with the smoke's own colour: steam glows, soot hardly
        }

        // puddles on flat wet tops: the scene mirrored about the water line, with rain ripples
        if (wet > 0.0 && uWet.x > 0.0 && topD < 12.0) {
            vec2 o8 = vec2(8.0 * pxs / uFull.x, 0.0), up = vec2(0.0, (topD + 1.5) * pxs / uFull.y);
            float flatTop = (1.0 - texture(uOcc, vUV + o8 + up).r) * (1.0 - texture(uOcc, vUV - o8 + up).r)
                          * texture(uOcc, vUV + o8).r * texture(uOcc, vUV - o8).r;
            float pud = flatTop * (1.0 - smoothstep(4.0, 12.0, topD)) * clamp(wet * uWet.x, 0.0, 1.0);
            if (pud > 0.01) {
                vec2 cp = gl_FragCoord.xy / (pxs * 34.0);
                vec2 ci = floor(cp);
                float hc = hash21(ci), tt = fract(uRain.w * 0.9 + hc);
                vec2 cc = ci + 0.5 + (vec2(hash21(ci + 3.1), hash21(ci + 7.7)) - 0.5) * 0.6;
                float rr = length(cp - cc), front = rr - tt * 0.55;
                float ring = sin(front * 38.0) * exp(-abs(front) * 14.0) * (1.0 - tt);
                vec2 mir = vUV + vec2(ring * 2.5 * pxs / uFull.x, (2.0 * topD + 2.0) * pxs / uFull.y);
                vec3 refl = toLinear(texture(uScene, mir).rgb) * (0.55 + 0.45 * Vn) + toLinear(texture(uEmis, mir).rgb) * 1.2;
                refl = mix(refl, uSkyC.rgb * (0.5 + 0.5 * Vn), 0.3);   // a little sky in every puddle
                refl += vec3(0.8, 0.85, 0.9) * max(ring, 0.0) * 0.2;
                c = mix(c, refl, pud * 0.62);
            }
        }

        // water (High Seas maps): reflections, waves, depth, foam and glints below the map's water line
        if (uWater.x > 0.0) {
            float wy = mix(uWorld.z, uWorld.w, vUV.y), wx = mix(uWorld.x, uWorld.y, vUV.x);
            float wpp = abs(uWorld.w - uWorld.z) / uFull.y;            // world units per pixel
            float fc = clamp(vUV.x, 0.0, 1.0) * 64.0 - 0.5;
            int c0 = int(clamp(floor(fc), 0.0, 63.0)), c1 = min(c0 + 1, 63);
            float wl = mix(uWaterH[c0 / 4][c0 % 4], uWaterH[c1 / 4][c1 % 4], clamp(fc - floor(fc), 0.0, 1.0));
            float dpx = (wy - wl) / wpp;                                // pixels below the surface (> 0 = under water)
            float tw = uWater.z;
            float wv = (sin(wx * 0.018 + tw * 1.5) * 0.6 + sin(wx * 0.043 - tw * 2.2) * 0.4) * uWater.w;
            float surf = dpx - wv * 3.0 * pxs * uWater2.z;              // the game's moving surface (or our waves)
            if (surf > 0.0) {
                float nz = vnoise(vec2(wx * 0.012 + tw * 0.35, dpx * 0.02 - tw * 0.2)) - 0.5;
                vec2 mir = vec2(vUV.x + (nz * 8.0 + wv * 2.5) * pxs * uWater.w / uFull.x, vUV.y + (2.0 * dpx + 2.0) / uFull.y);
                vec3 refl = mir.y < 1.0 ? toLinear(texture(uScene, mir).rgb) * (0.5 + 0.5 * uM1.w) + toLinear(texture(uEmis, mir).rgb) * 1.6
                                        : uSkyC.rgb;
                float fres = mix(0.6, 0.1, smoothstep(0.0, 220.0 * pxs, surf)) * uWater2.x;
                vec3 deep = vec3(0.03, 0.10, 0.14) * (0.35 + 0.65 * uM1.w) + E * 0.15;
                c = mix(c, deep + c * 0.3, smoothstep(0.0, 900.0 * pxs, surf) * 0.75);  // darker, bluer with depth
                c = mix(c, refl, clamp(fres, 0.0, 0.95));
                float glint = pow(max(vnoise(vec2(wx * 0.05 + tw * 0.9, wy * 0.3)) - 0.6, 0.0) * 2.5, 3.0) * (1.0 - smoothstep(0.0, 160.0 * pxs, surf));
                c += (uSkyC.rgb * 1.5 + E * 2.0) * glint * uWater.w;                    // sun and fire glints on the waves
                c += E * 0.25 * (1.0 - smoothstep(0.0, 60.0 * pxs, surf));                // light from explosions on the surface
                float foam = (1.0 - smoothstep(0.0, (4.0 + 3.0 * (wv + 1.0)) * pxs, surf)) * smoothstep(0.35, 0.75, vnoise(vec2(wx * 0.07 + tw * 0.7, tw * 0.4)));
                c = mix(c, vec3(0.86, 0.9, 0.93) * (0.55 + 0.45 * uM1.w) + E * 0.4, clamp(foam * uWater2.y, 0.0, 0.9));
            }
        }

        if (uF1.x > 0.0) {
            vec2 wp = vec2(mix(uWorld.x, uWorld.y, vUV.x), mix(uWorld.z, uWorld.w, vUV.y));
            float h = uF4.z > 0.0 ? clamp((wp.y - uF4.x) / max(uF4.y - uF4.x, 1.0), 0.0, 1.0) : 0.0;
            vec2 np = (wp + vec2(uTime * uF3.w, uTime * uF3.w * 0.15)) / max(uF2.w, 1.0);
            float n = fbm(np) * 1.25 + 0.15 * fbm(np * 3.7 - uTime * 0.05);
            float fvSky = clamp(texture(uFogV, gi).r / uL.z, 0.0, 1.0);
            float pool = (1.0 - fvSky) * (1.0 - fvSky);
            // haze and pooling are for the open air: inside a fort the sky is hidden too, but that is no valley
            float dens = uF1.x * (uF1.y * (1.0 - pf) + uF1.z * n + uF1.w * pool * (1.0 - pf) + uF4.z * h * h * n);
            vec3 fe = texture(uFogE, gi).rgb;
            fe *= 1.0 / (1.0 + luma(fe) * 0.5);
            float fv = clamp(texture(uFogV, gi).r / uL.z, 0.0, 1.0);
            vec3 inscatter = mix(uF3.rgb, bgCol, 0.5) * uF2.y * mix(0.35, 1.0, fv) + fe * uF2.z;
            // fog lives in the air: thinner in front of playfield objects, almost none on solids
            float T = exp(-dens * mix(1.0, uF2.x, pf) * (1.0 - 0.92 * solid));
            c = c * T + inscatter * (1.0 - T);
        }
        if (uRain.x > 0.0) {
            float streaks = 0.0;
            for (int L = 0; L < 2; L++) {
                float sc = L == 0 ? 3.0 : 1.6;
                vec2 p = vUV * vec2(uFull.x / uFull.y, 1.0) * sc;
                p.x += p.y * uRain.y;
                p.y += uRain.w * (L == 0 ? 1.6 : 1.1);
                vec2 cell = vec2(floor(p.x * 34.0), 0.0);
                float h = hash21(cell + float(L) * 17.0);
                float y = fract(p.y + h * 7.13);
                float fx = fract(p.x * 34.0) - 0.5;
                float len = 0.10 + 0.12 * h;
                streaks += (1.0 - smoothstep(0.0, 0.22, abs(fx))) * (1.0 - smoothstep(0.0, len, abs(y - 0.5))) * step(0.55, h) * (L == 0 ? 0.6 : 1.0);
            }
            vec3 rl = texture(uFogE, gi).rgb * 1.5 + uFlash.yzw * uFlash.x * 0.8 + vec3(0.25, 0.27, 0.3);
            c += rl * streaks * uRain.x * 0.07;   // a faint near layer; the real rain is the simulated drops
        }
        if (uSnow.x > 0.0 || uSnow.z > 0.0) {
            // near snow: three layers of soft flakes, far small to near big, swaying in the wind
            float flakes = 0.0;
            for (int L = 0; L < 3; L++) {
                float sc = L == 0 ? 7.0 : L == 1 ? 4.0 : 2.2;
                vec2 p = vUV * vec2(uFull.x / uFull.y, 1.0) * sc;
                p += vec2(uSnow.w * uRain.w * (0.04 + 0.03 * float(L)), uRain.w * (0.07 + 0.05 * float(L)));
                p.x += sin(p.y * 3.0 + uRain.w * 0.7 + float(L) * 2.0) * 0.08;
                vec2 ci = floor(p), f = fract(p) - 0.5;
                float h = hash21(ci + float(L) * 31.0);
                vec2 off = (vec2(hash21(ci + 5.3), hash21(ci + 9.1)) - 0.5) * 0.6;
                float sz = (0.03 + 0.035 * h) * (L == 2 ? 1.25 : 1.0);
                flakes += step(0.5, h) * (1.0 - smoothstep(sz * 0.4, sz, length(f - off))) * (L == 2 ? 0.6 : 1.0);
            }
            vec3 fe2 = texture(uFogE, gi).rgb;
            vec3 fl = fe2 * 1.2 + uSnowCol.rgb * 0.92 * uM1.w * uMood.x;
            c = mix(c, fl, clamp(flakes * uSnow.x * 0.42, 0.0, 0.8));
            // white-out: snow in the air veils everything, the distance most
            vec3 veil = uVeil.rgb * (0.6 + 0.4 * uM1.w) + fe2 * 0.3;
            c = mix(c, veil, uSnow.z * mix(0.6, 0.2, pf));
            if (uVeil.w > 0.0) {   // sandstorm: sheets of dust racing across the screen
                vec2 wp = vec2(mix(uWorld.x, uWorld.y, vUV.x), mix(uWorld.z, uWorld.w, vUV.y));
                vec2 q = wp / 1800.0 + vec2(uRain.w * 0.55 * sign(uSnow.w + 1e-3), uRain.w * 0.04);
                float sheet = smoothstep(0.42, 0.85, fbm(q * vec2(0.6, 2.2)) + 0.15 * fbm(q * 3.1 + 7.0));
                c = mix(c, veil, sheet * uVeil.w * mix(0.65, 0.35, pf));
            }
        }
    }

    vec3 bl = min(texture(uBloom, vUV).rgb, vec3(256.0)) * uA.z;
    bl *= uBl.x / (1.0 + uBl.z * 3.0 * texture(uBloomAvg, vec2(0.5)).r * uA.z);   // strength, dimmed when the screen glows
    if (uBl.y > 0.0) {   // limit: unchanged up to half of it, then eased towards it
        float L = luma(bl), kn = uBl.y * 0.5;
        if (L > kn) bl *= (kn + (uBl.y - kn) * (1.0 - exp(-(L - kn) / (uBl.y - kn)))) / L;
    }
    c = c * uA.x + bl;
    c = min(c, vec3(64.0));
    c = shoulder(c, uA.y);
    vec3 s = toGamma(c);
    float temp = uG1.w + uMood.y;
    s *= vec3(1.0 + 0.08 * temp, 1.0 + 0.04 * uG2.x, 1.0 - 0.08 * temp);   // white balance
    s = uG2.w * (s + uG2.y * (1.0 - s));                                       // lift / gain
    s = pow(max(s, vec3(0.0)), vec3(1.0 / max(uG2.z, 0.01)));                  // gamma
    s = (s - 0.5) * uG1.x + 0.5;                                               // contrast
    float l = luma(s);
    float sat = max(s.r, max(s.g, s.b)) - min(s.r, min(s.g, s.b));
    s = mix(vec3(l), s, uG1.y * uMood.z * (1.0 + uG1.z * (1.0 - clamp(sat, 0.0, 1.0))));
    s = clamp(s, 0.0, 1.0);
    o = vec4(s, luma(s));
}
)";

// FXAA 3.11-style edge search (quality preset ~12), luma read from alpha.
static const char* kFsFxaa = R"(
uniform sampler2D uSrc;
uniform vec2 uTexel;
float L(vec2 uv) { return texture(uSrc, uv).a; }
void main() {
    vec2 uv = vUV;
    vec4 cM = texture(uSrc, uv);
    float lM = cM.a;
    float lN = L(uv + vec2(0, uTexel.y)), lS = L(uv - vec2(0, uTexel.y));
    float lE = L(uv + vec2(uTexel.x, 0)), lW = L(uv - vec2(uTexel.x, 0));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) { o = cM; return; }
    float lNW = L(uv + vec2(-1, 1) * uTexel), lNE = L(uv + uTexel);
    float lSW = L(uv - uTexel), lSE = L(uv + vec2(1, -1) * uTexel);
    float eH = abs(-2.0 * lW + lNW + lSW) + 2.0 * abs(-2.0 * lM + lN + lS) + abs(-2.0 * lE + lNE + lSE);
    float eV = abs(-2.0 * lS + lSW + lSE) + 2.0 * abs(-2.0 * lM + lW + lE) + abs(-2.0 * lN + lNW + lNE);
    bool horz = eH >= eV;
    float l1 = horz ? lS : lW, l2 = horz ? lN : lE;
    float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    float stepLen = horz ? uTexel.y : uTexel.x;
    float lLocal;
    if (g1 >= g2) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lM); } else { lLocal = 0.5 * (l2 + lM); }
    float gScaled = 0.25 * max(g1, g2);
    vec2 cuv = uv + (horz ? vec2(0, stepLen * 0.5) : vec2(stepLen * 0.5, 0));
    vec2 off = horz ? vec2(uTexel.x, 0) : vec2(0, uTexel.y);
    vec2 uv1 = cuv - off, uv2 = cuv + off;
    float e1 = L(uv1) - lLocal, e2 = L(uv2) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    const float Q[10] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(r1 && r2); i++) {
        if (!r1) { uv1 -= off * Q[i]; e1 = L(uv1) - lLocal; r1 = abs(e1) >= gScaled; }
        if (!r2) { uv2 += off * Q[i]; e2 = L(uv2) - lLocal; r2 = abs(e2) >= gScaled; }
    }
    float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y;
    float d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    bool near1 = d1 < d2;
    float pix = -min(d1, d2) / (d1 + d2) + 0.5;
    bool ok = ((near1 ? e1 : e2) < 0.0) != (lM < lLocal);
    float fin = ok ? pix : 0.0;
    float lAvg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    fin = max(fin, sub * sub * 0.5);
    vec2 fuv = uv + (horz ? vec2(0, fin * stepLen) : vec2(fin * stepLen, 0));
    o = vec4(texture(uSrc, fuv).rgb, lM);
}
)";

// Distortion (shockwave rings, heat haze over light sources, screen shake), contrast-adaptive
// sharpening, chromatic aberration, vignette, film grain, colour pulse and impact frames -> back buffer.
static const char* kFsFinal = R"(
uniform sampler2D uSrc;
uniform int uHeatCount;
uniform vec4 uHeatSrc[16]; // heat sources (fire, explosions, firing guns): centre uv, radius (uv height units), strength
uniform sampler2D uOcc;    // a = solid coverage   (impact frames)
uniform sampler2D uEmis;   // rgb light sources, a = playfield coverage
uniform vec2 uTexel;
uniform vec4 uF;      // sharpen, vignette, grain, chromatic aberration
uniform float uTime;
uniform int uShockCount;
uniform vec4 uShock[16];   // centre uv, radius (uv height units), strength
uniform vec4 uShake;       // offset uv xy, overscan, heat haze strength
uniform vec4 uImpact;      // amount, centre uv xy, invert
uniform vec4 uPulse;       // white flash, extra chromatic aberration, saturation boost, exposure dip
uniform sampler2D uRaw;    // lab wipe: the game's own colours
uniform vec4 uLook;        // split-tone strength, ink outlines, posterize levels, wipe position (< 0 = off)
uniform vec4 uShT, uHiT;   // split-tone shadow / highlight colours (0.5 grey = neutral)
uniform sampler2D uDof;    // photo mode: a soft copy of the image
uniform vec4 uDofP;        // blur strength, focus uv (x, y), tilt-shift
uniform vec4 uFrame;       // framing bars: aspect ratio (0 = none)
vec2 gUV;
vec3 S(vec2 d) { return texture(uSrc, gUV + d * uTexel).rgb; }
float hash(vec2 p) { p = fract(p * vec2(443.897, 441.423)); p += dot(p, p.yx + 19.19); return fract((p.x + p.y) * p.x); }
float vn(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
void main() {
    float aspect = uTexel.y / uTexel.x;
    vec2 uv = 0.5 + (vUV - 0.5) * (1.0 - uShake.z) + uShake.xy;
    // shockwave rings: push pixels outward around an expanding ring
    for (int i = 0; i < uShockCount; i++) {
        vec2 d = uv - uShock[i].xy;
        d.x *= aspect;
        float r = length(d);
        float ring = r - uShock[i].z;
        float w = 0.035 + uShock[i].z * 0.12;
        float k = exp(-ring * ring / (w * w)) * uShock[i].w;
        if (r > 1e-4) uv -= (d / r) * vec2(1.0 / aspect, 1.0) * k * 0.035;
    }
    // heat haze: a rising plume of shimmer above each heat source
    if (uShake.w > 0.0 && uHeatCount > 0) {
        float heat = 0.0;
        for (int i = 0; i < uHeatCount; i++) {
            vec2 d = uv - uHeatSrc[i].xy;
            d.x *= aspect;
            float r = uHeatSrc[i].z;
            float across = exp(-d.x * d.x / (r * r * 0.5));
            float up = smoothstep(-0.7 * r, 0.1 * r, d.y) * (1.0 - smoothstep(0.6 * r, 3.2 * r, d.y));
            heat += uHeatSrc[i].w * across * up;
        }
        heat = clamp(heat, 0.0, 1.0);
        vec2 q = uv * vec2(aspect, 1.0) * 70.0 + vec2(0.0, -uTime * 3.5);
        uv += (vec2(vn(q), vn(q + 17.3)) - 0.5) * heat * uShake.w * 0.0045;
    }
    gUV = uv;
    vec3 c = S(vec2(0));
    float ca = uF.w + uPulse.y;
    if (ca > 0.0) {
        vec2 dir = (gUV - 0.5) * ca * 0.01;
        c.r = texture(uSrc, gUV + dir).r;
        c.b = texture(uSrc, gUV - dir).b;
    }
    if (uF.x > 0.0) {
        vec3 a = S(vec2(0, -1)), b = S(vec2(-1, 0)), d = S(vec2(1, 0)), e = S(vec2(0, 1));
        vec3 mn = min(c, min(min(a, b), min(d, e)));
        vec3 mx = max(c, max(max(a, b), max(d, e)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 w = amp * (-1.0 / mix(8.0, 5.0, clamp(uF.x, 0.0, 1.0)));
        c = clamp((c + (a + b + d + e) * w) / (1.0 + 4.0 * w), 0.0, 1.0);
    }
    // looks: split toning, posterize, comic ink outlines
    if (uLook.x > 0.0) {
        float l = luma(c);
        c = clamp(c + ((uShT.rgb - 0.5) * (1.0 - l) + (uHiT.rgb - 0.5) * l) * uLook.x, 0.0, 1.0);
    }
    if (uLook.z >= 2.0) {
        float l = max(luma(c), 1e-4);
        float lq = floor(l * uLook.z + 0.5) / uLook.z;
        float pfp = clamp(texture(uEmis, gUV).a, 0.0, 1.0);   // forts and terrain only: smooth skies band badly
        c = clamp(c * mix(1.0, lq / l, 0.85 * pfp), 0.0, 1.0);
    }
    if (uLook.y > 0.0) {
        float k = max(1.0, 1.0 / (uTexel.y * 1080.0));   // line width follows resolution
        float l00 = luma(S(vec2(-k, -k))), l10 = luma(S(vec2(0, -k))), l20 = luma(S(vec2(k, -k)));
        float l01 = luma(S(vec2(-k, 0))), l21 = luma(S(vec2(k, 0)));
        float l02 = luma(S(vec2(-k, k))), l12 = luma(S(vec2(0, k))), l22 = luma(S(vec2(k, k)));
        float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
        float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
        vec2 o2 = uTexel * k * 1.5;
        float so = abs(texture(uOcc, gUV + vec2(o2.x, 0.0)).a - texture(uOcc, gUV - vec2(o2.x, 0.0)).a)
                 + abs(texture(uOcc, gUV + vec2(0.0, o2.y)).a - texture(uOcc, gUV - vec2(0.0, o2.y)).a);
        float ink = clamp(smoothstep(0.15, 0.5, length(vec2(gx, gy))) * 0.8 + smoothstep(0.25, 0.8, so), 0.0, 1.0);
        c = mix(c, c * 0.1, ink * clamp(uLook.y, 0.0, 1.0) * (0.6 + 0.2 * clamp(uLook.y, 1.0, 2.0)));
    }
    if (uDofP.x > 0.0) {
        // depth of field: the forts and terrain are the near layer, the backdrop the far one; the depth
        // under the focus point is sharp, the other layer blurs, plus tilt-shift away from the focus line
        float dHere = 1.0 - clamp(texture(uEmis, gUV).a * 1.5, 0.0, 1.0);
        float dFocus = 1.0 - clamp(texture(uEmis, uDofP.yz).a * 1.5, 0.0, 1.0);
        float coc = abs(dHere - dFocus) * min(uDofP.x * 1.5, 1.0) + uDofP.w * smoothstep(0.06, 0.45, abs(gUV.y - uDofP.z));
        c = mix(c, texture(uDof, gUV).rgb, clamp(coc, 0.0, 1.0));
    }
    vec2 q = vUV - 0.5;
    q.x *= uTexel.y / uTexel.x;          // aspect-correct
    c *= mix(1.0, 1.0 - smoothstep(0.25, 1.25, length(q) * 1.4), uF.y);
    float n = hash(gl_FragCoord.xy + fract(uTime * 0.6180339) * 1000.0) - 0.5;
    float l = luma(c);
    c += n * uF.z * (1.0 - abs(l * 2.0 - 1.0) * 0.6);
    // colour pulse on big hits: saturation punch + white flash
    if (uPulse.z > 0.0) c = clamp(mix(vec3(luma(c)), c, 1.0 + uPulse.z), 0.0, 1.0);
    c = mix(c, vec3(1.0), clamp(uPulse.x, 0.0, 1.0));
    c *= 1.0 - clamp(uPulse.w, 0.0, 0.6);
    // impact frame: anime-style - black silhouettes on white, glowing core, radial speed lines
    if (uImpact.x > 0.0) {
        float pf = clamp(texture(uEmis, gUV).a + texture(uOcc, gUV).a, 0.0, 1.0);
        float glow = clamp(luma(texture(uEmis, gUV).rgb) * 3.0, 0.0, 1.0);
        vec3 imp = vec3(1.0 - smoothstep(0.25, 0.6, pf));
        vec2 d = (gUV - uImpact.yz) * vec2(aspect, 1.0);
        float ang = atan(d.y, d.x) / 6.2831853 + 0.5;
        float lineId = floor(ang * 140.0);
        float lines = step(0.72, hash(vec2(lineId, floor(uTime * 24.0)))) * smoothstep(0.08, 0.55, length(d));
        imp = mix(imp, vec3(0.0), lines * (1.0 - pf) * 0.85);
        imp = mix(imp, vec3(1.0, 0.25, 0.08), glow);
        if (uImpact.w > 0.5) imp = vec3(1.0) - imp;
        c = mix(c, imp, uImpact.x);
    }
    if (uFrame.x > 0.0) {   // framing bars (part of the photo)
        float s = uTexel.y / uTexel.x;
        if (uFrame.x > s) { if (abs(vUV.y - 0.5) > 0.5 * s / uFrame.x) c = vec3(0.0); }
        else if (abs(vUV.x - 0.5) > 0.5 * uFrame.x / s) c = vec3(0.0);
    }
    // lab wipe: the game's own colours left of the line, FortsGFX right of it
    if (uLook.w >= 0.0) {
        if (vUV.x < uLook.w) c = texture(uRaw, vUV).rgb;
        float dl = abs(vUV.x - uLook.w) / uTexel.x;
        c = mix(c, vec3(1.0), 1.0 - smoothstep(1.5, 3.5, dl));
    }
    o = vec4(c, 1.0);
}
)";

// ================================================================ Phase 4/5: effects
// Volumetric 3D fireballs, one instanced quad each. Written premultiplied into the scene and
// light-source targets before lighting, so they glow, bloom and light the world.
static const char* kVsFireball = R"(#version 330
uniform vec4 uFb[24];      // centre (GL screen px), radius px, age 0..1
uniform vec4 uFbSeed[24];  // seed, unused...
uniform vec2 uFull;
out vec2 vUV;
flat out vec4 vInfo;
flat out vec4 vSeed;
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    vec4 f = uFb[gl_InstanceID];
    vUV = corner;
    vInfo = f;
    vSeed = uFbSeed[gl_InstanceID];
    vec2 px = f.xy + corner * f.z;
    gl_Position = vec4(px / uFull * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFsFireball = R"(
layout(location = 1) out vec4 o1;
flat in vec4 vInfo;
flat in vec4 vSeed;
uniform float uTime;
uniform vec4 uFbLook;      // brightness, saturation, smoke, unused
float h3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float n3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(h3(i), h3(i + vec3(1, 0, 0)), f.x), mix(h3(i + vec3(0, 1, 0)), h3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(h3(i + vec3(0, 0, 1)), h3(i + vec3(1, 0, 1)), f.x), mix(h3(i + vec3(0, 1, 1)), h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm3(vec3 p) { float v = 0.0, a = 0.5; for (int i = 0; i < 4; i++) { v += a * n3(p); p = p * 2.07 + 3.1; a *= 0.5; } return v; }
vec3 ramp(float heat) {
    vec3 c = mix(vec3(0.12, 0.10, 0.09), vec3(0.75, 0.12, 0.03), smoothstep(0.05, 0.3, heat));
    c = mix(c, vec3(1.0, 0.45, 0.08), smoothstep(0.3, 0.55, heat));
    c = mix(c, vec3(1.0, 0.85, 0.45), smoothstep(0.55, 0.8, heat));
    return mix(c, vec3(1.0, 1.0, 0.92), smoothstep(0.8, 1.0, heat));
}
void main() {
    float r2 = dot(vUV, vUV);
    if (r2 > 1.0) discard;
    float age = vInfo.w;
    float grow = 0.35 + 0.65 * (1.0 - pow(1.0 - age, 3.0));          // fast expansion, ease out
    float zMax = sqrt(1.0 - r2);
    vec3 acc = vec3(0.0), em = vec3(0.0);
    float T = 1.0;
    const int N = 12;
    for (int i = 0; i < N; i++) {
        float z = mix(-zMax, zMax, (float(i) + 0.5) / float(N));
        vec3 p = vec3(vUV, z) / grow;
        float rad = length(p);
        if (rad > 1.0) continue;
        vec3 q = p * 2.3 + vSeed.x * 7.0 + vec3(0.0, -uTime * 0.9 - age * 2.0, uTime * 0.3);
        float dens = fbm3(q) * 1.35 - rad * (0.75 + age * 0.55);
        dens = clamp(dens * 3.0, 0.0, 1.0) * (1.0 - smoothstep(0.65, 1.0, age));
        if (dens <= 0.0) continue;
        float heat = clamp((1.0 - rad * 1.2) * (1.0 - age * 1.25) * 1.6 + fbm3(q * 1.7) * 0.4 - 0.2, 0.0, 1.0);
        vec3 col = ramp(heat);
        float shade = mix(0.45, 1.0, clamp(0.6 - p.y * 0.5 + (1.0 - rad), 0.0, 1.0));   // lit from the core / above
        float a = dens * (2.0 / float(N)) * 2.2;
        a = clamp(a, 0.0, 1.0);
        vec3 lit = col * mix(shade, 1.0, heat);
        acc += T * a * lit;
        em += T * a * col * smoothstep(0.25, 0.7, heat) * uFbLook.x;
        T *= 1.0 - a;
        if (T < 0.02) break;
    }
    float alpha = 1.0 - T;
    acc = mix(vec3(luma(acc)), acc, uFbLook.y);
    o = vec4(toGamma(acc), alpha);                    // premultiplied-ish (gamma space like the game)
    o1 = vec4(toGamma(em), alpha * (1.0 - uFbLook.z));
}
)";

// The milk miko's milk: its particles are splatted as soft blobs into a half-res density target, and here
// the density becomes a liquid surface - a threshold for the edge, its gradient for a normal, a creamy
// colour, a highlight - drawn into the scene before lighting, so it is lit like everything else.
static const char* kFsMilk = R"(
layout(location = 1) out vec4 o1;
uniform sampler2D uD;
uniform vec2 uTexel;
uniform float uWater;      // 0 milk (opaque, creamy), 1 water (clear blue, foamy edges, sharper shine)
void main() {
    float d = texture(uD, vUV).r;
    float dl = texture(uD, vUV - vec2(uTexel.x, 0.0)).r, dr = texture(uD, vUV + vec2(uTexel.x, 0.0)).r;
    float dd = texture(uD, vUV - vec2(0.0, uTexel.y)).r, du = texture(uD, vUV + vec2(0.0, uTexel.y)).r;
    float ds = (d * 4.0 + dl + dr + dd + du) / 8.0;
    float cov = smoothstep(0.22, 0.45, ds);
    if (cov <= 0.0) discard;
    vec3 n = normalize(vec3(-vec2(dr - dl, du - dd) * 3.0, 1.0));
    vec3 L = normalize(vec3(-0.4, 0.7, 0.6));
    float dif = 0.78 + 0.22 * max(dot(n, L), 0.0);
    float spec = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 24.0);
    float edge = 1.0 - smoothstep(0.55, 1.2, ds);          // thin at the edges: a little translucent and bluish
    vec3 col = mix(vec3(0.97, 0.96, 0.92), vec3(0.86, 0.9, 0.98), edge * 0.5) * dif + spec * 0.6;
    float alpha = cov * mix(1.0, 0.85, edge);
    if (uWater > 0.5) {                                    // water: see-through blue in the thick of it, white foam where it thins out
        float foam = smoothstep(0.35, 0.9, edge);
        vec3 deep = vec3(0.32, 0.55, 0.78), lite = vec3(0.7, 0.86, 0.97);
        col = mix(mix(deep, lite, 0.35 + 0.65 * max(dot(n, L), 0.0)), vec3(0.95, 0.98, 1.0), foam * 0.8) + spec * 1.1;
        alpha = cov * mix(0.55, 0.9, foam);
    }
    o = vec4(col, alpha);
    o1 = vec4(0.0, 0.0, 0.0, cov);
}
)";

// The water cannon's jet as one continuous stream: a ribbon along its shots in flight (nozzle -> front), narrow at the
// nozzle and widening, a darker outline, light streaks flowing along it, foam where the front breaks up. Water, or the
// milk miko's milk.
static const char* kVsStream = R"(#version 330
uniform vec4 uPts[256];   // GL px x, y, half width (px), distance from the nozzle (world)
uniform int uCount;
uniform vec2 uFull;
out vec2 vUV;             // x: distance from the nozzle (world), y: -1..1 across
void main() {
    int i = gl_VertexID / 2;
    float side = (gl_VertexID & 1) == 0 ? -1.0 : 1.0;
    vec4 p = uPts[i];
    vec2 a = uPts[max(i - 1, 0)].xy, b = uPts[min(i + 1, uCount - 1)].xy;
    vec2 t = b - a;
    float l = length(t);
    t = l > 1e-4 ? t / l : vec2(1.0, 0.0);
    vUV = vec2(p.w, side);
    gl_Position = vec4((p.xy + vec2(-t.y, t.x) * side * p.z) / uFull * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char* kFsStream = R"(
layout(location = 1) out vec4 o1;
uniform float uTime;
uniform float uLen;       // its length (world)
uniform float uWater;     // 1 water, 0 milk
float sh(vec2 p) { p = fract(p * vec2(127.1, 311.7)); p += dot(p, p + 19.19); return fract(p.x * p.y); }
float sn(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(sh(i), sh(i + vec2(1, 0)), f.x), mix(sh(i + vec2(0, 1)), sh(i + vec2(1, 1)), f.x), f.y);
}
void main() {
    float u = vUV.x, v = vUV.y, av = abs(v);
    // long streaks running along the jet, flowing out from the nozzle (~1000 world units a second)
    float streak = smoothstep(0.6, 0.78, sn(vec2(u * 0.004 - uTime * 4.0, v * 5.0))) * 0.8 + smoothstep(0.68, 0.82, sn(vec2(u * 0.007 - uTime * 7.0 + 3.0, v * 9.0))) * 0.5;
    float body = 1.0 - smoothstep(0.86, 1.0, av);
    float rim = smoothstep(0.62, 0.9, av) * body;              // the jet's darker outline
    float start = smoothstep(0.0, 25.0, u), end = 1.0 - smoothstep(uLen - 60.0, uLen, u);
    vec3 col;
    float a;
    if (uWater > 0.5) {
        col = mix(vec3(0.42, 0.68, 0.92), vec3(0.85, 0.95, 1.0), clamp(streak, 0.0, 1.0));
        col = mix(col, vec3(0.1, 0.45, 0.85), rim * 0.8);
        a = body * mix(0.55, 0.9, max(clamp(streak, 0.0, 1.0), rim));
    } else {
        col = mix(vec3(0.95, 0.94, 0.9), vec3(1.0), clamp(streak, 0.0, 1.0) * 0.6);
        col = mix(col, vec3(0.78, 0.8, 0.86), rim * 0.6);
        a = body * 0.97;
    }
    a *= start * end;
    float foam = (1.0 - end) * smoothstep(0.5, 0.75, sn(vec2(u * 0.05 - uTime * 3.0, v * 4.0)));   // the front breaks into foam
    col = mix(col, vec3(1.0), foam * 0.7);
    a = max(a, foam * 0.8 * start * body);
    o = vec4(col, a);
    o1 = vec4(0.0, 0.0, 0.0, a);
}
)";

// Lasers and firebeams, layered like the orbital laser's beam: a wide soft halo, a coloured glow, a white-hot core,
// energy pulsing along it, a flicker - drawn additively over the game's own beam (kVsStream ribbon, start -> hit).
static const char* kFsBeam = R"(
layout(location = 1) out vec4 o1;
uniform float uTime;
uniform float uLen;
uniform vec4 uCol;
uniform float uI;
float bh(float x) { return fract(sin(x * 91.7) * 43758.5453); }
void main() {
    float u = vUV.x, av = abs(vUV.y);
    float core = exp(-av * av * 180.0);
    float glow = exp(-av * av * 18.0);
    float halo = exp(-av * av * 3.0) * 0.35;
    float pulse = 0.75 + 0.25 * sin(u * 0.03 - uTime * 40.0) * sin(u * 0.011 + uTime * 13.0);
    float flick = 0.85 + 0.15 * bh(floor(uTime * 30.0));
    float ends = smoothstep(0.0, 30.0, u) * (1.0 - smoothstep(uLen - 10.0, uLen, u));
    vec3 c = (vec3(1.0) * core * 1.6 + uCol.rgb * glow * 1.1 * pulse + uCol.rgb * halo) * flick * uI * ends;
    o = vec4(c, 0.0);
    o1 = vec4(c * 0.8, 0.0);
}
)";

// A textured quad in screen space (the miko's manga words, speed lines, sparkles), drawn after the final pass.
static const char* kVsSprite = R"(#version 330
uniform vec4 uRect;   // centre (GL px), half size (px)
uniform float uRot;
uniform vec2 uFull;
out vec2 vUV;
void main() {
    vec2 c = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    vec2 r = vec2(c.x * uRect.z, c.y * uRect.w);
    float cs = cos(uRot), sn = sin(uRot);
    vUV = c * 0.5 + 0.5;
    gl_Position = vec4((uRect.xy + vec2(r.x * cs - r.y * sn, r.x * sn + r.y * cs)) / uFull * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char* kFsSprite = R"(
uniform sampler2D uTexS;
uniform vec4 uTint;   // rgb multiply, a opacity
void main() { vec4 t = texture(uTexS, vec2(vUV.x, 1.0 - vUV.y)); o = vec4(t.rgb * uTint.rgb, t.a * uTint.a); }
)";

// Scorch marks, craters and molten marks where things really hit (the game's own impacts), drawn into the
// scene before lighting - only on surfaces (solid coverage), so they are lit like the structure they are on.
static const char* kVsDecal = R"(#version 330
uniform vec4 uDc[96];      // centre (GL screen px), radius px, age 0..1
uniform vec4 uDk[96];      // kind (0 scorch, 1 crater, 2 molten), strength, seed, heat 0..1
uniform vec2 uFull;
out vec2 vUV;
flat out vec4 vD;
flat out vec4 vK;
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    vD = uDc[gl_InstanceID];
    vK = uDk[gl_InstanceID];
    vUV = corner;
    gl_Position = vec4((vD.xy + corner * vD.z) / uFull * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFsDecal = R"(
layout(location = 1) out vec4 o1;
flat in vec4 vD;
flat in vec4 vK;
uniform sampler2D uSolid;
uniform sampler2D uEmis;   // what glows here (half res): the game's blasts and flames stay on top of the marks
uniform vec2 uFull;
float dh(vec2 p) { p = fract(p * vec2(127.1, 311.7)); p += dot(p, p + 19.19); return fract(p.x * p.y); }
float dn(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(dh(i), dh(i + vec2(1, 0)), f.x), mix(dh(i + vec2(0, 1)), dh(i + vec2(1, 1)), f.x), f.y);
}
void main() {
    float r = length(vUV);
    if (r > 1.0) discard;
    float sol = texture(uSolid, gl_FragCoord.xy / uFull).a;
    if (sol < 0.02) discard;
    int kind = int(vK.x + 0.5);
    float seed = vK.z, age = vD.w, heat = vK.w;
    vec2 q = vUV * 2.6 + seed;
    float n = dn(q) * 0.6 + dn(q * 2.7 + 5.0) * 0.4;
    float ang = atan(vUV.y, vUV.x);
    float streak = 0.7 + 0.3 * dn(vec2(ang * 3.0 + seed, r * 1.5));   // soot thrown out in streaks
    float edge = 1.0 - smoothstep(0.3, 1.0, r + (n - 0.5) * 0.6);
    float fade = 1.0 - smoothstep(0.55, 1.0, age);
    vec3 col = vec3(0.045, 0.04, 0.035);
    float a = edge * streak * (0.55 + 0.45 * n) * 0.7;
    if (kind == 1) {                       // crater: a darker bowl with a lighter, broken rim of thrown earth
        float bowl = 1.0 - smoothstep(0.0, 0.62, r + (n - 0.5) * 0.3);
        float rim = smoothstep(0.5, 0.68, r) * (1.0 - smoothstep(0.7, 0.95, r + (n - 0.5) * 0.25)) * step(0.35, n);
        col = mix(vec3(0.06, 0.05, 0.04), vec3(0.3, 0.25, 0.19), rim);
        a = max(a * 0.8, max(bowl * 0.75, rim * 0.5));
    } else if (kind == 2) {                // molten mark from a beam: small and glossy black
        a = (1.0 - smoothstep(0.2, 0.9, r + (n - 0.5) * 0.3)) * 0.85;
    }
    float glow = luma(texture(uEmis, gl_FragCoord.xy / uFull).rgb);
    a *= clamp(vK.y, 0.0, 1.6) * fade * clamp(sol * 1.5, 0.0, 1.0) * (1.0 - smoothstep(0.03, 0.25, glow));
    // still hot for a moment: thin glowing cracks in the char (a molten mark glows while the beam is on it)
    float cn = dn(vUV * 9.0 + seed * 1.7);
    float cracks = smoothstep(0.66, 0.72, cn) * (1.0 - smoothstep(0.66, 0.78, cn)) * 2.0 * (1.0 - smoothstep(0.0, 0.75, r));
    float hotA = heat * heat * (kind == 2 ? 1.0 - smoothstep(0.0, 0.6, r) : cracks) * clamp(sol * 1.5, 0.0, 1.0) * (1.0 - smoothstep(0.03, 0.25, glow));
    vec3 hot = mix(vec3(1.0, 0.3, 0.05), vec3(1.0, 0.78, 0.4), heat);
    o = vec4(mix(col, hot, clamp(hotA, 0.0, 1.0) * 0.85), max(a, hotA));
    o1 = vec4(hot * hotA * 0.4, 0.0);
}
)";

// GPU particles: state lives in two RGBA32F textures (pos.xy world, depth, life | vel.xyz, type).
// Types: 0 ember, 1 debris, 2 spark, 3 rain, 4 splash.
static const char* kFsParticleUpdate = R"(
layout(location = 1) out vec4 o1;
uniform sampler2D uPos;
uniform sampler2D uVel;
uniform sampler2D uSolid;
uniform vec4 uWorld;       // left, right, bottom, top
uniform vec4 uSim;         // dt, time, wind x, gravity
uniform vec4 uPrecip;      // x: what falls as type 6 (1 snow, 2 sand, 3 ash)
uniform vec4 uWaterP;      // water: on, surface (world y, y down)
uniform sampler2D uTerrain; // world terrain mask (in the snow layer's rect uSnowR)
uniform float uTerrainOn;
uniform sampler2D uSnowL;  // snow layer (r = fill), world-anchored
uniform float uSnowOn;
uniform vec4 uSnowR;       // its world rect
uniform vec4 uBlast[8];    // blasts: x, y (world), front radius (< 0: the air rushing back, within -z), push (world units/s^2)
uniform vec4 uBlastW[8];   // x: the front's width
uniform int uBlastN;
uniform sampler2D uScene;  // the game's frame (this frame, before the effects): what the surfaces look like
uniform sampler2D uEmis;   // what glows in it (the explosion and fire sprites)
uniform float uHitCol;     // 1: debris takes the colour of what it broke off
vec2 WUV(vec2 w) { return vec2((w.x - uWorld.x) / (uWorld.y - uWorld.x), (w.y - uWorld.z) / (uWorld.w - uWorld.z)); }
bool InView(vec2 uv) { return uv.x > 0.0 && uv.y > 0.0 && uv.x < 1.0 && uv.y < 1.0; }
float PackCol(vec3 c) { vec3 q = floor(clamp(c, 0.0, 1.0) * 63.0 + 0.5); return 1.0 + q.r * 4096.0 + q.g * 64.0 + q.b; }
// The colour of the front surface at uv (packed), 0 if there is none or it glows (the game's blast sprite covers the hit).
float SurfaceAt(vec2 uv) {
    if (!InView(uv) || texture(uSolid, uv).r < 0.5) return 0.0;
    if (dot(texture(uEmis, uv).rgb, vec3(0.3, 0.5, 0.2)) > 0.06) return 0.0;
    vec3 c = texture(uScene, uv).rgb;
    if (max(c.r, max(c.g, c.b)) < 0.13) return 0.0;   // the black ink outline round the game's sprites
    return PackCol(c);
}
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec4 p = texelFetch(uPos, c, 0), v = texelFetch(uVel, c, 0);
    o = p; o1 = v;
    if (p.w <= 0.0) return;
    float dt = uSim.x;
    int type = int(v.w + 0.5);
    if (type == 19) {   // a candidate: dust or grit shaken off a surface near a blast, kept only where there really is one
        // v.z: 0 grit off an underside, 1..999 a dust puff off an underside (its size), 1000+ dust off a top (size + 1000)
        o = vec4(p.xy, p.z, 0.0);                      // gone, unless it finds its surface
        vec2 uv0 = WUV(p.xy);
        if (!InView(uv0)) return;
        bool top = v.z > 999.5, inSolid = texture(uSolid, uv0).r > 0.5;
        // an underside: from inside a surface down to the air below it, from the air up to the surface above;
        // a top: from inside up to the air above it, from the air down to the surface below (world y grows down)
        float dir = top == inSolid ? -1.0 : 1.0, st = abs(uWorld.w - uWorld.z) * 0.004;
        vec2 last = p.xy;
        for (int k = 1; k <= 20; k++) {
            vec2 q = p.xy + vec2(0.0, dir * st * float(k)), quv = WUV(q);
            if (!InView(quv)) return;
            if ((texture(uSolid, quv).r > 0.5) != inSolid) {
                vec2 air = inSolid ? q : last, surf = inSolid ? last : q;
                if (v.z < 0.5) { o = vec4(air, fract(sin(dot(p.xy, vec2(0.37, 1.0))) * 4375.85) * 2.0 - 1.0, p.w); o1 = vec4(v.xy, SurfaceAt(WUV(surf)), 18.0); }
                else { o = vec4(air, p.w, p.w); o1 = vec4(v.xy, top ? v.z - 1000.0 : v.z, 11.0); }
                return;
            }
            last = q;
        }
        return;
    }
    if ((type == 1 || type == 13) && v.z < -0.5) {   // just broken off: the colour of a surface next to the hit
        float col = 0.0;
        if (uHitCol > 0.5) {
            float sd = fract(sin(dot(p.xy, vec2(12.9898, 78.233))) * 43758.5453), vh = abs(uWorld.w - uWorld.z);
            for (int i = 0; i < 12 && col == 0.0; i++) {     // rings out from the blast, a random one first
                float k = mod(float(i) + floor(sd * 12.0), 12.0), a = k * 2.39996 + sd * 6.2832;
                col = SurfaceAt(WUV(p.xy + vec2(cos(a), sin(a)) * vh * (0.006 + 0.016 * floor(k / 3.0))));
            }
        }
        v.z = col; o1 = v;
    }
    // blast wind: a front sweeping out from a blast shoves what is light enough (not what the blast itself just made at
    // its heart), then the air rushing back in draws things back a little
    float bw = (type == 8 || type == 9 || type == 11 || type == 12) ? 1.0 : type == 17 ? 1.3 : (type == 6 || type == 7) ? 0.8 :
               type == 3 ? 0.45 : (type == 0 || type == 5) ? 0.7 : type == 4 ? 0.5 : type == 16 ? 0.3 : type == 18 ? 0.35 :
               type == 2 ? 0.2 : (type == 1 || type == 13) ? 0.1 : 0.0;
    vec2 push = vec2(0.0);
    for (int i = 0; i < uBlastN && bw > 0.0; i++) {
        vec2 d = p.xy - uBlast[i].xy;
        float l = length(d), f;
        if (l < 1.0) continue;
        if (uBlast[i].z > 0.0) { float u = (l - uBlast[i].z) / max(uBlastW[i].x, 1.0); f = exp(-u * u * (u < 0.0 ? 9.0 : 1.0)); }   // sharp behind the front
        else { f = max(0.0, 1.0 + l / uBlast[i].z); f *= f; }
        push += d / l * uBlast[i].w * f;
    }
    push *= bw;
    if (type == 7 && length(push) > 2500.0) { o1 = vec4(push * 0.03, 0.0, 6.0); return; }   // settled snow blown off: it flies again
    v.xy += push * dt;
    vec2 acc = vec2(uSim.z * 0.3, uSim.w);
    float drag = 0.4;
    if (type == 0 || type == 5) { acc = vec2(uSim.z * 0.8 + sin(uSim.y * 3.0 + p.x * 0.05) * 40.0, -90.0); drag = 1.6; }   // embers rise
    else if (type == 2) { drag = 0.8; }
    else if (type == 13) { drag = 0.6; }                                              // wood splinter: tumbles and falls
    else if (type == 16) { acc = vec2(uSim.z * 0.1, uSim.w); drag = 0.25; }            // milk: falls, then runs along what it lands on
    else if (type == 17) {                                                            // sakura petal: flutters down on the wind
        acc = vec2(uSim.z * 0.8 + sin(uSim.y * 2.3 + p.z * 17.0) * 60.0, uSim.w * 0.06 + cos(uSim.y * 1.7 + p.z * 11.0) * 20.0);
        drag = 2.2;
    }
    else if (type == 14) { drag = 0.25; }                                             // molten slag: heavy, falls, bounces
    else if (type == 15) {                                                            // electric arc: crackles about, no weight
        float j = fract(sin(dot(p.xy, vec2(12.9898, 78.233)) + uSim.y * 91.7) * 43758.5453) - 0.5;
        v.xy += vec2(-v.y, v.x) * j * 9.0 * dt;
        acc = vec2(0.0); drag = 3.0;
    }
    else if (type == 3) { acc = vec2(uSim.z * 0.5, uSim.w * 1.4); drag = 0.0; }
    else if (type == 4) { drag = 1.0; }
    if (type == 7) { p.w -= dt; o = p; o1 = vec4(0.0, 0.0, 0.0, 7.0); return; }      // settled snowflake
    if (type == 10) { p.xy += v.xy * dt; p.w -= dt; o = p; o1 = v; return; }          // tracer: straight on, no gravity
    if (type == 8 || type == 9 || type == 11 || type == 12) {                         // steam / smoke / dust / hot gas puff
        float vh = abs(uWorld.w - uWorld.z);
        float lift = type == 8 ? 0.10 : type == 12 ? 0.13 : type == 11 ? -0.012 : 0.05;  // dust settles, hot gas rises fast
        acc = vec2(uSim.z * 0.6 + sin(p.y * 0.006 + uSim.y * 0.9 + v.z * 0.1) * vh * 0.035, -vh * lift);
        drag = type == 11 ? 1.8 : 1.3;
    }
    if (type == 6) {                                                                  // snowflake: sways, falls steadily
        float vw = abs(uWorld.y - uWorld.x);
        float sway = uPrecip.x > 2.5 ? 0.05 : uPrecip.x > 1.5 ? 0.0 : 0.03;    // ash drifts most, sand flies straight
        v.x += sin(uSim.y * 1.3 + p.z * 9.0 + p.y * 0.004) * vw * sway * dt;
        if (uPrecip.x < 1.5 || uPrecip.x > 2.5) v.x *= exp(-0.8 * dt);
        acc = vec2(0.0); drag = 0.0;
    }
    v.xy += acc * dt;
    v.xy *= exp(-drag * dt);
    vec2 np = p.xy + v.xy * dt;
    // collide with real terrain / structures (this frame's solid mask)
    vec2 uv = vec2((np.x - uWorld.x) / (uWorld.y - uWorld.x), (np.y - uWorld.z) / (uWorld.w - uWorld.z));
    if (uWaterP.x > 0.5 && np.y > uWaterP.y && p.y <= uWaterP.y + 1.0) {          // reaching the water
        if (type == 3) { o = vec4(np.x, uWaterP.y, p.z, 0.25); o1 = vec4(v.x * 0.1, -abs(v.y) * 0.12, 0.0, 4.0); return; }   // rain: a splash
        if (type == 0 || type == 2 || type == 5 || type == 6 || type == 14 || type == 15 || type == 16 || type == 18) { o = vec4(np, p.z, 0.0); o1 = v; return; }   // put out / melted / mixed in
    }
    if (uWaterP.x > 0.5 && np.y > uWaterP.y && (type == 1 || type == 13)) { v.xy *= exp(-4.0 * dt); np = p.xy + v.xy * dt; }   // debris sinks slowly
    bool collides = type == 1 || type == 2 || type == 3 || type == 4 || type == 6 || type == 13 || type == 14 || type == 16 || type == 18;
    vec4 sm = uv.x > 0.0 && uv.y > 0.0 && uv.x < 1.0 && uv.y < 1.0 ? texture(uSolid, uv) : vec4(0.0);
    float hit = (type == 3 || type == 6 || type == 18) ? sm.r : sm.a;   // rain, snow and grit only land on front surfaces
    if ((type == 3 || type == 6) && uTerrainOn > 0.5) {      // ...and on the real terrain shape, also off screen
        vec2 tuv = vec2((np.x - uSnowR.x) / (uSnowR.y - uSnowR.x), (np.y - uSnowR.z) / (uSnowR.w - uSnowR.z));
        if (tuv.x > 0.0 && tuv.y > 0.0 && tuv.x < 1.0 && tuv.y < 1.0) hit = max(hit, step(0.5, texture(uTerrain, tuv).r));
    }
    if (type == 6 && uSnowOn > 0.5) {                        // ...and on snow already lying there, so piles grow
        vec2 suv = vec2((np.x - uSnowR.x) / (uSnowR.y - uSnowR.x), (np.y - uSnowR.z) / (uSnowR.w - uSnowR.z));
        if (suv.x > 0.0 && suv.y > 0.0 && suv.x < 1.0 && suv.y < 1.0) hit = max(hit, step(0.5, texture(uSnowL, suv).r));
    }
    if (collides && hit > 0.5) {
        if (type == 3) {                                  // rain drop -> splash
            o = vec4(p.xy, p.z, 0.25);
            o1 = vec4(v.x * 0.1, -abs(v.y) * 0.12, 0.0, 4.0);
            return;
        }
        if (type == 6) {                                  // snowflake lands and lies there a moment
            vec2 puv = vec2((p.x - uWorld.x) / (uWorld.y - uWorld.x), (p.y - uWorld.z) / (uWorld.w - uWorld.z));
            vec4 sp = texture(uSolid, puv);
            bool shelter = sp.a > 0.5 && sp.r < 0.3;           // inside a fort (its back wall)
            for (int k = 1; k <= 16 && !shelter; k++) {         // or under a strut / roof: only tops keep snow
                vec2 q = vec2(puv.x, (p.y - 38.0 * float(k) - uWorld.z) / (uWorld.w - uWorld.z));
                if (q.y < 0.0 || q.y > 1.0) break;
                shelter = texture(uSolid, q).a > 0.3;
            }
            if (shelter) { o = vec4(p.xy, p.z, 0.0); o1 = v; return; }   // gone, no pile in there
            o = vec4(p.xy, p.z, min(p.w, 2.5));
            o1 = vec4(0.0, 0.0, 0.0, 7.0);
            return;
        }
        vec2 e = vec2(4.0 / 3840.0, 4.0 / 2160.0);
        vec2 g = vec2(texture(uSolid, uv + vec2(e.x, 0)).a - texture(uSolid, uv - vec2(e.x, 0)).a,
                      texture(uSolid, uv + vec2(0, e.y)).a - texture(uSolid, uv - vec2(0, e.y)).a);
        // uv y grows up the screen, world y grows down it
        vec2 nrm = length(g) > 1e-4 ? -normalize(vec2(g.x, -g.y)) : vec2(0.0, -1.0);
        float restitution = type == 2 ? 0.35 : type == 14 ? 0.22 : type == 18 ? 0.15 : 0.3;
        if (type == 16) {                                 // a liquid: loses what goes into the surface, runs along it
            if (dot(v.xy, nrm) < 0.0) v.xy -= nrm * dot(v.xy, nrm);
            v.xy *= 0.9;
            p.xy = p.xy + nrm * 1.0 + v.xy * dt;
            p.w -= dt;
            o = p; o1 = v;
            return;
        }
        if (dot(v.xy, nrm) < 0.0) v.xy = reflect(v.xy, nrm) * restitution;
        np = p.xy + nrm * 2.0;
        if (length(v.xy) < 25.0) v.xy = vec2(0.0);       // came to rest
    }
    p.xy = np;
    p.w -= dt;
    o = p; o1 = v;
}
)";

static const char* kVsParticle = R"(#version 330
uniform sampler2D uPos;
uniform sampler2D uVel;
uniform vec4 uWorld;
uniform vec2 uFull;
uniform int uWidth;
uniform int uPass;         // 0 = glowing (embers, sparks), 1 = solid (debris, rain, splashes)
uniform float uScale;      // world units -> px at this zoom
uniform float uTime;
out vec2 vUV;
flat out vec4 vInfo;       // type, life, depth, speed
void main() {
    ivec2 c = ivec2(gl_InstanceID % uWidth, gl_InstanceID / uWidth);
    vec4 p = texelFetch(uPos, c, 0), v = texelFetch(uVel, c, 0);
    int type = int(v.w + 0.5);
    bool glow = type == 0 || type == 2 || type == 5 || type == 10 || type == 14 || type == 15;
    bool milk = type == 16;   // pass 2: only milk, into its density target
    bool show = uPass == 2 ? milk : (!milk && glow == (uPass == 0));
    if (p.w <= 0.0 || !show || type == 19) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vUV = vec2(0); vInfo = vec4(0); return; }
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    float depth = 1.0 + p.z * 0.6;
    vec2 scr = vec2((p.x - uWorld.x) / (uWorld.y - uWorld.x), (p.y - uWorld.z) / (uWorld.w - uWorld.z)) * uFull;
    vec2 vel = vec2(v.x, -v.y) * uScale;                  // px/s, GL screen y up
    float sp = length(vel);
    vec2 along = sp > 1.0 ? vel / sp : vec2(1.0, 0.0);
    vec2 side = vec2(-along.y, along.x);
    float size = (type == 0 ? 5.0 : type == 1 ? 9.0 : type == 2 ? 3.0 : type == 3 ? 1.7 : type == 4 ? 3.4 : type == 5 ? 3.2 :
                  type == 13 ? 6.0 : type == 14 ? 3.4 : type == 15 ? 1.5 : type == 17 ? 9.0 : type == 18 ? 2.4 : type >= 6 ? 2.8 : 2.5) * depth * uFull.y / 2160.0;
    float len = size;
    if (type == 2) len = size + sp * 0.022;               // motion streaks
    if (type == 13) { len = size * 1.6; size *= 0.3; }    // a long thin splinter
    if (type == 14) len = size + sp * 0.008;              // a molten drop stretches a little as it flies
    if (type == 15) len = size + sp * 0.03;               // an arc is a thin crackling streak
    if (type == 16) { size = max(v.z * uScale, 1.5); len = size + sp * 0.012; }   // milk: world-sized blobs, stretched by speed
    if (type == 17) len = size * 1.4;
    if (type == 18) len = size + sp * 0.006;             // grit: a speck, a little streaked as it falls
    if (type == 3) len = size + sp * 0.015;
    if (type == 10) { size = 2.2 * uFull.y / 2160.0; len = size + sp * 0.03; }   // tracer streak
    if ((type >= 6 && type <= 12)) { along = vec2(1.0, 0.0); side = vec2(0.0, 1.0); }   // round flakes and puffs
    if (type == 17) {                                      // a petal turns slowly as it falls
        float a = uTime * (1.2 + p.z) + float(gl_InstanceID);
        along = vec2(cos(a), sin(a)); side = vec2(-along.y, along.x) * (0.55 + 0.45 * abs(sin(uTime * 2.0 + p.z * 5.0)));
    }
    float age = 0.0;
    bool puff = type == 8 || type == 9 || type == 11 || type == 12;
    if (puff) {                          // pos.z = life at spawn, vel.z = size at spawn (world units)
        age = clamp(1.0 - p.w / max(p.z, 1e-3), 0.0, 1.0);
        size = abs(v.z) * uScale * (0.55 + 1.7 * sqrt(age));   // (< 0: a thin dust haze)
        len = size;
    }
    if (type == 1) { size *= 0.6 + 0.9 * fract(abs(p.z) * 7.31); len = size; }   // chunks of all sizes
    if (type == 1 || type == 13) {                         // debris and splinters tumble
        float a = uTime * (4.0 + p.z * 3.0) + float(gl_InstanceID);
        along = vec2(cos(a), sin(a)); side = vec2(-along.y, along.x);
    }
    vec2 px = scr + along * corner.x * len + side * corner.y * size;
    if (type == 10) px = scr + along * (corner.x - 1.0) * len + side * corner.y * size;   // all behind the projectile
    gl_Position = vec4(px / uFull * 2.0 - 1.0, 0.0, 1.0);
    vUV = corner;
    vInfo = puff ? vec4(float(type), p.w, age, float(gl_InstanceID % 997) + (v.z < 0.0 ? 1000.0 : 0.0)) :
            type == 10 ? vec4(10.0, p.w, v.z, sp) : (type == 6 || type == 7) ? vec4(float(type), p.w, depth, v.z > 0.0 ? v.z - p.w : 99.0) :
            (type == 1 || type == 13 || type == 18) ? vec4(float(type), p.w, depth, v.z) :   // w: the colour of what it came off (packed)
            vec4(float(type), p.w, depth, sp);
}
)";

static const char* kFsParticle = R"(
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;   // g = smoke density (lit smoke in the composite)
flat in vec4 vInfo;
uniform vec4 uLook;        // ember brightness, spark brightness, saturation, unused
uniform vec4 uPrecipCol;   // snow / sand / ash colour, unused
float ph(vec2 p) { p = fract(p * vec2(127.1, 311.7)); p += dot(p, p + 19.19); return fract(p.x * p.y); }
float pn(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(ph(i), ph(i + vec2(1, 0)), f.x), mix(ph(i + vec2(0, 1)), ph(i + vec2(1, 1)), f.x), f.y);
}
vec3 UnpackCol(float f) { float c = f - 1.0; return vec3(floor(c / 4096.0), floor(mod(c, 4096.0) / 64.0), mod(c, 64.0)) / 63.0; }
void main() {
    int type = int(vInfo.x + 0.5);
    float life = vInfo.y;
    float r = length(vUV);
    o2 = vec4(0.0);
    if (type == 16) {                                      // milk: density for the liquid surface (pass 2)
        float k = max(0.0, 1.0 - r * r);
        o = vec4(k * k * clamp(life * 2.0, 0.0, 1.0), 0.0, 0.0, 0.0); o1 = vec4(0.0);
        return;
    }
    if (type == 17) {                                      // sakura petal: pink, paler in the middle, a notch at the tip
        float a = 1.0 - smoothstep(0.7, 1.0, length(vec2(vUV.x, vUV.y * 1.5)));
        a *= smoothstep(0.1, 0.35, length(vUV - vec2(1.0, 0.0)));
        a *= clamp(life * 1.5, 0.0, 1.0);
        vec3 col = mix(vec3(0.98, 0.62, 0.74), vec3(1.0, 0.9, 0.94), 1.0 - r);
        o = vec4(col, a); o1 = vec4(0.0, 0.0, 0.0, a);
        return;
    }
    if (type == 10) {                                      // tracer: white-hot head, coloured glowing tail
        float along = vUV.x * 0.5 + 0.5;                   // 0 tail .. 1 head
        float a = smoothstep(0.0, 0.85, along) * (1.0 - smoothstep(0.35, 1.0, abs(vUV.y)));
        int kind = int(vInfo.z + 0.5);
        vec3 tint = kind == 0 ? vec3(1.0, 0.85, 0.45) : kind == 1 ? vec3(1.0, 0.6, 0.25) : vec3(1.0, 0.45, 0.2);
        vec3 col = mix(tint, vec3(1.0, 0.97, 0.9), smoothstep(0.75, 1.0, along)) * uLook.y * 0.9;
        o = vec4(col * a, 0.0); o1 = vec4(col * a * 0.8, 0.0);
    } else if (type == 5) {                                       // fire ember: small, dim, mostly lights itself
        float a = (1.0 - smoothstep(0.15, 1.0, r)) * clamp(life * 1.2, 0.0, 1.0) * 0.8;
        vec3 col = mix(vec3(1.0, 0.3, 0.05), vec3(1.0, 0.65, 0.25), clamp(life - 0.5, 0.0, 1.0)) * uLook.x * 0.35;
        o = vec4(col * a, 0.0); o1 = vec4(col * a * 0.4, 0.0);
    } else if (type == 0) {                                // ember: soft hot dot, cools to red
        float a = (1.0 - smoothstep(0.2, 1.0, r)) * clamp(life * 1.5, 0.0, 1.0);
        vec3 col = mix(vec3(1.0, 0.25, 0.04), vec3(1.0, 0.75, 0.3), clamp(life - 0.4, 0.0, 1.0)) * uLook.x;
        o = vec4(col * a, 0.0); o1 = vec4(col * a, 0.0);
    } else if (type == 2) {                                // spark: white-hot streak
        float a = (1.0 - smoothstep(0.3, 1.0, abs(vUV.y))) * (1.0 - smoothstep(0.6, 1.0, abs(vUV.x))) * clamp(life * 3.0, 0.0, 1.0);
        vec3 col = vec3(1.0, 0.82, 0.5) * uLook.y;
        o = vec4(col * a, 0.0); o1 = vec4(col * a, 0.0);
    } else if (type == 1) {                                // debris: an irregular chunk of rock, wood or metal, lit from above
        float seed = vInfo.z * 17.31;
        float ang = atan(vUV.y, vUV.x) / 6.2831853 + 0.5;
        float k = 5.0 + floor(fract(seed * 0.37) * 3.0);     // 5 to 7 corners
        float i0 = floor(ang * k), f = fract(ang * k);
        float h0 = 0.5 + 0.45 * fract(sin((i0 + seed) * 12.9898) * 43758.5453);
        float h1 = 0.5 + 0.45 * fract(sin((mod(i0 + 1.0, k) + seed) * 12.9898) * 43758.5453);
        float edgeR = mix(h0, h1, f);                        // straight edges between jagged corners
        float a = 1.0 - smoothstep(edgeR - 0.08, edgeR, r);
        int kind = int(fract(seed * 0.71) * 3.0);
        vec3 base = kind == 0 ? vec3(0.30, 0.27, 0.24) : kind == 1 ? vec3(0.36, 0.24, 0.14) : vec3(0.30, 0.31, 0.33);
        if (vInfo.w > 0.5) base = mix(base, UnpackCol(vInfo.w) * 1.15, 0.85);   // a chunk of what was hit
        float lit = clamp(vUV.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 col = base * (0.35 + 0.75 * lit) * (1.0 - 0.35 * smoothstep(edgeR - 0.25, edgeR, r));
        float fade = clamp(life, 0.0, 1.0);
        o = vec4(col, a * fade); o1 = vec4(0.0, 0.0, 0.0, a * fade);
    } else if (type == 8 || type == 9 || type == 11 || type == 12) {   // steam / smoke / dust / hot gas: a soft billowing puff, lit later by the scene
        float age = vInfo.z;
        vec2 q = vUV * 1.7 + vec2(vInfo.w * 0.371, vInfo.w * 0.113) + age * vec2(0.2, 0.5);
        float nz = pn(q) * 0.6 + pn(q * 2.3 + 4.0) * 0.4;
        float a = (1.0 - smoothstep(0.2, 1.0, r + (nz - 0.5) * 0.55)) * smoothstep(0.0, 0.12, age) * (1.0 - smoothstep(0.4, 1.0, age));
        vec3 col = type == 8 ? vec3(0.88, 0.9, 0.93) : type == 11 ? vec3(0.5, 0.43, 0.33) : type == 12 ? vec3(0.2, 0.19, 0.18) : vec3(0.27, 0.26, 0.25);
        float hot = type == 12 ? 1.0 - smoothstep(0.05, 0.42, age) : 0.0;   // hot gas: glows orange, cools into dark smoke
        col = mix(col, vec3(1.0, 0.56, 0.2), hot * 0.85);
        col *= 0.8 + 0.35 * clamp(vUV.y * 0.5 + 0.5, 0.0, 1.0) + (nz - 0.5) * 0.25;   // lit from above, billows
        float op = (type == 8 ? 0.42 : type == 11 ? 0.3 : type == 12 ? 0.6 : 0.5) * (vInfo.w > 999.5 ? 0.4 : 1.0);   // a dust haze is thin
        o = vec4(col, a * op); o1 = vec4(vec3(1.0, 0.5, 0.15) * hot * 4.0, a * op);   // a: covers the playfield (blended by it)
        // how much light it scatters (lit smoke in the composite): steam glows, soot mostly swallows light
        float scatter = type == 8 ? 1.0 : type == 11 ? 0.55 : 0.3;
        o2 = vec4(0.0, a * op * scatter, a * op, 0.0);   // g: lit-smoke density, b: puff coverage
    } else if (type == 13) {                               // wood splinter, lit from above
        float a = 1.0 - smoothstep(0.7, 1.0, max(abs(vUV.x), abs(vUV.y)));
        float lit = clamp(vUV.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 col = mix(vec3(0.20, 0.13, 0.07), vec3(0.55, 0.40, 0.24), lit);
        if (vInfo.w > 0.5) col = mix(col, UnpackCol(vInfo.w) * (0.5 + 0.75 * lit), 0.8);   // the bracing's own colour
        o = vec4(col, a * clamp(life, 0.0, 1.0)); o1 = vec4(0.0, 0.0, 0.0, a * clamp(life, 0.0, 1.0));
    } else if (type == 18) {                               // grit shaken off a surface: a speck of it
        float a = (1.0 - smoothstep(0.55, 1.0, r)) * clamp(life * 2.0, 0.0, 1.0);
        vec3 col = (vInfo.w > 0.5 ? UnpackCol(vInfo.w) * 0.85 : vec3(0.32, 0.29, 0.25)) * (0.7 + 0.45 * clamp(vUV.y * 0.5 + 0.5, 0.0, 1.0));
        o = vec4(col, a); o1 = vec4(0.0, 0.0, 0.0, a);
    } else if (type == 14) {                               // molten slag: white-hot, cooling to dull red as it falls
        float a = (1.0 - smoothstep(0.25, 1.0, r)) * clamp(life * 2.0, 0.0, 1.0);
        float heat = clamp(life / 1.3, 0.0, 1.0);
        vec3 col = mix(vec3(0.7, 0.12, 0.02), mix(vec3(1.0, 0.55, 0.15), vec3(1.0, 0.95, 0.8), smoothstep(0.6, 1.0, heat)), smoothstep(0.1, 0.6, heat)) * uLook.y;
        o = vec4(col * a, 0.0); o1 = vec4(col * a, 0.0);
    } else if (type == 15) {                               // electric arc: blue-white, flickering
        float a = (1.0 - smoothstep(0.2, 1.0, abs(vUV.y))) * (1.0 - smoothstep(0.6, 1.0, abs(vUV.x))) * clamp(life * 5.0, 0.0, 1.0);
        a *= step(0.3, fract(sin(vInfo.w * 0.37 + life * 61.0) * 437.58));
        vec3 col = vec3(0.65, 0.85, 1.0) * uLook.y * 1.3;
        o = vec4(col * a, 0.0); o1 = vec4(col * a, 0.0);
    } else if (type >= 6) {                                // snowflake (falling or settled)
        float a = (1.0 - smoothstep(0.3, 1.0, r)) * clamp(life * 0.8, 0.0, 1.0) * (0.55 + 0.35 * clamp(vInfo.z - 0.4, 0.0, 1.0))
                * clamp(vInfo.w / 0.6, 0.0, 1.0);   // fades in where it appeared
        o = vec4(uPrecipCol.rgb, a); o1 = vec4(0.0);
    } else {                                               // rain streak / splash
        float a = (1.0 - smoothstep(0.2, 1.0, abs(vUV.y))) * (1.0 - smoothstep(0.5, 1.0, abs(vUV.x)));
        a *= type == 4 ? clamp(life * 4.0, 0.0, 1.0) * 0.8 : 0.32 * (0.55 + 0.45 * vInfo.z);
        o = vec4(vec3(0.84, 0.89, 0.96), a); o1 = vec4(0.0);
    }
}
)";
// Lightning bolt segments as glowing quads (white core, blue glow), additive into scene + light sources.
static const char* kVsBolt = R"(#version 330
uniform vec4 uSeg[128];    // GL screen px: x0, y0, x1, y1
uniform vec4 uWid[128];    // width px, intensity
uniform vec2 uFull;
out vec2 vUV;
flat out vec4 vInfo;
void main() {
    vec4 s = uSeg[gl_InstanceID];
    vec4 w = uWid[gl_InstanceID];
    vec2 a = s.xy, b = s.zw, d = b - a;
    float len = max(length(d), 1e-3);
    vec2 dir = d / len, side = vec2(-dir.y, dir.x);
    float glow = w.x * 7.0;
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? 0.0 : 1.0, (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    vec2 px = a + dir * (corner.x * (len + glow) - glow * 0.5) + side * corner.y * glow;
    gl_Position = vec4(px / uFull * 2.0 - 1.0, 0.0, 1.0);
    vUV = vec2(corner.y * glow / w.x, 0.0);
    vInfo = w;
}
)";

static const char* kFsBolt = R"(
layout(location = 1) out vec4 o1;
flat in vec4 vInfo;
void main() {
    float d = abs(vUV.x);                                  // distance from the core in core widths
    float core = exp(-d * d * 1.5);
    float glow = exp(-d * 0.55) * 0.35;
    vec3 col = (vec3(1.0, 1.0, 1.0) * core * 1.6 + vec3(0.45, 0.6, 1.0) * glow * 1.2) * vInfo.y;
    o = vec4(col, 0.0);
    o1 = vec4(col * 0.35, 0.0);
}
)";
// ================================================================ hybrid 3D backdrop
// Replaces the farthest painted layers: sky gradient, sun, stars, two drifting cloud decks lit by
// the sun (and lightning), and up to three ranges of procedural mountains with real parallax,
// slope shading, rim light and aerial perspective. Colours come from the replaced layers' textures.
// Space: "layer units" = world units / reference view height, y up; each range has a parallax
// factor p (0 = infinitely far) that controls how much it follows the camera.
static const char* kFsBackdrop = R"(
uniform vec4 uCam;        // camera centre x, y (world, y up), view width, view height (world)
uniform vec4 uRef;        // reference view width, height (world), horizon offset, parallax scale
uniform vec4 uSkyTop;
uniform vec4 uSkyHor;
uniform vec4 uMtn[3];     // far -> near range colours
uniform vec4 uSunB;       // sun dir xy, sun glow, cloud amount
uniform vec4 uSunCol;
uniform vec4 uMisc;       // time, stars, mountain height, ranges
uniform vec4 uFlashB;     // lightning flash amount, flash x (uv), cloud speed, snow cover
uniform vec4 uFuji;       // Mount Fuji: on, centre (layer x), half width, height
uniform vec2 uFull;
float hb(vec2 p) { p = fract(p * vec2(234.34, 435.345)); p += dot(p, p + 34.23); return fract(p.x * p.y); }
float vnb(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hb(i), hb(i + vec2(1, 0)), f.x), mix(hb(i + vec2(0, 1)), hb(i + vec2(1, 1)), f.x), f.y);
}
float fbmb(vec2 p, int oct) { float v = 0.0, a = 0.5; for (int i = 0; i < 6; i++) { if (i >= oct) break; v += a * vnb(p); p = p * 2.02 + 7.7; a *= 0.5; } return v; }
float ridged(float x, float seed) {
    float v = 0.0, a = 0.55, f = 1.0;
    for (int i = 0; i < 6; i++) {
        float n = vnb(vec2(x * f + seed, seed * 1.7 + float(i) * 3.1));
        n = 1.0 - abs(n * 2.0 - 1.0);
        v += a * n * n;
        f *= 2.1; a *= 0.5;
    }
    return v;
}
// rock relief across a mountain face (ridged, both axes)
float rock(vec2 q) {
    float v = 0.0, a = 0.5;
    for (int k = 0; k < 4; k++) { float r = 1.0 - abs(vnb(q) * 2.0 - 1.0); v += a * r * r; q = q * 2.03 + 5.1; a *= 0.5; }
    return v;
}
vec2 layer(vec2 uv, float p) {
    float aspect = uFull.x / uFull.y;
    float pw = mix(uRef.x, uCam.z, clamp(p * uRef.w, 0.0, 1.0));
    float ph = pw / aspect;
    vec2 w = vec2(uCam.x * p + (uv.x - 0.5) * pw, uCam.y * p + (uv.y - 0.5) * ph);
    return w / uRef.y + vec2(0.0, uRef.z);
}
void main() {
    vec2 uv = vUV;
    float aspect = uFull.x / uFull.y;
    vec3 sunDir3 = normalize(vec3(uSunB.xy, 0.55));
    // sky
    vec3 col = mix(uSkyHor.rgb, uSkyTop.rgb, smoothstep(0.2, 1.0, uv.y));
    vec2 sunUV = vec2(0.5 + uSunB.x * 0.36, 0.55 + uSunB.y * 0.3);
    vec2 ds = (uv - sunUV) * vec2(aspect, 1.0);
    float sd = length(ds);
    col += uSunCol.rgb * (smoothstep(0.035, 0.028, sd) * 2.5 + exp(-sd * 7.0) * 0.45 * uSunB.z);
    float dark = 1.0 - smoothstep(0.06, 0.3, dot(uSkyTop.rgb, vec3(0.3, 0.5, 0.2)));
    if (uMisc.y > 0.0 && dark > 0.0) {
        vec2 sp = uv * vec2(aspect, 1.0) * 420.0;
        float st = step(0.9965, hb(floor(sp))) * (0.5 + 0.5 * sin(uMisc.x * 2.0 + hb(floor(sp) + 3.0) * 30.0));
        col += vec3(st) * dark * uMisc.y * smoothstep(0.3, 0.7, uv.y);
    }
    // clouds: two decks, the far one slower
    for (int c = 0; c < 2; c++) {
        float p = c == 0 ? 0.03 : 0.09;
        vec2 L = layer(uv, p);
        vec2 q = L * (c == 0 ? 1.1 : 1.8) + vec2(uMisc.x * uFlashB.z * (c == 0 ? 0.01 : 0.02), 0.0);
        float d = fbmb(q * vec2(1.0, 2.2), 5) - (1.0 - uSunB.w) * 0.55 - 0.15;
        float band = smoothstep(0.32, 0.62, uv.y) * (1.0 - smoothstep(0.95, 1.05, uv.y));
        d = smoothstep(0.0, 0.3, d) * band;
        if (d <= 0.001) continue;
        float d2 = fbmb((q + uSunB.xy * 0.06) * vec2(1.0, 2.2), 4);
        float shade = clamp(0.55 + (fbmb(q * vec2(1.0, 2.2), 4) - d2) * 4.0, 0.0, 1.0);
        vec3 cc = mix(uSkyHor.rgb * 0.7, mix(uSkyTop.rgb, uSunCol.rgb, 0.55) * 1.15, shade);
        cc += vec3(0.75, 0.85, 1.0) * uFlashB.x * (0.7 + 0.6 * exp(-abs(uv.x - uFlashB.y) * 5.0)) * (0.5 + shade);
        col = mix(col, cc, d * (c == 0 ? 0.65 : 0.85));
    }
    if (uFuji.x > 0.5) {   // Mount Fuji, far behind the ranges: concave flanks, a flat crater top, a streaked snow cap
        vec2 L = layer(uv, 0.02);
        float dx = abs(L.x - uFuji.y) / uFuji.z;
        float prof = uFuji.w * pow(max(1.0 - dx, 0.0), 1.55);
        float top = -0.05 + min(prof, uFuji.w * 0.93);
        float pxL = (mix(uRef.x, uCam.z, clamp(0.02 * uRef.w, 0.0, 1.0)) / aspect) / uRef.y / uFull.y;
        float inside = smoothstep(-pxL, pxL, top - L.y);
        if (inside > 0.0) {
            float h = (L.y + 0.05) / max(uFuji.w, 1e-3);
            float streak = vnb(vec2(L.x * 40.0 / uFuji.z, 3.0)) * 0.12 + vnb(vec2(L.x * 9.0 / uFuji.z, 7.0)) * 0.08;
            float snow = smoothstep(0.6 - streak, 0.64 - streak, h);
            float side = clamp(0.5 + (L.x - uFuji.y) / uFuji.z * 0.9 * sign(uSunB.x + 1e-3), 0.0, 1.0);   // the sunward flank lighter
            vec3 fc = mix(mix(vec3(0.24, 0.3, 0.46), vec3(0.36, 0.42, 0.58), side), mix(vec3(0.78, 0.84, 0.95), vec3(1.0), side), snow);
            col = mix(col, mix(fc, uSkyHor.rgb, 0.45), inside);
        }
    }
    // mountain ranges, far to near
    for (int i = 0; i < 3; i++) {
        if (float(i) >= uMisc.w) break;
        float p = i == 0 ? 0.035 : i == 1 ? 0.075 : 0.13;
        float scale = i == 0 ? 1.4 : i == 1 ? 0.9 : 0.55;
        float base = -0.02 - float(i) * 0.07;
        float amp = uMisc.z * (i == 0 ? 0.42 : i == 1 ? 0.32 : 0.22);
        vec2 L = layer(uv, p);
        float seed = float(i) * 13.7 + 2.0;
        float r0 = ridged(L.x / scale, seed);
        float top = base + amp * r0;
        float pxL = (mix(uRef.x, uCam.z, clamp(p * uRef.w, 0.0, 1.0)) / aspect) / uRef.y / uFull.y;
        float inside = smoothstep(-pxL, pxL, top - L.y);
        if (inside <= 0.0) continue;
        float e = 0.004;
        float slope = (ridged((L.x + e) / scale, seed) - r0) / e * amp;
        float depthIn = clamp((top - L.y) / max(amp, 1e-3), 0.0, 1.5);
        // face normal: ridge slope near the crest, rock relief everywhere
        vec2 q = vec2(L.x, L.y * 1.6) / scale * 1.25 + seed;
        float f0 = rock(q), fx = rock(q + vec2(0.03, 0.0)), fy = rock(q + vec2(0.0, 0.03));
        vec2 g = vec2(fx - f0, fy - f0) / 0.03;
        vec3 nrm = normalize(vec3(-g.x * 0.14 - slope * 1.3 * exp(-depthIn * 2.5), -g.y * 0.12 + 0.3, 0.8));
        float lit = clamp(dot(nrm, sunDir3), 0.0, 1.0);
        float band = 0.6 + 0.4 * smoothstep(0.36, 0.42, lit) + 0.2 * smoothstep(0.74, 0.79, lit);     // firm toon bands
        vec3 base3 = uMtn[i].rgb;
        vec3 mc = base3 * band * mix(1.08, 0.82, smoothstep(0.0, 1.2, depthIn));                    // lit tops, darker foot
        if (uFlashB.w > 0.0) {                                                                    // snow-capped in snowy weather
            float line = 0.25 + 0.5 * uFlashB.w + 0.12 * (vnb(vec2(L.x * 6.0 / scale, seed)) - 0.5);
            float snowc = (1.0 - smoothstep(line - 0.06, line + 0.06, depthIn)) * smoothstep(-0.25, 0.25, nrm.y + 0.15);
            mc = mix(mc, vec3(0.90, 0.93, 1.0) * band, clamp(snowc * uFlashB.w * 1.3, 0.0, 0.95));
        }
        mc += uSunCol.rgb * smoothstep(0.05, 0.0, depthIn) * lit * 0.3;                          // rim on the ridge line
        mc = mix(mc, uSkyHor.rgb, (i == 0 ? 0.5 : i == 1 ? 0.32 : 0.16));                           // aerial perspective
        mc = mix(mc, uSkyHor.rgb * 0.95, smoothstep(0.5, 1.4, depthIn) * 0.5);                      // haze at the foot
        mc += vec3(0.75, 0.85, 1.0) * uFlashB.x * 0.35 * lit;
        col = mix(col, mc, inside);
    }
    o = vec4(clamp(col, 0.0, 4.0), 1.0);
}
)";

// ================================================================ snow physics
// Snow lives in a world-anchored layer (r = how full each cell is, 0..1) covering a large region around
// the battle, so it stays where it fell when the camera pans away and back. Each frame only the visible
// rectangle is simulated (into a scratch target, then copied back): settled flakes deposit, and a
// falling-sand step moves the snow - it falls into free cells below, slides diagonally off edges and
// slopes, piles up on front surfaces, collapses when what holds it is destroyed, is limited to a maximum
// depth and melts. Off-screen snow is kept as it was.
static const char* kVsSnowDeposit = R"(#version 330
uniform sampler2D uPos;
uniform sampler2D uVel;
uniform int uWidth;
uniform vec4 uRegion;      // world rect of the snow layer: left, right, bottom, top
uniform vec2 uMSize;       // layer size (cells)
uniform vec2 uOrg;         // scratch origin in layer cells
uniform vec2 uSSize;       // scratch size (cells)
out vec2 vUV;
void main() {
    ivec2 c = ivec2(gl_VertexID % uWidth, gl_VertexID / uWidth);
    vec4 p = texelFetch(uPos, c, 0), v = texelFetch(uVel, c, 0);
    vUV = vec2(0.0);
    if (p.w <= 0.0 || int(v.w + 0.5) != 7) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    vec2 cell = vec2((p.x - uRegion.x) / (uRegion.y - uRegion.x), (p.y - uRegion.z) / (uRegion.w - uRegion.z)) * uMSize - uOrg;
    gl_Position = vec4((cell + 0.5) / uSSize * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFsSnowDeposit = R"(
uniform float uRate;
void main() { o = vec4(uRate, 0.0, 0.0, 0.0); }
)";

static const char* kFsSnowCopy = R"(
uniform sampler2D uS;
uniform vec2 uOrg;
void main() { o = texelFetch(uS, ivec2(gl_FragCoord.xy - uOrg), 0); }
)";

static const char* kFsSnowStep = R"(
uniform sampler2D uM;      // the snow layer (world-anchored)
uniform sampler2D uSolid;  // r = front surfaces (full res, this frame)
uniform sampler2D uTerrain; // world terrain mask (same cells as the layer), from the terrain geometry
uniform vec4 uRegion;      // world rect of the layer
uniform vec2 uMSize;
uniform vec2 uOrg;         // this scratch pixel's layer cell = uOrg + its position
uniform vec4 uWorld;       // world rect of the screen
uniform vec4 uP;           // slide rate, melt per step, max depth (cells), direction (+1 / -1)
uniform vec4 uWaterS;      // the sea: on, surface (world y, y down)
uniform vec4 uHeatW[8];    // heat that melts snow: world x, y, radius, melt per step
uniform int uHeatN;
ivec2 gCell;
float S(vec2 d) {
    ivec2 c = gCell + ivec2(d);
    if (c.x < 0 || c.y < 0 || c.x >= int(uMSize.x) || c.y >= int(uMSize.y)) return 0.0;
    return texelFetch(uM, c, 0).r;
}
vec2 screenUV(vec2 d) {
    vec2 c = vec2(gCell) + d + 0.5;
    vec2 w = vec2(mix(uRegion.x, uRegion.y, c.x / uMSize.x), mix(uRegion.z, uRegion.w, c.y / uMSize.y));
    return vec2((w.x - uWorld.x) / (uWorld.y - uWorld.x), (w.y - uWorld.z) / (uWorld.w - uWorld.z));
}
float T(vec2 d) {   // terrain in that cell (geometry)
    ivec2 c = clamp(gCell + ivec2(d), ivec2(0), ivec2(uMSize) - 1);
    return texelFetch(uTerrain, c, 0).r;
}
float O(vec2 d) {   // solid in that cell: the terrain, or a front surface on screen this frame
    vec2 uv = screenUV(d);
    float s = max(texture(uSolid, uv).r, texture(uSolid, uv - vec2(0.0, 0.3 / uMSize.y * (uRegion.w - uRegion.z) / (uWorld.w - uWorld.z))).r);
    return max(s, T(d));
}
float cap(float s, float o) { return (1.0 - step(0.5, o)) * max(0.0, 1.0 - s); }
// A structure above this cell (a strut, a roof, a deck, the fort's back wall) within ~600 world units:
// flakes can't reach it, so snow doesn't fill open frames or lie inside forts - only their tops.
bool Sheltered(vec2 uv0) {
    float stepUV = max(9.5, abs(uRegion.w - uRegion.z) / uMSize.y) / abs(uWorld.w - uWorld.z);
    for (int k = 1; k <= 64; k++) {
        vec2 uv = uv0 + vec2(0.0, float(k) * stepUV);   // uv y grows up the screen
        if (uv.y > 1.0) break;
        if (texture(uSolid, uv).a > 0.3) return true;
    }
    return false;
}
void main() {
    gCell = ivec2(uOrg + floor(gl_FragCoord.xy));
    float sC = S(vec2(0.0));
    vec2 uv0 = screenUV(vec2(0.0));
    if (T(vec2(0.0)) > 0.5) { o = vec4(0.0); return; }        // never inside terrain
    if (uv0.x < 0.01 || uv0.y < 0.01 || uv0.x > 0.99 || uv0.y > 0.99) { o = vec4(sC, 0.0, 0.0, 1.0); return; }   // at the screen edge: keep
    float dir = uP.w;
    float sB = S(vec2(0.0, -1.0)), sA = S(vec2(0.0, 1.0));
    float oC = O(vec2(0.0)), oB = O(vec2(0.0, -1.0));
    float outDown = min(sC, cap(sB, oB));                        // falling into free space
    float inAbove = min(sA, cap(sC, oC));
    float sT = S(vec2(dir, -1.0)), oT = O(vec2(dir, -1.0));      // sliding, one diagonal direction per frame
    bool blockedC = oB > 0.5 || sB > 0.95;
    float outDiag = blockedC ? min(sC - outDown, cap(sT, oT)) * uP.x : 0.0;
    float sSrc = S(vec2(-dir, 1.0));
    float sSide = S(vec2(-dir, 0.0)), oSide = O(vec2(-dir, 0.0));
    bool blockedSrc = oSide > 0.5 || sSide > 0.95;
    float inDiag = blockedSrc ? min(sSrc, max(0.0, cap(sC, oC) - inAbove)) * uP.x : 0.0;
    float s = sC - outDown - outDiag + inAbove + inDiag;
    if (oC > 0.5) s = 0.0;                                       // snow never sits inside a surface
    vec4 sol0 = texture(uSolid, uv0);
    if (sol0.a > 0.5 && sol0.r < 0.3) s = 0.0;                   // inside a fort (its back wall): it doesn't fill up
    float cellY = mix(uRegion.z, uRegion.w, (float(gCell.y) + 0.5) / uMSize.y);
    if (uWaterS.x > 0.5 && cellY > uWaterS.y) s = 0.0;           // melts in the sea
    float cellX = mix(uRegion.x, uRegion.y, (float(gCell.x) + 0.5) / uMSize.x);
    for (int i = 0; i < uHeatN; i++) {                           // melts around fires and hot spots
        float d = length(vec2(cellX, cellY) - uHeatW[i].xy);
        if (d < uHeatW[i].z) s -= uHeatW[i].w * (1.0 - d / uHeatW[i].z);
    }
    if (s > 0.0 && oC < 0.5 && Sheltered(uv0)) s = 0.0;           // under a structure: open frames stay clear
    float depth = 0.0;                                           // maximum depth: compacts from the top
    for (int k = 1; k <= 24; k++) {
        if (float(k) > uP.z + 0.5) break;
        if (O(vec2(0.0, -float(k))) > 0.5 || S(vec2(0.0, -float(k))) < 0.6) break;
        depth += 1.0;
    }
    if (depth >= uP.z) s *= 0.85;
    s = max(0.0, s - uP.y);
    o = vec4(clamp(s, 0.0, 1.0), 0.0, 0.0, 1.0);
}
)";

// Photo mode depth of field: one pass of a 9-tap Gaussian along uDir (run three times).
static const char* kFsBlur = R"(
uniform sampler2D uSrc;
uniform vec2 uDir;
void main() {
    const float w[5] = float[5](0.227, 0.194, 0.122, 0.054, 0.016);
    vec3 c = texture(uSrc, vUV).rgb * w[0];
    for (int i = 1; i < 5; i++) {
        c += texture(uSrc, vUV + uDir * float(i)).rgb * w[i];
        c += texture(uSrc, vUV - uDir * float(i)).rgb * w[i];
    }
    o = vec4(c, 1.0);
}
)";

// Terrain mask: the terrain triangles (world space, read from the game's display lists) drawn into a
// world-anchored target aligned with the snow layer.
static const char* kVsTerrain = R"(#version 330
uniform sampler2D uVerts;   // RGBA32F, xy = world position
uniform int uWidth;
uniform vec4 uRegion;
out vec2 vUV;
void main() {
    vec2 w = texelFetch(uVerts, ivec2(gl_VertexID % uWidth, gl_VertexID / uWidth), 0).xy;
    vec2 uv = vec2((w.x - uRegion.x) / (uRegion.y - uRegion.x), (w.y - uRegion.z) / (uRegion.w - uRegion.z));
    vUV = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFsTerrain = R"(
void main() { o = vec4(1.0); }
)";