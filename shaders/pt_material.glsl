// What a voxel is made of, and how it answers light.
//
// Owned as its own module so that the response of a surface can be worked on without touching
// the transport that asks it. Two halves, and the split is the point:
//
//   - the unpacking of a voxel record into a Material, which is a pure function of a voxel type;
//   - the lobes, which are a pure function of a Material and three directions.
//
// Neither half knows where the ray has been, what it has already collected, or what it will do
// next. That is the transport's business, and keeping it out is what lets a lobe be added here
// without reading pathtrace.comp at all.

struct Material {
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 emission;

    // What a ray can get through, and what happens to it on the way.
    //
    // All three were stored in the voxel record from the beginning and read by nothing, so a
    // clip could declare glass and get paint. `opacity` is how much of a ray the surface stops,
    // `index` is what bends it, and `absorb` is how much colour a metre of the stuff takes out
    // of what passes through — which is the difference between glass and green glass, and
    // between a puddle and deep water.
    float opacity;
    float index;      // refractive index: 1.0 is vacuum
    vec3 absorb;      // Beer-Lambert coefficient per metre

    // How much of what reaches the back of this material comes out of the front.
    //
    // The last of the fields the record has always carried and nothing ever read. It is not
    // transparency: a ray does not go *through* a translucent voxel, it goes in one side,
    // forgets which way it was pointing, and leaves the other diffusely. That is the whole
    // difference between marble and painted stone, between alabaster and plaster, and between a
    // leaf and a piece of green card. See scatter_into and voxel_transmit for how far it gets
    // and why nothing has to say how thick the material is.
    float translucency;

    // Two lobes the record does not carry yet; both read 0 until it does, which costs a shift
    // and an and, and both are worth having ready because the format has a spare byte and these
    // are what it should hold. See the note on kFlagBrushShift for the same argument about bits.
    //
    // `clearcoat` is a second, smooth specular lobe over the first: lacquer, varnish, glaze, the
    // wet look on a fired tile. What makes it read as a coating rather than as a shinier
    // material is that the lobe underneath keeps its own roughness and its own colour and is
    // merely seen *through* the coat.
    //
    // `sheen` is the grazing scatter of a fibre. A cloth is not a rough dielectric: at a glancing
    // angle it goes bright and slightly desaturated where a rough dielectric goes dark, because
    // what the eye catches is the fuzz standing off the weave rather than the weave.
    float clearcoat;
    float sheen;

    // VisualFlags, as declared in world/voxel_type.hpp. Carried whole rather than unpacked into
    // booleans because most of the consumers are outside this file.
    uint flags;
};

// ---------------------------------------------------------------------------------------
// Flags. These mirror VisualFlags in src/world/voxel_type.hpp and the two must agree.

const uint kFlagSmoothSurface = 1u;   // shade from the fill gradient: water, lenses
const uint kFlagDispersive    = 2u;   // wavelength-dependent index

// Declared for completeness and read by nothing, deliberately.
//
// It means "photons are traced through this", and there is no photon pass and cannot be one: a
// photon pass is a second traversal of the world, march is enormous, and the shader already
// carries as many copies of it as the device will take. Caustics here will come from the
// radiance bins — a face already records what arrives along a direction, which is the same
// information a photon map holds and is keyed the way everything else in this renderer is keyed.
// So the flag names a mechanism the architecture has ruled out. It should go; see the report.
const uint kFlagCausticSource = 4u;

// Which way the grain of a brushed surface runs, in two bits: 0 none, 1 x, 2 y, 3 z.
//
// A brushed metal has a direction and its highlight stretches across it, and something has to
// say which direction. A voxel world can honestly provide exactly one thing here: a world axis.
// It has no UVs, no tangent frame and no surface parameterisation — a face is one of six
// axis-aligned squares, and the only directions that exist in its plane and are the same on the
// next face round a corner are the world's own three. So the record names an axis and the
// tangent is that axis projected into whatever face is being shaded. A balustrade brushed along
// y keeps one continuous grain up its whole length and round all four sides, which is what a
// drawn or extruded section actually looks like.
//
// Rejected: circular grain, the turned-on-a-lathe finish of a knob or a spun disc. It needs the
// centre the surface was turned about, and a voxel does not know what object it belongs to. A
// centre could be authored, but then it is a property of a shape and not of a material, and
// every voxel of that material anywhere else in the world would inherit somebody's lathe.
const uint kFlagBrushShift = 3u;
const uint kFlagBrushMask  = 3u;

bool mat_smooth_surface(Material m) { return (m.flags & kFlagSmoothSurface) != 0u; }
bool mat_dispersive(Material m)     { return (m.flags & kFlagDispersive) != 0u; }

// How far apart the two axes of a brushed highlight are pulled.
//
// Fixed rather than authored, because the record has no byte left for it and because the number
// of usefully different answers is small: below about three the stretch is not visible and above
// about ten the highlight becomes a wire. Six is a fine satin finish. Applied as a multiply on
// one axis and a divide on the other so the product of the two widths — which is what sets how
// bright the peak is — does not change, and a brushed surface is therefore neither brighter nor
// darker overall than the isotropic one it replaces.
const float kBrushStretch = 2.45;   // sqrt(6): the two axes end up 6:1

// A lacquer is smooth by definition. If it were rough it would be the material, not a coat on
// it. 0.06 is about the sharpest lobe worth having before it becomes a mirror the size of a
// pixel and starts to sparkle instead of to shine.
const float kCoatRoughness = 0.06;

Material material_of(uint type_id) {
    Material m;
    m.albedo = vec3(0.5);
    m.roughness = 0.5;
    m.metallic = 0.0;
    m.emission = vec3(0.0);
    m.opacity = 1.0;
    m.index = 1.0;
    m.absorb = vec3(0.0);
    m.translucency = 0.0;
    m.clearcoat = 0.0;
    m.sheen = 0.0;
    m.flags = 0u;

    // Clamped, both of them, because a single out-of-range index here does not produce a
    // wrong colour â€” it produces a page fault that takes the device down and the whole
    // process with it. A hit above level 0 carries a filtered colour and no type at all, and
    // its type_id field is whatever happened to be in the register; every caller is supposed
    // to check the level first, and one that forgets should get a wrong pixel rather than a
    // crash report. This cost a device loss: the driver reported four faulting addresses and
    // the checkpoint named the path tracer, and it was one unguarded call.
    uint type_count = uint(types.items.length());
    uint visual_id = types.items[min(type_id, type_count - 1u)].x;
    uint visual_count = uint(visuals.items.length());
    uvec4 v = visuals.items[min(visual_id, visual_count - 1u)];

    // Byte layout of VisualRecord (world/voxel_type.hpp): rgb + opacity, then roughness,
    // metallic, ior, emissive, then absorption and translucency, then the emissive tint, the
    // flags and the spare byte.
    m.albedo = vec3(float(v.x & 0xFFu), float((v.x >> 8u) & 0xFFu),
                    float((v.x >> 16u) & 0xFFu)) / 255.0;
    m.roughness = float(v.y & 0xFFu) / 255.0;
    m.metallic = float((v.y >> 8u) & 0xFFu) / 255.0;
    m.opacity = float((v.x >> 24u) & 0xFFu) / 255.0;
    // The record stores the index as an offset from vacuum so a byte can span the useful range:
    // 1.0 to about 3.0, which covers water at 1.33, glass at 1.5 and diamond at 2.4.
    m.index = 1.0 + float((v.y >> 16u) & 0xFFu) / 128.0;
    // Per metre, and scaled so a byte reaches far enough to make a thick pane visibly green
    // without a thin one being black. Eight is about the strongest useful.
    m.absorb = vec3(float(v.z & 0xFFu), float((v.z >> 8u) & 0xFFu),
                    float((v.z >> 16u) & 0xFFu)) * (8.0 / 255.0);
    m.translucency = float((v.z >> 24u) & 0xFFu) / 255.0;
    m.flags = (v.w >> 16u) & 0xFFu;
    // The spare byte, as two nibbles. Sixteen steps is plenty for either: both are the *strength*
    // of a lobe whose shape is fixed, and nobody can see the difference between a lacquer at
    // 9/15 and one at 10/15. Zero in both nibbles is what an unset record already holds, so
    // every world written before this reads exactly as it did.
    m.clearcoat = float((v.w >> 24u) & 0x0Fu) / 15.0;
    m.sheen = float((v.w >> 28u) & 0x0Fu) / 15.0;

    float emissive = float((v.y >> 24u) & 0xFFu) / 255.0;
    if (emissive > 0.0) {
        uint tint = v.w & 0xFFFFu;   // RGB565
        vec3 colour = vec3(float((tint >> 11u) & 0x1Fu) / 31.0,
                           float((tint >> 5u) & 0x3Fu) / 63.0, float(tint & 0x1Fu) / 31.0);
        // Squared so the byte spans a useful range: a torch and a furnace are not one step
        // apart on a linear scale.
        m.emission = colour * (emissive * emissive * 64.0);
    }
    return m;
}

// Albedo is all a coarse hit gives, so treat it as a rough dielectric. Only reachable when a
// ray leaves the streamed region entirely; primary and bounce rays both run at level 0.
Material material_from_colour(uint packed) {
    Material m;
    m.albedo = vec3(float(packed & 0xFFu), float((packed >> 8u) & 0xFFu),
                    float((packed >> 16u) & 0xFFu)) / 255.0;
    m.roughness = 0.8;
    m.metallic = 0.0;
    m.emission = vec3(0.0);
    m.opacity = 1.0;
    m.index = 1.0;
    m.absorb = vec3(0.0);
    m.translucency = 0.0;
    m.clearcoat = 0.0;
    m.sheen = 0.0;
    m.flags = 0u;
    return m;
}

// ---------------------------------------------------------------------------------------
// The shading normal's microstructure.
//
// There is no way in this renderer to make a surface LOOK rough without MAKING it rough: all
// detail is geometry or per-voxel colour, so a wall of one material is six perfectly flat
// mirrors of it and a highlight crossing that wall is a single unbroken shape. Real stone,
// casting, weathered metal and rolled sheet all break a highlight up at a scale far below the
// geometry, and that break-up is most of what tells the eye which of them it is looking at.
//
// A hash on the voxel gives it for almost nothing. Keyed on the voxel and only on the voxel, so
// it is the same tilt every frame and from every angle — a perturbation that changed with the
// camera would be noise, and noise averaged by the face cache is a slightly darker surface and
// nothing else.
//
// Two rules it must not break:
//
//   - a polished surface must stay polished. The amplitude is proportional to roughness, so a
//     mirror at roughness 0.03 tilts by a third of a degree and a rough stone at 0.9 by nine.
//   - the tilted normal must stay well inside the face's own hemisphere. `from` is offset along
//     the shading normal to leave the surface, and a normal tilted past the face's plane offsets
//     the shadow ray's origin INTO the wall, where it is immediately occluded — which draws
//     black speckle through every lit surface. The guard below is the cheap version of that
//     check and it is not optional.
const float kMicroTilt = 0.18;   // radians of tilt at roughness 1, small-angle

vec3 perturb_normal(vec3 n, ivec3 voxel, ivec3 face, Material m) {
    float amount = m.roughness * kMicroTilt;
    if (amount < 0.004) return n;

    uint h = 0x9E3779B9u;
    h = hash_u32(h ^ uint(voxel.x));
    h = hash_u32(h ^ uint(voxel.y));
    h = hash_u32(h ^ uint(voxel.z));
    // Two uniforms on (-1, 1) from one hash. The second word is the first hashed again rather
    // than another mix of the coordinates, which would correlate the two axes along a lattice
    // the same way an XOR of products correlates face keys.
    float jx = float(h) * (2.0 / 4294967296.0) - 1.0;
    float jy = float(hash_u32(h)) * (2.0 / 4294967296.0) - 1.0;

    // Built on the face rather than on n, so every pixel standing on one voxel gets one tilt
    // even when the shading normal underneath varies across it.
    vec3 f = vec3(face);
    vec3 t = (abs(f.z) < 0.5) ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 b = normalize(cross(f, t));
    t = cross(b, f);

    vec3 tilted = normalize(n + t * (jx * amount) + b * (jy * amount));
    // Well inside the hemisphere, not merely on the right side of it: see above.
    return (dot(tilted, f) > 0.25) ? tilted : n;
}

// ---------------------------------------------------------------------------------------
// The lobes.

// The direction the grain runs in, here, on this face. Zero when there is no grain, and zero
// when the grain runs along the face's own normal — which is not a degenerate case to be
// papered over with an arbitrary tangent, it is the honest answer: the end of a brushed bar has
// no grain across it, and giving it one puts a stretched highlight on the cut end of every
// baluster in the building.
vec3 brush_tangent(Material m, vec3 n) {
    uint axis = (m.flags >> kFlagBrushShift) & kFlagBrushMask;
    if (axis == 0u) return vec3(0.0);
    vec3 a = (axis == 1u) ? vec3(1, 0, 0) : (axis == 2u) ? vec3(0, 1, 0) : vec3(0, 0, 1);
    vec3 t = a - n * dot(a, n);
    float len2 = dot(t, t);
    if (len2 < 1e-3) return vec3(0.0);
    return t * inversesqrt(len2);
}

// Everything a surface does with light arriving from one direction: the BRDF times the cosine,
// so the caller multiplies by the radiance along `l` and by nothing else.
//
// `v` points at the eye and `l` at the light. Both leave the surface, which is the convention
// worth stating once because getting it backwards puts every highlight on the wrong side.
vec3 surface_response(Material m, vec3 n, vec3 v, vec3 l) {
    float n_dot_l = dot(n, l);
    if (n_dot_l <= 0.0) return vec3(0.0);

    float n_dot_v = max(dot(n, v), 1e-4);
    vec3 h = normalize(l + v);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(v, h), 1e-4);

    float a = max(m.roughness * m.roughness, 1e-3);

    // The base specular lobe, stretched across the grain if the record named one.
    float d;
    vec3 t = brush_tangent(m, n);
    if (dot(t, t) > 0.0) {
        // Narrow ALONG the grain and wide across it, which is the way round that puts the
        // stretch in the highlight perpendicular to the brush marks. A groove is smooth down its
        // length and curved across it, so the micro-normals fan out across the grain and the
        // lobe fans with them. Rendered the other way first and it was obvious: the streak ran
        // down the brushed sphere parallel to its grain, which is what a spun disc would show as
        // rings instead of the radial rays it actually shows.
        vec3 b = cross(n, t);
        float at = a / kBrushStretch;
        float ab = a * kBrushStretch;
        float x = dot(h, t) / at;
        float y = dot(h, b) / ab;
        float k = x * x + y * y + n_dot_h * n_dot_h;
        d = 1.0 / max(kPi * at * ab * k * k, 1e-6);
    } else {
        d = a * a / max(kPi * pow(n_dot_h * n_dot_h * (a * a - 1.0) + 1.0, 2.0), 1e-6);
    }
    // Shadowing stays isotropic, on the geometric mean of the two widths — which is `a`, since
    // the stretch multiplies one axis by exactly what it divides the other by. The anisotropic
    // Smith term is two more square roots for a difference that shows up at the very edge of a
    // grazing highlight and nowhere else.
    float g = ggx_smith(n_dot_v, n_dot_l, m.roughness);
    vec3 f0 = mix(vec3(0.04), m.albedo, m.metallic);
    vec3 f = f0 + (vec3(1.0) - f0) * pow(1.0 - v_dot_h, 5.0);
    vec3 specular = d * g * f / max(4.0 * n_dot_v * n_dot_l, 1e-4);

    vec3 diffuse = m.albedo * (1.0 - m.metallic) / kPi;

    // Fibre fuzz. The same Fresnel-shaped factor as everything else here, added to the diffuse
    // lobe rather than to the specular one, because what it describes is scattering off strands
    // standing in every direction and not a reflection off anything flat.
    if (m.sheen > 0.0) {
        diffuse += m.albedo * (m.sheen * pow(1.0 - v_dot_h, 5.0)) / kPi;
    }

    vec3 response = diffuse + specular;

    // The lacquer, over the top of whatever that came to. Its own narrow lobe, its own
    // dielectric Fresnel — a coat is clear, so its reflectance is 0.04 whatever the metal under
    // it is doing — and the material beneath dimmed by what the coat sent back, because
    // everything below is seen through it.
    if (m.clearcoat > 0.0) {
        float ac = kCoatRoughness * kCoatRoughness;
        float dc = ac * ac / max(kPi * pow(n_dot_h * n_dot_h * (ac * ac - 1.0) + 1.0, 2.0), 1e-6);
        float gc = ggx_smith(n_dot_v, n_dot_l, kCoatRoughness);
        float fc = 0.04 + 0.96 * pow(1.0 - v_dot_h, 5.0);
        float coat = m.clearcoat * dc * gc * fc / max(4.0 * n_dot_v * n_dot_l, 1e-4);
        response = response * (1.0 - m.clearcoat * fc) + vec3(coat);
    }

    return response * n_dot_l;
}

// ---------------------------------------------------------------------------------------
// Going in one side and coming out the other.
//
// A translucent material is not a transparent one and the difference is the whole of it. A ray
// does not pass THROUGH marble; it goes in, is knocked about until it has forgotten which way it
// was pointing, and leaves somewhere else. That is why a marble panel with the sun behind it
// glows evenly instead of showing you the sun, why the thin parts of a carving light up and the
// thick parts do not, and why a leaf is a leaf rather than a piece of green glass.
//
// So it is handled where transmission is already handled: the loop in pathtrace.comp that
// marches, decides what an interface does, and marches again. A translucent surface is one more
// answer that loop can give, and it costs no ray — march is mentioned once inside it and this
// adds no mention. What it does add is a direction that no longer has anything to do with the
// one that arrived.
//
// Rejected, and worth writing down because it is the obvious design and it does not work: read
// the light on the BACK face out of the face table, keyed (node, level, -normal, kind) like
// everything else, and let a buried back face read unknown so thickness ends the bleed for free.
// The keying is right and the table would hold it. Nothing would ever be in it. Every entry in
// that table is written by a pixel standing on the face it belongs to — bounces contribute to
// where they STARTED, never to what they hit — so the far side of a panel the camera cannot see
// has never been measured and never will be, and the whole term reads zero in exactly the frames
// it was written for. A cache can only tell you what somebody has already looked at.

// How deep light gets into this material before it is gone, at full translucency, in VOXELS.
//
// Voxels rather than metres because that is the unit the marcher charges absorption in — it
// counts voxels crossed, not distance travelled — and because it is the only honest unit here: a
// voxel is the thinnest thing this world can build, so how far light gets into marble is a
// number of voxels or it is nothing. Squared, so the useful range sits low: a marble authored at
// a third of the byte reaches under one voxel and only a lip or an arris glows, and alabaster at
// the top of it reaches six and a whole thin panel does.
const float kTranslucentVoxels = 6.0;

// How much of the direction it arrived with a scattered ray keeps. Nought would be a perfect
// diffuser and is wrong for everything: a slab passes light on preferentially the way it was
// already travelling, which is why a backlit leaf blazes when you look towards the sun through
// it and is merely bright from the side.
const float kForwardBias = 0.4;

// How much of a ray survives one voxel of this stuff. Answers the marcher's WS_VOXEL_TRANSMIT
// for a material that is solid but not opaque to light.
//
// The exponential is what makes thickness matter at all, and it is the whole reason nothing has
// to author a thickness: at a third of the byte a lip one voxel thick passes a fifth of what
// reaches it, three voxels pass under a hundredth, and a wall passes nothing. The renderer finds
// out how thin a carving is by counting the voxels a ray crosses, which is the one measurement
// this world can always make.
//
// Grey, because the marcher's transmittance is one float. The colour of light that has been
// inside the stuff is put on where it goes in — see the tint in the call site — which is the
// same place a pane of glass gets its.
float voxel_transmit(Material m) {
    if (m.translucency <= 0.0) return 0.0;
    float depth = kTranslucentVoxels * m.translucency * m.translucency;
    return exp(-1.0 / max(depth, 0.05));
}

// Whether a ray reaching this surface goes into the material rather than stopping at it, and
// which way it is pointing once it has.
//
// `face` points out of the surface, `dir_in` is the ray that arrived. The draw is the honest
// energy split: a translucency of a fifth means a fifth of the paths go through carrying what is
// behind and four fifths shade the front exactly as they always did, so the surface neither
// gains nor loses light by being translucent — it moves some of it to the other side.
bool scatter_into(Material m, vec3 dir_in, vec3 face, inout uint state, out vec3 dir_out) {
    dir_out = dir_in;
    if (m.translucency <= 0.0) return false;
    if (rand(state) >= m.translucency) return false;

    vec3 inward = cosine_hemisphere(-face, state);
    dir_out = normalize(mix(inward, dir_in, kForwardBias));
    // The mix can only fail to point inward if dir_in did, which means the caller handed us a
    // face it was leaving rather than entering. Fall back to the pure diffuse draw rather than
    // send a ray back out of the surface it just entered.
    if (dot(dir_out, face) >= 0.0) dir_out = inward;
    return true;
}

// ---------------------------------------------------------------------------------------
// Dispersion, for the flag that asks for it.
//
// One ray cannot carry three indices, and there is no budget for three rays — a second march is
// a second inlined copy of an enormous function and the device does not survive a fourth. So a
// dispersive interface picks one channel per sample and the other two are carried by the samples
// that picked them: the estimate is unbiased, the tint below puts back the two thirds it did not
// trace, and what the eye sees converge is the spectrum spreading rather than one ray splitting.
//
// The spread is expressed as a fraction of the index rather than as an Abbe number, because the
// record stores an index in a byte and has nowhere to put a second material constant. Two per
// cent across the visible band is about right for a dense flint and is generous for a window,
// which is the correct way round: a flag that has to be asked for should show when it is.
const float kDispersion = 0.02;

float dispersed_index(Material m, float u, out vec3 tint) {
    if (!mat_dispersive(m)) {
        tint = vec3(1.0);
        return m.index;
    }
    // Red bends least, blue most, which is the direction that makes a prism throw red to the
    // inside of the fan. Getting it backwards is invisible on a sphere and obvious on an edge.
    if (u < (1.0 / 3.0)) {
        tint = vec3(3.0, 0.0, 0.0);
        return m.index * (1.0 - kDispersion);
    }
    if (u < (2.0 / 3.0)) {
        tint = vec3(0.0, 3.0, 0.0);
        return m.index;
    }
    tint = vec3(0.0, 0.0, 3.0);
    return m.index * (1.0 + kDispersion);
}
