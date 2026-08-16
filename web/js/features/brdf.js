// The material model: every lobe a VisualRecord declares, rasterised.
//
// A `VisualRecord` is sixteen bytes and the viewer was reading about half of them. Colour,
// roughness and metalness reached the shader; `lacquer`, `sheen`, `brush` and the emissive tint
// were fetched and thrown away, and the specular lobe that was there sat at the wrong strength
// relative to the diffuse. What that draws is grey plastic with a colour on it: the three golds
// of `_contract.clip` — `ormolu` at rough 48, `gilt` at 64, `gold_leaf` at 40, deliberately a stop
// apart so they can be told apart inside one frame — came out as one yellow, and velvet, silk,
// parquet and porcelain came out as four matte paints.
//
// So this is the game's own shading, ported. `shaders/pt_material.glsl` is the reference and
// `surface_response` in it is the function being matched: the anisotropic base lobe, the sheen
// added to the DIFFUSE (not the specular), and the lacquer laid over the top with everything
// underneath dimmed by what the coat sent back. `shaders/face_terms.glsl` and the composite in
// `shaders/resolve.comp` are the reference for the half a rasteriser has to do differently — the
// environment through the split-sum, the diffuse's share of what the specular turned away, and the
// ceiling on a lobe narrower than the sun's own disc.
//
// # The one thing that had to be converted, and it is why the metals were dark
//
// The game writes a BRDF: `surface_response` returns `f * cos`, its diffuse is `albedo / PI`, and
// the composite multiplies by an irradiance. This viewer has never had the `/ PI`: its diffuse is
// `albedo * sunColour * n·l` and its exposure, its sky colours and every screenshot ever taken of
// it are tuned around that. Both are defensible; they differ by a factor of PI on the diffuse
// alone, which means the specular in this viewer was PI times too weak RELATIVE to it, and a
// factor of three on the specular of a metal is the whole difference between metal and paint.
//
// The conversion is therefore stated once, here, and nothing else has to know:
//
//     this viewer's surface term  ==  PI * (the game's surface_response) * irradiance
//
// The diffuse comes out `albedo * (1 - metal) * n·l * sunColour`, which is exactly what the shader
// drew before — so stone does not move — and the specular arrives at the strength the game gives
// it. The old code also carried `n·l` twice on the specular; that is gone with the same change.
//
// The ENVIRONMENT term is not multiplied by PI, and that is not an inconsistency. A prefiltered
// environment is already an outgoing radiance: `F(f0, n·v) * L_env` is what a mirror shows, and a
// mirror has to be exactly as bright as the sky it reflects or the reflection is brighter than the
// thing reflected. `face_lobe_environment` in `shaders/face_terms.glsl` is the same expression.
//
// # What is matched exactly, and what is approximated
//
// Exact, term for term against `surface_response`:
//
//   - the base GGX distribution, `alpha = max(roughness^2, 1e-3)`;
//   - the ANISOTROPIC form of it, at `kBrushStretch = 2.45` — sqrt(6), so the two axes end up 6:1
//     and the product of the widths does not change, which is why a brushed surface is neither
//     brighter nor darker overall than the isotropic one it replaces. Narrow ALONG the grain and
//     wide across it: a groove is smooth down its length and curved across it. The grain is a
//     WORLD axis projected into the face, and a face the grain runs straight out of has no grain,
//     which is the honest answer and not a degenerate case — the cut end of a brushed baluster
//     does not have a stretched highlight on it;
//   - Smith, `k = alpha / 2`, isotropic even under the brush (the game says why: the anisotropic
//     Smith is two more square roots for a difference at the very edge of a grazing highlight);
//   - Schlick's Fresnel, `f0 = mix(dielectric, albedo, metal)`;
//   - SHEEN, into the diffuse lobe and not the specular one, as `albedo * sheen * (1 - v·h)^5 / PI`.
//     It is fibre fuzz scattering in every direction, not a reflection off anything flat, and it
//     is `v·h` — the half vector — rather than `n·v`, which is what makes velvet brightest along
//     the light rather than merely at the silhouette;
//   - CLEARCOAT: its own GGX at a fixed roughness of 0.06, its own Smith, its own DIELECTRIC
//     Fresnel (a coat is clear, so 0.04 whatever the metal under it is doing), and — the part that
//     makes it read as a coating rather than as a shinier material — `response * (1 - clearcoat *
//     fc) + coat`, so the base keeps its own roughness and its own colour and is merely seen
//     THROUGH the lacquer;
//   - emission, `tint * emissive^2 * 64`, the square included. A torch and a furnace are not one
//     step apart on a linear scale, and the byte has to span both.
//
// Approximated, deliberately, with the reason:
//
//   - THE SUN IS CLAMPED. `face_lobe_sun` caps `D * Vis * n·l` at one, because a GGX lobe narrower
//     than the sun's disc integrates to more than one and draws a single blinding pixel that the
//     tonemap then exposes the whole building down to avoid. `surface_response` has no such cap
//     because a path tracer samples the disc. This has no disc, so it takes the cap.
//   - AMBIENT SHEEN uses `(1 - n·v)^5` where the direct term uses `(1 - v·h)^5`. There is no
//     `v·h` for a hemisphere; in the game a velvet in a room with no sun still gets its sheen
//     because every bounce ray evaluates `surface_response`, and this viewer has no bounce rays.
//     `n·v` is that integral's grazing shape and nothing more.
//   - WHAT A SURFACE REFLECTS INDOORS IS THE ROOM, AND THE ROOM HERE IS THE AMBIENT. In the game a
//     metal's environment comes out of the face's own lobe bins, which hold the room it is standing
//     in. This viewer has no such measurement and was reflecting the SKY attenuated by how much sky
//     the lattice could find, which four metres inside a state room is nearly nothing. So a surface
//     reflects the sky where the sky reaches it and the ambient the light grid already carries
//     where it does not, at the same occlusion the diffuse gets. See the measured table below for
//     what that is worth and for what it is NOT: it lifts every specular in a room, gilt and
//     plaster alike, rather than picking the metals out.
//   - THE COAT REFLECTS THE SAME THING. The game's face path gives the environment to the BASE lobe
//     only; the coat's share of it arrives through bounces. Without an environment term the coat
//     would dim what is under it and give nothing back, which is a lacquer that makes a floor
//     darker.
//   - F0 FOR A DIELECTRIC COMES FROM THE RECORD'S `ior`, where the game uses a flat 0.04. The two
//     agree at ior 1.5, which is glass; this is the viewer's existing behaviour and it is kept
//     because `crystal` at 1.62 and `water` at 1.33 are declared and mean something.
//   - THE DIFFUSE'S LOSS TO THE SPECULAR is the HEMISPHERICAL average of Schlick,
//     `F0 + (1 - F0) / 21`, and not the view-angle one — `resolve.comp` has a long note on why,
//     and the short version is that taking it at the view angle sent the gilt paterae grey.
//
// # The arms, measured rather than argued
//
// `facility-salon`, orbit at yaw 1.5708, pitch -0.02, distance 6.2, target (7.2, 0.2, -4.8), at
// 900x700 — the room's long axis, low sun through the south windows. Each figure is the mean sRGB
// red of a fixed box, so they are comparable down a column and mean nothing across one.
//
//                                        gilt panel   plaster   parquet, sunlit   chandelier
//   before this change                        92.4      93.8         100.8           80.2
//   this                                     108.6     113.6         107.1           93.8
//   ...environment = sky * sky visibility      93.1      94.9         102.5           84.2
//   ...room's share taken unoccluded          138.2     149.3         114.0          117.9
//   ...no environment for the coat            108.1     113.6         101.5           93.7
//
// Three things that says:
//
//   - the room-as-environment fallback is the change that moves the picture, and it moves GILT AND
//     PLASTER BY THE SAME SIXTH. It is not a metal fix; it is the specular ambient a rasteriser
//     owes every surface, and the metals were only the loudest thing missing it;
//   - taking the room's share unoccluded lifts everything by a further quarter to a third, which is
//     a room with its shadows washed out. The occlusion is not optional;
//   - the coat's environment is worth about six per cent on sunlit parquet and nothing anywhere
//     else in this view. It earns its place for what it does at a grazing angle rather than for
//     what it does to an average.
//
// The last three rows are one line of this file changed at a time; `?lobes=` below takes the arms
// for the lobes themselves without touching it.
//
// # A phone
//
// Every lobe is behind a branch on the material's own bytes. `coat` is one byte holding both
// nibbles, so a surface with neither lacquer nor sheen — which is every stone in the building —
// pays one integer compare for both. The brush is a second compare. Nothing else was added to the
// path a plain limestone wall takes, and the one `sky_colour` call the environment needs was
// already there and is now shared with the coat rather than doubled.
//
// # Turning them off
//
// `?lobes=-sheen,-coat,-brush,-metal` in the page's URL compiles the named lobes out, which is the
// control arm for measuring what each costs. Nothing else reads it and it defaults to all on.

const LOBES = ['sheen', 'coat', 'brush', 'metal', 'emit'];

// What the URL asked to be compiled out. `-name` disables; anything else is ignored rather than
// being an error, because a mistyped query on a phone should not be a black page.
function disabled() {
    const off = new Set();
    try {
        const asked = new URLSearchParams(globalThis.location?.search || '').get('lobes');
        if (asked) {
            for (const part of asked.split(',')) {
                const name = part.trim().replace(/^-/, '');
                if (part.trim().startsWith('-') && LOBES.includes(name)) off.add(name);
            }
        }
    } catch (e) { /* no location: a test harness. Everything on. */ }
    return off;
}

// The GLSL. Written as one string so the surface shader is one splice, and it needs exactly one
// thing from its host: `vec3 sky_colour(vec3)`, which gl.js defines above the splice point.
//
// NO BACK-QUOTES BELOW THIS LINE. The whole shader is a template string, so one back-quote in a
// GLSL comment ends the string and the page dies on a JavaScript syntax error at the identifier
// that happened to follow it — which reads like anything except a stray punctuation mark in a
// comment. It has happened twice here. gl.js carries the same warning over its own shaders.
export function brdfGlsl() {
    const off = disabled();
    const on = (name) => (off.has(name) ? '0' : '1');
    return `
// ===== the material model — see web/js/features/brdf.js for what is matched and what is not =====
#define WS_LOBE_SHEEN ${on('sheen')}
#define WS_LOBE_COAT  ${on('coat')}
#define WS_LOBE_BRUSH ${on('brush')}
#define WS_LOBE_METAL ${on('metal')}
#define WS_LOBE_EMIT  ${on('emit')}

const float WS_PI = 3.14159265;

// sqrt(6): the two axes of a brushed highlight end up 6:1, and the product of the widths — which
// is what sets how bright the peak is — does not change. shaders/pt_material.glsl, kBrushStretch.
const float WS_BRUSH_STRETCH = 2.45;

// A lacquer is smooth by definition. If it were rough it would be the material, not a coat on it.
// shaders/pt_material.glsl, kCoatRoughness.
const float WS_COAT_ROUGH = 0.06;

struct WsMaterial {
    vec3 albedo;        // linear
    float opacity;
    float roughness;
    float metal;
    float f0d;          // the dielectric reflectance the record's ior asks for
    float translucency;
    float clearcoat;
    float sheen;
    int brush;          // 0 none, 1 x, 2 y, 3 z — a WORLD axis, so it survives a corner
    vec3 emission;
    vec3 absorb;
};

// The sixteen bytes, unpacked. Byte for byte what shaders/pt_material.glsl's material_of reads out
// of the same record, and the only place in this viewer that knows the layout.
WsMaterial ws_material(vec4 base, vec4 surface, vec4 depth, vec4 extra) {
    WsMaterial m;
    m.albedo = base.rgb * base.rgb;            // close enough to sRGB, and one multiply
    m.opacity = base.a;
    m.roughness = surface.r;
    m.metal = surface.g;
    m.translucency = depth.a;
    m.absorb = depth.rgb;

    // The record stores the index as an offset from vacuum, so ((n-1)/(n+1))^2 is u/(2+u) squared.
    // Zero means "not declared" and takes the 0.04 every dielectric gets.
    float iorByte = surface.b * 255.0;
    float u = iorByte / 128.0;
    m.f0d = (iorByte > 0.5) ? pow(u / (2.0 + u), 2.0) : 0.04;

    int flags = int(extra.b * 255.0 + 0.5);
    int coat = int(extra.a * 255.0 + 0.5);
#if WS_LOBE_BRUSH
    m.brush = (flags >> 3) & 3;
#else
    m.brush = 0;
#endif
    // Two lobes in one byte, four bits each: low nibble lacquer, high nibble sheen.
    m.clearcoat = float(coat & 15) / 15.0;
    m.sheen = float((coat >> 4) & 15) / 15.0;
#if !WS_LOBE_COAT
    m.clearcoat = 0.0;
#endif
#if !WS_LOBE_SHEEN
    m.sheen = 0.0;
#endif
#if !WS_LOBE_METAL
    m.metal = 0.0;
#endif

    m.emission = vec3(0.0);
#if WS_LOBE_EMIT
    float emissive = surface.a;
    if (emissive > 0.0) {
        int tint = int(extra.r * 255.0 + 0.5) | (int(extra.g * 255.0 + 0.5) << 8);   // RGB565
        vec3 colour = vec3(float((tint >> 11) & 31) / 31.0,
                           float((tint >> 5) & 63) / 63.0,
                           float(tint & 31) / 31.0);
        // Squared, so a byte spans a candle and a furnace. shaders/pt_material.glsl.
        m.emission = colour * (emissive * emissive * 64.0);
    }
#endif
    return m;
}

float ws_ggx_d(float ndh, float alpha) {
    float a2 = alpha * alpha;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(WS_PI * d * d, 1e-6);
}

// Smith, on the geometric mean of the two widths — which is alpha itself, since the brush
// multiplies one axis by exactly what it divides the other by.
float ws_smith(float ndv, float ndl, float alpha) {
    float k = alpha * 0.5;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

vec3 ws_fresnel(vec3 f0, float cosTheta) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

// The direction the grain runs in, here, on this face. Zero when the grain runs along the face's
// own normal, which is the honest answer: the end of a brushed bar has no grain across it.
vec3 ws_brush_tangent(int axis, vec3 n) {
    if (axis == 0) return vec3(0.0);
    vec3 a = (axis == 1) ? vec3(1.0, 0.0, 0.0)
           : (axis == 2) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 t = a - n * dot(a, n);
    float len2 = dot(t, t);
    if (len2 < 1e-3) return vec3(0.0);
    return t * inversesqrt(len2);
}

// Everything the surface does with light arriving from ONE direction, as a BRDF times the cosine.
// This is shaders/pt_material.glsl's surface_response with the cosine folded through every term,
// so the caller multiplies by the irradiance along l and by PI (see the note at the top) and by
// nothing else.
vec3 ws_direct(WsMaterial m, vec3 f0, vec3 turnedAway, vec3 n, vec3 v, vec3 l) {
    float ndl = dot(n, l);
    if (ndl <= 0.0) return vec3(0.0);
    float ndv = max(dot(n, v), 1e-4);
    vec3 h = normalize(l + v);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 1e-4);

    float alpha = max(m.roughness * m.roughness, 1e-3);

    float d;
#if WS_LOBE_BRUSH
    vec3 t = ws_brush_tangent(m.brush, n);
    if (dot(t, t) > 0.0) {
        // Narrow ALONG the grain, wide ACROSS it: the micro-normals of a groove fan out across it
        // and the lobe fans with them. The other way round puts the streak parallel to the brush
        // marks, which is what a spun disc shows and not what a drawn one does.
        vec3 b = cross(n, t);
        float at = alpha / WS_BRUSH_STRETCH;
        float ab = alpha * WS_BRUSH_STRETCH;
        float x = dot(h, t) / at;
        float y = dot(h, b) / ab;
        float k = x * x + y * y + ndh * ndh;
        d = 1.0 / max(WS_PI * at * ab * k * k, 1e-6);
    } else {
        d = ws_ggx_d(ndh, alpha);
    }
#else
    d = ws_ggx_d(ndh, alpha);
#endif

    float g = ws_smith(ndv, ndl, alpha);
    // The cosine is folded in here: D * G / (4 n·v) is the specular BRDF times n·l. Capped at one
    // because a lobe narrower than the sun's own disc integrates to more than it (face_lobe_sun).
    vec3 specular = ws_fresnel(f0, vdh) * min(d * g / (4.0 * ndv), 1.0);

    vec3 diffuse = m.albedo * (1.0 - m.metal) * (vec3(1.0) - turnedAway) / WS_PI;
#if WS_LOBE_SHEEN
    // Fibre fuzz, on the DIFFUSE lobe: strands standing off the weave in every direction, not a
    // reflection off anything flat. Grazing on the half vector, which is what makes velvet
    // brightest where you look along the light.
    if (m.sheen > 0.0) diffuse += m.albedo * (m.sheen * pow(1.0 - vdh, 5.0)) / WS_PI;
#endif

    vec3 response = diffuse * ndl + specular;

#if WS_LOBE_COAT
    // The lacquer, over the top of whatever that came to. Its own narrow lobe, its own dielectric
    // Fresnel, and the material beneath dimmed by what the coat sent back — because everything
    // below is seen through it, and that is what makes it read as a coating.
    if (m.clearcoat > 0.0) {
        float ac = WS_COAT_ROUGH * WS_COAT_ROUGH;
        float dc = ws_ggx_d(ndh, ac);
        float gc = ws_smith(ndv, ndl, ac);
        float fc = 0.04 + 0.96 * pow(1.0 - vdh, 5.0);
        float coat = m.clearcoat * fc * min(dc * gc / (4.0 * ndv), 1.0);
        response = response * (1.0 - m.clearcoat * fc) + vec3(coat);
    }
#endif
    return response;
}

// The whole surface, both halves of the light this viewer has: one sun, and a hemisphere.
//
//   sunColour    the sun as an irradiance, which is what this viewer's uniform has always been
//   sunVisible   what the baker's ray found of the disc from here
//   ambient      the sky as an irradiance at this normal
//   skyVisible   what the baker's lattice found of the sky from here
//   ao           the quad's own corner occlusion, 0..1
vec3 ws_shade(WsMaterial m, vec3 n, vec3 v,
              vec3 sun, vec3 sunColour, float sunVisible,
              vec3 ambient, float skyVisible, float ao) {
    vec3 f0 = mix(vec3(m.f0d), m.albedo, m.metal);
    float ndv = max(dot(n, v), 1e-4);

    // What the specular turns away never reaches the diffuse under it. The HEMISPHERICAL average
    // of Schlick, F0 + (1 - F0) / 21, because what gets in is a property of the material and not
    // of where the eye happens to be — resolve.comp's turned_away, and its note on why the
    // view-angle version sent the gilt paterae grey.
    vec3 turnedAway = mix(f0, vec3(1.0), 1.0 / 21.0);

    // Corner occlusion is a voxel's own shape and the light grid is the room it stands in; both
    // are needed, and neither substitutes for the other.
    float occluded = mix(0.35, 1.0, ao) * mix(0.25, 1.0, skyVisible);
    float shade = mix(0.25, 1.0, skyVisible) * mix(0.4, 1.0, ao);

    // The sun, through every lobe. PI because this viewer's diffuse has never carried the 1/PI a
    // BRDF does; see the note at the top of brdf.js, which is where that factor is stated once.
    vec3 colour = ws_direct(m, f0, turnedAway, n, v, sun) * (WS_PI * sunVisible) * sunColour;

    // The sky, diffusely.
    colour += m.albedo * (1.0 - m.metal) * (vec3(1.0) - turnedAway) * ambient * occluded;

    // What this surface has around it to reflect.
    //
    // The environment used to be the sky along the reflection and nothing else, attenuated by how
    // much sky the baker's lattice found. Outdoors that is right. Indoors it is nearly nothing, so
    // every specular in a state room went out and what was left was diffuse.
    //
    // The game does not have this problem because a face's lobe bins hold THE ROOM: what a surface
    // reflects indoors is measured, and it is the room. This viewer has one stand-in for the room
    // and it is the ambient the light grid already carries. So a surface reflects the sky where
    // the sky reaches it and the room's own light where it does not.
    //
    // The room's share is the ambient AS THE DIFFUSE SEES IT — the same occluded — and not the
    // raw ambient. Taking it raw lifts a whole room by a quarter and washes its shadows out; the
    // table at the top of this file is the measurement.
    vec3 room = ambient * occluded;
    vec3 r = reflect(-v, n);
    vec3 env = sky_colour(r);
    vec3 around = mix(room, env, shade);

    // The split-sum, with that blurred towards flat by the surface's own roughness. Flat is the
    // ROOM and not the raw ambient, for the same reason and by the same number — a fully rough
    // surface deep in a hall must not be lit by a sky it cannot see. No PI: a prefiltered
    // environment is already a radiance, and a mirror has to be exactly as bright as what it is
    // reflecting.
    vec3 blurred = mix(around, room, m.roughness * m.roughness);
    colour += ws_fresnel(f0, ndv) * blurred;

#if WS_LOBE_SHEEN
    // The dome's share of the fuzz. (1 - n·v)^5 rather than the half vector, because a hemisphere
    // has no half vector; this is the shape of that integral and nothing more.
    if (m.sheen > 0.0) {
        colour += m.albedo * (m.sheen * pow(1.0 - ndv, 5.0)) * ambient * mix(0.25, 1.0, skyVisible);
    }
#endif

#if WS_LOBE_COAT
    // The lacquer's own view of its surroundings, and the same dimming of what is under it that
    // the direct term applies. This is the sky in a polished parquet floor.
    //
    // It reflects the SAME thing the base lobe does — the sky where the sky reaches, the room's
    // own ambient where it does not — and at the same occlusion, because a coat and the surface
    // under it are in one place and cannot be standing in two different rooms. Nearly unblurred,
    // because a lacquer is smooth by definition.
    //
    // The game's face path gives the environment to the base lobe only and lets the coat's share
    // arrive through bounces. This has no bounces, and leaving the term out is not neutral: the
    // dimming above would then be a straight loss and a lacquered floor would come out DARKER
    // than a bare one, which is the opposite of what a lacquer does.
    if (m.clearcoat > 0.0) {
        float fc = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
        float take = m.clearcoat * fc;
        vec3 coatEnv = mix(around, room, WS_COAT_ROUGH);
        colour = colour * (1.0 - take) + coatEnv * take;
    }
#endif

    return colour;
}
// ===== end of the material model ================================================================
`;
}

export const BRDF_GLSL = brdfGlsl();
