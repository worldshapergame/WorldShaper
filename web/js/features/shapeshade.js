// Shading the ◉ view with the clip's own materials.
//
// The ◉ view draws every shape the author wrote, ray-marched, at no resolution at all. It drew
// them all in one flat grey, and the question that produced this file was whether the raw view
// shows the colours the clip will have in game. So the answer has to be that switching between ◉
// and the voxel view changes the RESOLUTION AND NOTHING ELSE: same materials, same sun, same sky,
// same light grid, same tone map, same exposure. A grey box must be the same grey in both.
//
// # What this file is
//
// One GLSL chunk, spliced into SHAPE_FRAGMENT in web/js/gl.js, holding three things:
//
//   the material fetch     the same four RGBA8 rows the surface shader reads, unquantised, and
//                          every field of the VisualRecord used rather than just the colour
//   the lighting           the surface shader's own, transcribed term for term
//   material_at()          which material is at a hit point — the paint stack, or a stub
//
// # Three seams, and each is one edit
//
// **`material_at`** comes from web/js/features/paint.js, which a parallel agent is writing: it
// evaluates the clip's paint rules at a world point and says which material the voxel sampler
// would have given that place. Until it lands there is a STUB here (see MATERIAL_AT_STUB) that
// picks a material per shape so the shading path is visibly working before the answer is right.
// When paint.js appears exporting MATERIAL_AT_GLSL, it is used instead, with no edit here.
//
// **The BRDF** is the surface shader's single Cook-Torrance lobe, transcribed below. A parallel
// agent is rewriting that into a fuller one in web/js/features/brdf.js — clearcoat, sheen,
// anisotropy, metal. If brdf.js appears exporting SHAPE_LIGHTING_GLSL it is used instead, and the
// only thing that has to be true of it is that it defines
//
//     vec3 ws_shade(WsMaterial m, vec3 N, vec3 V, vec3 L, float sunVisible, float skyVisible,
//                   float ao)
//
// against the WsMaterial struct declared here. The choice is made in exactly one place —
// `pickLighting()` — so swapping it is one line.
//
// **The light grid** is read the way the surface shader reads it, with one difference that is the
// whole reason this file has a comment about it. See THE BIAS below.
//
// # THE BIAS
//
// The baker casts sun and sky visibility onto a lattice 0.4 m apart and buries nothing: a lattice
// point inside stone has no light of its own and is filled in from its brightest neighbour at half
// strength, twice. So a trilinear fetch AT a surface blends the air in front of it with the stone
// behind it, and comes out dark. The surface shader answers that by sampling one whole light cell
// along the normal (`u_lightBias` = clip.lightCell), and the slice cap answers it by sampling half
// a voxel out of the cut. Both of those were black first.
//
// This view has the same trap and one more, because ITS HIT POINTS ARE NOT THE VOXELS'. The march
// lands on the true analytic surface; the light grid was cast against the voxelised copy, whose
// surface is up to a voxel away and may be on either side of it. A point on the analytic surface
// can therefore be INSIDE the voxel matter, which is the darkest place in the grid.
//
// So the bias here is `u_lightBias + half a voxel` — one light cell, as the surface shader uses,
// plus the largest disagreement there can be between the two surfaces. At 16 voxels to the metre
// that is 0.4 + 0.031 m, which moves the fetch by an extra eighth of a cell: far too little to
// change the smooth term it is reading, and enough that the analytic surface can never be the one
// buried point that reads as night. gl.js computes it, so the two views' biases sit next to each
// other in one function.
//
// # The rim light is KEPT
//
// It was there because a construction view is read by its silhouettes, and the argument for
// dropping it once the shapes are properly coloured is real: it is not light, the voxel view does
// not have it, and anything the two views do not share is a thing that makes them disagree. It is
// kept anyway, at a third of its old strength and only on the shapes view, for one reason that
// survives colouring: this view draws INTERPENETRATING shapes with no ambient occlusion of any
// kind — there is no corner term, because there are no voxel corners — so two walls meeting at a
// right angle in the same stone have nothing at all between them and read as one lump. The rim is
// what keeps a column in front of a wall of the same marble a column. At 0.5 it was a fog that
// lifted the whole silhouette; at 0.18 it is an edge.

// ---------------------------------------------------------------------------------------------
// The material record, whole.
//
// Four RGBA8 texels a material, exactly as web/js/gl.js uploads them and exactly as the surface
// shader reads them. Nothing is quantised on the way out of the baker — documentation/24 §2 says
// so and says why — so nothing is dropped here either.
//
//   row 0   colour.rgb                      opacity
//   row 1   roughness  metallic  ior        emission
//   row 2   absorb.rgb                      translucency
//   row 3   tint.lo    tint.hi   flags      coat (clearcoat | sheen << 4)
const MATERIAL_GLSL = `
uniform sampler2D u_materials;
uniform int u_materialCount;

struct WsMaterial {
    vec3 albedo;        // linear
    float opacity;
    float rough;
    float metal;
    float iorByte;      // the record's own byte; ior = 1 + iorByte / 128
    float emissive;
    vec3 emissiveTint;
    vec3 absorb;
    float translucency;
    int flags;
    float clearcoat;
    float sheen;
};

vec4 ws_material_row(int material, int which) {
    int at = material * 4 + which;
    return texelFetch(u_materials, ivec2(at & 255, at >> 8), 0);
}

WsMaterial ws_material(int material) {
    vec4 base = ws_material_row(material, 0);
    vec4 surface = ws_material_row(material, 1);
    vec4 depth = ws_material_row(material, 2);
    vec4 extra = ws_material_row(material, 3);

    WsMaterial m;
    m.albedo = base.rgb * base.rgb;          // close enough to sRGB, and one multiply
    m.opacity = base.a;
    m.rough = clamp(surface.r, 0.045, 1.0);
    m.metal = surface.g;
    m.iorByte = surface.b * 255.0;
    m.emissive = surface.a;
    m.absorb = depth.rgb;
    m.translucency = depth.a;
    m.flags = int(extra.b * 255.0 + 0.5);

    int coat = int(extra.a * 255.0 + 0.5);
    m.clearcoat = float(coat & 15) / 15.0;
    m.sheen = float((coat >> 4) & 15) / 15.0;

    // RGB565, low byte first, which is how a u16 lands in two bytes here.
    int tint = int(extra.r * 255.0 + 0.5) | (int(extra.g * 255.0 + 0.5) << 8);
    m.emissiveTint = vec3(float((tint >> 11) & 31) / 31.0,
                          float((tint >> 5) & 63) / 63.0,
                          float(tint & 31) / 31.0);
    return m;
}
`;

// ---------------------------------------------------------------------------------------------
// The lighting.
//
// This is FRAGMENT_SOURCE's main() in web/js/gl.js, term for term and constant for constant, with
// the material pulled into a struct and the two things the surface shader takes from its vertex —
// corner occlusion, and the face's own normal — passed in. It is a transcription rather than a
// call because gl.js's surface shader is another agent's file this week; when brdf.js lands it
// becomes the one implementation both views use and this constant goes away.
//
// If ANY of these numbers is changed, the two views stop matching, which is the one thing this
// file exists to prevent.
const LIGHTING_GLSL = `
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;

float ws_ggx(float ndh, float rough) {
    float a = rough * rough;
    float d = ndh * ndh * (a * a - 1.0) + 1.0;
    return (a * a) / (3.14159265 * d * d + 1e-7);
}

float ws_smith(float ndv, float ndl, float rough) {
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

vec3 ws_fresnel(vec3 f0, float vdh) {
    float f = pow(1.0 - vdh, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

vec3 ws_sky(vec3 direction) {
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 base = mix(u_skyDown, u_skyUp, up);
    float towards = max(dot(direction, u_sun), 0.0);
    base += u_sunColour * pow(towards, 400.0) * 3.0;
    base += u_sunColour * pow(towards, 8.0) * 0.05;
    return base;
}

vec3 ws_shade(WsMaterial m, vec3 N, vec3 V, vec3 L, float sunVisible, float skyVisible, float ao) {
    vec3 H = normalize(L + V);
    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 1e-4);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);

    float occluded = mix(0.35, 1.0, ao) * mix(0.25, 1.0, skyVisible);

    float dielectric = (m.iorByte > 0.5)
        ? pow((m.iorByte / 128.0) / (2.0 + m.iorByte / 128.0), 2.0)
        : 0.04;
    vec3 f0 = mix(vec3(dielectric), m.albedo, m.metal);

    vec3 ambient = mix(u_skyDown, u_skyUp, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) * 0.5;
    vec3 direct = u_sunColour * ndl * sunVisible;

    vec3 diffuse = m.albedo * (1.0 - m.metal) * (direct + ambient * occluded);

    if (m.translucency > 0.0) {
        float through = max(dot(-N, L), 0.0) * 0.6 + 0.25;
        diffuse += m.albedo * m.translucency * through * u_sunColour * sunVisible;
    }

    // Brushed metal: the grain runs along one world axis, projected into the surface, and the
    // highlight is stretched across it. Bits 3 and 4 of the flags are the axis.
    float distribution = ws_ggx(ndh, m.rough);
    int brush = (m.flags >> 3) & 3;
    if (brush != 0) {
        vec3 axis = (brush == 1) ? vec3(1.0, 0.0, 0.0)
                  : (brush == 2) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
        vec3 grain = axis - N * dot(N, axis);
        float reach = length(grain);
        if (reach > 0.05) {
            grain /= reach;
            vec3 across = cross(N, grain);
            float alpha = m.rough * m.rough;
            float ax = max(alpha * 0.35, 0.002);
            float ay = max(alpha * 1.60, 0.002);
            float th = dot(grain, H) / ax;
            float bh = dot(across, H) / ay;
            float d = th * th + bh * bh + ndh * ndh;
            distribution = 1.0 / (3.14159265 * ax * ay * d * d + 1e-7);
        }
    }

    vec3 specular = ws_fresnel(f0, vdh) * distribution * ws_smith(ndv, ndl, m.rough) /
                    (4.0 * ndv * max(ndl, 1e-4) + 1e-4) * direct * ndl;

    vec3 R = reflect(-V, N);
    vec3 reflected = mix(ws_sky(R), ambient, m.rough * m.rough);
    vec3 ambientSpecular = reflected * ws_fresnel(f0, ndv) * mix(0.25, 1.0, skyVisible) *
                           mix(0.4, 1.0, ao);

    vec3 colour = diffuse + specular + ambientSpecular;

    if (m.clearcoat > 0.0) {
        float lacquer = ws_ggx(ndh, 0.1) * ws_smith(ndv, ndl, 0.1) /
                        (4.0 * ndv * max(ndl, 1e-4) + 1e-4);
        colour += m.clearcoat * 0.25 * (lacquer * direct * ndl + reflected * 0.15 * skyVisible);
    }
    if (m.sheen > 0.0) {
        float rim = pow(1.0 - ndv, 3.0);
        colour += m.sheen * rim * m.albedo * (ambient * skyVisible + direct * 0.3);
    }
    if (m.emissive > 0.0) {
        colour += m.emissiveTint * m.emissive * 6.0;
    }
    return colour;
}
`;

// ---------------------------------------------------------------------------------------------
// The light grid, and the bias. See THE BIAS at the top of this file.
//
// The mapping is the surface shader's, uniform for uniform, so that two points in the same place
// in the two views fetch the same texel. Only `u_shapeLightBias` differs, and gl.js computes it
// next to the surface shader's own so the difference is one line and is visible.
const LIGHT_GLSL = `
uniform sampler3D u_light;
uniform vec3 u_lightOrigin;
uniform vec3 u_lightScale;      // 1 / (light grid size in metres)
uniform vec3 u_lightTexel;      // half a texel, so a fetch lands on a point and not between two
uniform float u_shapeLightBias; // one light cell PLUS half a voxel: the analytic surface is not
                                // the voxel surface and may be the wrong side of it

vec2 ws_light_at(vec3 world, vec3 N) {
    vec3 at = (world + N * u_shapeLightBias - u_lightOrigin) * u_lightScale;
    at = clamp(at + u_lightTexel, vec3(0.0), vec3(1.0));
    return texture(u_light, at).rg;
}
`;

// ---------------------------------------------------------------------------------------------
// Which material is at a hit point.
//
// THE STUB, and it is a stub: it has never read a paint rule. It hashes the shape's own placement
// — the three flat attributes that are constant across an instance and different between two
// instances — into the material table, so several materials appear at once and every field of a
// real VisualRecord is exercised by something on screen. It is per-shape and not per-point, so a
// shape is one material all over, which the real one will not be.
//
// It goes away the moment web/js/features/paint.js exports MATERIAL_AT_GLSL. That chunk must
// define `uint material_at(vec3 p, vec3 n)` and may declare its own uniforms; gl.js gives it the
// world hit point and the world normal.
const MATERIAL_AT_STUB = `
uint material_at(vec3 p, vec3 n) {
    // Per-shape, not per-point: everything in here is flat.
    vec4 seed = vec4(v_offset, v_kind.z) + vec4(v_p0.xyz, v_p1.x);
    float h = dot(seed, vec4(12.9898, 78.233, 37.719, 4.1414));
    h = fract(sin(h) * 43758.5453);
    int count = max(u_materialCount, 1);
    return uint(clamp(float(count) * h, 0.0, float(count - 1)));
}
`;

// THE CONTROL ARM. `?shapemat=7` paints every shape in the ◉ view with material 7 and nothing
// else, which is the only way to ask whether the two views LIGHT the same — put the same material
// on the same surface in both and the pixels either agree or they do not. It is what caught the
// light-grid bias, and it stays because the question comes back every time either view is touched.
function forcedMaterial() {
    if (typeof location === 'undefined') return -1;
    const asked = new URLSearchParams(location.search).get('shapemat');
    if (asked === null) return -1;
    const index = Number.parseInt(asked, 10);
    return Number.isFinite(index) && index >= 0 ? index : -1;
}

const forced = forcedMaterial();

// paint.js and brdf.js are written by other hands and may simply not be here yet, so they are
// probed rather than imported. A 404 for either in the console is that probe and not a fault.
async function optional(path) {
    try {
        return await import(path);
    } catch (error) {
        return null;
    }
}

const paint = await optional('./paint.js');
const brdf = await optional('./brdf.js');

// ONE PLACE. Swapping either seam is an edit to one of these two functions and nothing else.
function pickMaterialAt() {
    if (forced >= 0) {
        return {
            glsl: 'uint material_at(vec3 p, vec3 n) { return uint(' + forced + '); }\n',
            stubbed: true,
            how: 'forced to ' + forced,
        };
    }
    if (paint && typeof paint.MATERIAL_AT_GLSL === 'string') {
        return { glsl: paint.MATERIAL_AT_GLSL, stubbed: false, how: 'the paint stack' };
    }
    return { glsl: MATERIAL_AT_STUB, stubbed: true, how: 'STUBBED — a hash per shape' };
}

function pickLighting() {
    if (brdf && typeof brdf.SHAPE_LIGHTING_GLSL === 'string') {
        return { glsl: brdf.SHAPE_LIGHTING_GLSL, source: 'features/brdf.js' };
    }
    return { glsl: LIGHTING_GLSL, source: 'the surface shader lobe' };
}

const chosenMaterialAt = pickMaterialAt();
const chosenLighting = pickLighting();

export const materialAtIsStubbed = chosenMaterialAt.stubbed;
export const materialAtSource = chosenMaterialAt.how;
export const lightingSource = chosenLighting.source;

// Everything above main() in SHAPE_FRAGMENT. Order matters: the material struct is declared before
// the lighting that takes one, and `material_at` last because a real one may want either.
export const SHAPE_SHADING_GLSL =
    MATERIAL_GLSL + chosenLighting.glsl + LIGHT_GLSL + chosenMaterialAt.glsl;

// What main() does once it has a hit: the material at the point, the light that reaches it, the
// same lobe the voxels get, and the two things a rasterised view of glass can honestly say about
// transparency without a sorted pass.
//
// `ws_shade_hit` returns LINEAR radiance. The tone map and the transfer stay in gl.js, so the
// curve and the exposure are the ones every other pass in this viewer uses.
export const SHAPE_SHADE_HIT_GLSL = `
vec3 ws_shade_hit(vec3 P, vec3 N, vec3 V) {
    WsMaterial m = ws_material(int(material_at(P, N)));
    vec2 visible = ws_light_at(P, N);

    // No corner occlusion: there are no voxel corners here. The voxel view's own term is 1.0 in
    // the middle of any face, which is where the two views are compared.
    vec3 colour = ws_shade(m, N, V, u_sun, visible.r, visible.g, 1.0);

    // Transparency, and what this view can honestly do about it.
    //
    // The voxel view draws glass in a second blended pass over whatever is behind it, and what it
    // composites is absorb * paneColour * alpha + behind * (1 - alpha) -- the Beer-Lambert tint on
    // the PANE'S OWN colour, over a thickness nobody knows, because a rasteriser has no path
    // length. Both of those terms are transcribed here unchanged, so a pane is the same colour and
    // the same opacity in both views.
    //
    // The one thing that differs is what "behind" is. There is no second pass here — the shapes
    // are one instanced draw of ray-marched boxes and a sorted pass would double the march on the
    // biggest clips — so what shows through a pane in this view is the environment along the
    // refracted direction rather than the stone actually behind it. Over open ground the two are
    // the same picture; over a wall the ◉ view shows sky where the voxel view shows the wall, and
    // that is the honest limit of one pass.
    if (m.opacity < 1.0) {
        float ndv = max(dot(N, V), 1e-4);
        colour *= vec3(1.0) - m.absorb * 0.75;
        float ior = 1.0 + m.iorByte / 128.0;
        vec3 through = refract(-V, N, 1.0 / max(ior, 1.0));
        if (dot(through, through) < 1e-6) through = reflect(-V, N);   // total internal reflection
        // Glancing angles of a pane are more reflective and less see-through, which is the one
        // thing about glass everybody notices when it is missing. The surface shader's own line.
        float cover = clamp(m.opacity + (1.0 - m.opacity) * pow(1.0 - ndv, 4.0), 0.0, 1.0);
        colour = mix(ws_sky(normalize(through)), colour, cover);
    }

    return colour;
}
`;
