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
//     It is fibre fuzz scattering in every direction, not a reflection off anything flat, so it is
//     added to the diffuse and it takes the same Fresnel-shaped factor everything else here does —
//     on the HALF VECTOR, which is the game's own choice and is what makes a cloth go bright where
//     the eye and the light are far apart. **It is not retro-reflection and neither is the game's.**
//     `(1 - v·h)^5` is nearly NOUGHT when you look straight down the light, so velvet lights up
//     side-on and backlit, round the edge of a fold, and not along the beam. `_contract.clip`
//     describes velvet as "brightest where you look along the light"; that is what the material is,
//     it is not what this lobe or `surface_response` draws, and matching the game was the
//     instruction. Measured on the salon's silk ceiling it is worth 1 to 3 per cent — see below;
//   - CLEARCOAT: its own GGX at a fixed roughness of 0.06, its own Smith, its own DIELECTRIC
//     Fresnel (a coat is clear, so 0.04 whatever the metal under it is doing), and — the part that
//     makes it read as a coating rather than as a shinier material — `response * (1 - clearcoat *
//     fc) + coat`, so the base keeps its own roughness and its own colour and is merely seen
//     THROUGH the lacquer;
//   - emission, `tint * emissive^2 * 64`, the square included. A torch and a furnace are not one
//     step apart on a linear scale, and the byte has to span both. Worth knowing what that number
//     means here: every emitter in the facility lands between 22 (`sconce`) and 43 (`taper`) against
//     a sun of 3.3, so ALL OF THEM CLIP THROUGH THE TONEMAP AND COME OUT WHITE, and their RGB565
//     tint survives only where a blended surface dims them. That was already true of the old
//     `emissive * 6`; what the game's curve changes is that an emitter now reads as emitting in a
//     dark corner — `?lobes=-emit` takes the salon's sconces from 250,250,250 to 80,67,50 — and
//     that the gap between a candle and a furnace is a gap rather than a step. Fixing the white is
//     a glare pass, which is the game's answer too and is not this file's business.
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
//     where it does not, at the same occlusion the diffuse gets. It is not a metal fix on its own:
//     it lifts every specular in a room, gilt and boiserie alike, by about a sixth.
//   - THE ENVIRONMENT IS PREFILTERED BY THE LOBE, WHICH IS WHAT SEPARATES ONE METAL FROM ANOTHER.
//     See ws_environment below. The game gets this from the bin count a face's roughness asks for
//     and from a real measurement of the room; here it is the sun's disc spread over the lobe at
//     constant energy, which is the same statement about the same quantity. Without it, roughness
//     reaches the picture only through the sun's own highlight — so an interior, where there is no
//     sun on anything, drew a mirror and a rough metal identically. It is the one term here that is
//     not in the game's shaders in this form.
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
// of a fixed box of the picture the page drew, decoded from the screenshot afterwards, so they are
// comparable DOWN a column and mean nothing across one. Every arm below is `?lobes=` on the same
// build; the "before" row is the tree without this change at all.
//
//                       gilt panel      ceiling       pilaster        parquet         damask
//                    (metal 225,r64)   (plaster)   (pale, no coat)  (lacquer 10)   (sheen 10)
//   before             90, 81, 42     76,81,89     125,129,132     82, 69, 67     58,51,66
//   this              105, 95, 50     71,75,82     128,131,134     80, 71, 78     55,48,63
//   no lacquer        105, 95, 50     71,75,82     127,130,131     71, 56, 51     53,46,61
//   no sheen          105, 95, 50     71,75,82     127,131,133     80, 71, 78     54,48,63
//   no metal           61, 56, 38     71,75,82     122,126,132     79, 71, 78     55,48,64
//
// Which says, in order:
//
//   - THE METAL PICKS OUT FROM THE DIELECTRIC, which is the whole test. Gilt is up a sixth and
//     warmer while the plaster beside it is down a fifteenth and the pilaster has not moved. An
//     earlier arm of this file lifted gilt and plaster BY THE SAME FIFTH, which was the specular
//     ambient a rasteriser owes every surface and not a metal at all; the prefiltered environment
//     is what separated them.
//   - TURNING METAL OFF COSTS GILT 42 PER CENT and costs the plaster nothing, so the metal is
//     carrying the panel rather than decorating it. It was worth 8 per cent before this change.
//   - THE LACQUER IS THE SKY IN THE FLOOR. Parquet goes 71,56,51 without it to 80,71,78 with it:
//     half again as much blue, on a brown floor, which is a low sky reflected in a polished
//     surface at a grazing angle. Nothing else in this view moves by more than a unit.
//   - THE SHEEN IS A SILHOUETTE TERM AND THIS VIEW IS THE WRONG ONE FOR IT: one unit on the
//     damask. It is `(1 - v·h)^5`, so it lives in the last few degrees before the edge, which is
//     what the game does and what velvet does.
//
// And the same thing outdoors, where the sun is on everything. `mirror_test`, orbit at yaw 0,
// pitch -0.05, distance 5.0, target (0, 0.95, -1.2) — a near-mirror ball and a rough one side by
// side, which is what that clip exists for:
//
//                      chrome      brushed        gold      red post       floor
//                    (r8,m250)   (r110,m220)  (r40,m240)   (r200,m0)   (r18,m30)
//   before          174,187,209  141,153,174  162,142,63   141,59,66  237,237,239
//   this            180,192,213  116,128,150  165,146,67   136,60,64  234,233,236
//
// A metal at roughness 8 and a metal at roughness 110 were 19 per cent apart and are now 35, and
// the one that moved is the ROUGH one — down an eighth, because a lobe that wide no longer reads a
// sharp sky. That is the only reading of "is the metal right" this viewer can be given without
// rays, and it is the reading `_contract.clip` asks for.
//
// The dielectric moves 3 per cent the other way: the red post loses the share of its diffuse that
// the specular turns away. Stone is meant to sit still under this change and it does.
//
// # A phone, and what each lobe costs
//
// Every lobe is behind a branch on the material's own bytes. `coat` is one byte holding both
// nibbles, so a surface with neither lacquer nor sheen — which is every stone in the building —
// pays one integer compare for both. The brush is a second compare. The environment's two `pow`
// calls were already in `sky_colour` and are not doubled: `ws_environment` replaces that function
// rather than wrapping it, and the coat reads its own lobe out of the same one call.
//
// **The per-lobe frame cost could not be measured on the machine this was written on, and the
// control that says so is in the table.** `?lobes=` compiles each lobe out; the renderer's own
// draw was then timed with a readPixels fence on each end, fastest of seven runs of six draws, at
// a pinned 640x480. On the salon camera above:
//
//   all lobes                                  424.5 ms      (software GL, so read RATIOS only)
//   ...without the lacquer                     393.6
//   ...without the sheen                       416.7
//   ...without the brush                       411.3
//   ...without the emission                    386.5
//   ...WITHOUT ANY OF THEM                     442.8   <-- and this is the control
//   all lobes, again                           426.5
//
// And the same on mirror_test, which declares NO lacquer, NO sheen and NO brush at all — so those
// three arms cannot possibly be doing anything, and are the null control:
//
//   all lobes    163.1   156.6   156.3
//   none of them 168.0   149.2   153.8
//
// Compiling every lobe out came back FOUR PER CENT SLOWER than leaving them all in on the salon,
// and on a clip where three of them are provably dead the two arms interleave. So the run-to-run
// spread of this box — a software rasteriser sharing four cores with a dozen other browsers — is
// wider than anything being asked about.
//
// THAT TABLE'S FAULT IS ITS DESIGN AS WELL AS ITS MACHINE, and fixing the design does give a bound.
// Eight arms run one after another cannot tell a lobe from a busy minute, which is precisely what it
// reported. Run only the two extreme arms, ALTERNATED, six rounds each, so a slow patch hits both:
//
//   all lobes     383.9  272.8  265.9  431.6  441.2  399.6     best 265.9 ms
//   none of them  263.7  263.6  374.4  382.1  398.7  445.7     best 263.6 ms
//
// All five lobes together are worth 0.9 PER CENT of the fastest frame this box will produce, on the
// salon, where every one of them is in use. The spread WITHIN one arm is 66 per cent, which is why
// the best of many is the only statistic worth reading and why running the same script again gave
// "none of them" the slower best — it never caught a quiet patch. The lobes together cost under
// about one per cent of a software frame; per-lobe attribution is not resolvable here at any number
// of repeats, and a phone with a real GPU is where that measurement lives.
//
// What can be counted exactly is the work, per pixel, on a face that has the lobe:
//
//   brush      one cross, two dots, one inversesqrt and about ten multiplies, REPLACING the
//              isotropic distribution's five. Nothing at all on a face with no grain named.
//   sheen      two pow(x, 5) and about eight multiplies — one of each in the direct term and in
//              the ambient one.
//   lacquer    two pow(x, 5) and about twenty-two multiplies: a second GGX, a second Smith, its
//              own Fresnel twice, and the mix that dims what is under it.
//   metal      nothing. It is a mix that was already there.
//   emission   one branch and three multiplies, on emissive faces only.
//
// And the two additions that are NOT behind a branch, because every surface has an environment:
//
//   the room fallback        one extra mix — three multiply-adds — on every pixel.
//   the prefilter            ws_environment against the old sky_colour: one max, one divide, one
//                            multiply and one extra mix. The two pow() calls were already there
//                            and one of them now has a computed exponent instead of a literal.
//
// That is four or five instructions a pixel for the term that separates one metal from another,
// which is the cheapest thing in this file per unit of what it buys.
//
// # Turning them off
//
// `?lobes=-sheen,-coat,-brush,-metal,-emit` in the page's URL compiles the named lobes out, which
// is the control arm above. Nothing else reads it and it defaults to all on.

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

// The GLSL. Written as one string so the surface shader is one splice, and what it needs from its
// host is four uniforms and no functions: `u_sun`, `u_sunColour`, `u_skyUp`, `u_skyDown`. Those are
// the sky, and `ws_environment` below is the same sky prefiltered by the lobe reading it — so if
// gl.js's `sky_colour` ever changes shape, this changes with it or a mirror stops matching what it
// is reflecting.
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

// The environment, PREFILTERED BY THE LOBE THAT IS GOING TO READ IT. The first half of the split
// sum, and the half this viewer did not have.
//
// It cannot be got by blurring what sky_colour returns, and that is the whole point. That function
// adds two things together and they prefilter differently: the GRADIENT is smooth over the whole
// sky and survives any lobe, and the SUN'S DISC is about four degrees across and is spread out by
// the lobe until it is gone. Blurring their sum by one number is what made ormolu, gilt and
// gold_leaf — three golds a stop apart, put in the contract precisely so that a metal could be
// judged against itself inside one frame — come out as one yellow. Their reflected lobes are 3.4,
// 4.1 and 7.2 degrees wide, and against a gradient all three of those are "sharp".
//
// So the disc is spread here instead. A lobe of half-width w reads the disc over (w / disc)^2
// times its solid angle, so the peak falls by exactly that and the energy does not change:
// gold_leaf keeps a hard bright sun at 3.0, ormolu a softer one at 2.1, gilt a broad warm glow at
// 0.66. Below the disc's own width nothing happens at all, which is what keeps a mirror showing
// the sun exactly as the sky draws it and no brighter.
//
// The exponents are sky_colour's own: pow(cos, 400) is a half-width of 0.059 radians, and the wide
// pow(cos, 8) glow is already broader than any lobe in this building. THIS FUNCTION MIRRORS
// gl.js's sky_colour AND THE TWO HAVE TO MOVE TOGETHER — at alpha 0 it reproduces it exactly, and
// that is the test if the sky is ever changed.
const float WS_SUN_DISC = 0.059;

vec3 ws_environment(vec3 dir, float alpha, vec3 flatly) {
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    // The gradient, towards flat as the lobe opens out. Four times alpha, because a gradient this
    // smooth is still legible at a lobe width that has already swallowed the disc whole.
    vec3 base = mix(mix(u_skyDown, u_skyUp, up), flatly, clamp(alpha * 4.0, 0.0, 1.0));
    // The reflected lobe is twice the surface's own, and never narrower than the disc it reads.
    float w = max(2.0 * alpha, WS_SUN_DISC);
    float shrink = WS_SUN_DISC / w;
    float towards = max(dot(dir, u_sun), 0.0);
    base += u_sunColour * pow(towards, 1.386 / (w * w)) * (3.0 * shrink * shrink);
    base += u_sunColour * pow(towards, 8.0) * 0.05;
    return base;
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
    // reflection off anything flat. On the HALF VECTOR, verbatim from surface_response — so it is
    // brightest where the eye and the light are far apart and nearly nought straight down the
    // beam. Not retro-reflection; see the note at the top of this file for why that is the game's
    // answer rather than an approximation of a better one.
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
    // The GGX width, the same one ws_direct takes and the one ws_environment prefilters by.
    float alpha = max(m.roughness * m.roughness, 1e-3);

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
    // Prefiltered by this surface's own lobe rather than blurred afterwards: see ws_environment
    // for why the difference is the whole of whether three golds a stop apart read as three
    // metals. Flat is the ROOM and not the raw ambient — a fully rough surface deep in a hall
    // must not be lit by a sky it cannot see.
    vec3 env = ws_environment(r, alpha, room);

    // The split sum. No PI: a prefiltered environment is already a radiance, and a mirror has to
    // be exactly as bright as what it is reflecting.
    colour += ws_fresnel(f0, ndv) * mix(room, env, shade);

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
        // The coat's own lobe is 0.06 rough, so it reads the environment nearly sharp — its own
        // prefilter, not the base material's, which is the point of a lacquer over rough wood.
        vec3 coatEnv = ws_environment(r, WS_COAT_ROUGH * WS_COAT_ROUGH, room);
        colour = colour * (1.0 - take) + mix(room, coatEnv, shade) * take;
    }
#endif

    return colour;
}
// ===== end of the material model ================================================================
`;
}

export const BRDF_GLSL = brdfGlsl();
