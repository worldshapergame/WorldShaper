// The rasteriser.
//
// The game renders these clips with a path tracer, and a phone cannot. So this draws the same
// matter with the same materials the honest way a rasteriser can: one instanced quad per merged
// voxel face, a Cook-Torrance lobe per light, and every term that needs to know what is between a
// surface and the sky read out of the light grid the baker cast.
//
// What it deliberately does NOT do is fake the path tracer. There are no screen-space reflections,
// no ambient occlusion pass, no denoiser. A mirror is a rough metal with a sky in it, glass is
// blended, and neither claims to be what the game shows. What it IS faithful about is the matter:
// the geometry is the clip's own voxels and the materials are its own VisualRecords, unquantised.
//
// # Three passes and a cap
//
//   sky          a full-screen gradient with the sun in it
//   opaque       depth written; back faces culled only when nothing is sliced
//   transparent  blended, depth tested, depth not written
//
//   cap          the cut face itself, where the plane passes through solid matter
//
// # The cap, which has now been wrong in both directions
//
// Slicing is a clip plane, and a clip plane through a voxel mesh leaves an OPEN SHELL, because a
// mesh has only the faces that touch air and solid stone has no faces inside it at all. A wall cut
// in half is therefore two sheets of paper with a gap between them, and a building cut in half is
// see-through where it should be stone.
//
// It was first filled with a flat grey quad through a stencil parity pass. That was removed,
// because one flat grey covering the whole cut paints over the rooms the slider exists to open up
// -- reported as *"make the slicing show the inside of the clip, not culling the faces of sliced
// voxels"*. And removing it produced the opposite fault, reported next as *"the slicer not showing
// sliced voxels faces"*: the model went hollow and shredded, every cut wall a pair of shells with
// daylight between them.
//
// BOTH REPORTS ARE THE SAME REQUEST AND A PARITY CAP IS WHAT SATISFIES IT. The stencil inverts on
// every fragment of the geometry BEHIND the plane, so odd parity falls exactly where the plane is
// inside matter and nowhere else. A room is air, its parity is even, and it is not filled -- so
// you still see into it. A wall is stone, its parity is odd, and it is filled -- so it reads as
// cut stone. The old pass had the right mechanism and the wrong paint.
//
// So the cap is drawn, and it is drawn as VOXELS rather than as a plane:
//
//   the clip's own stone, not a fixed grey. `capColour` is the clip's commonest opaque material,
//   taken from the quad histogram at load, so a marble clip cuts marble-coloured and the facility
//   cuts the colour of what it is mostly made of.
//   a voxel lattice at the clip's own pitch, because the thing being asked for is the FACES of the
//   sliced voxels, and a cut through a voxel model is a grid of little square faces. Without it a
//   cap is a poured slab at any resolution and tells you nothing about how finely the clip is cut.
//   the light grid, sampled at the cut, so a cap deep inside a building is dark and one at an
//   outside wall is not -- the same volume every other surface here reads.
//
// It is skipped when the eye is on the discarded side, where the parity is counted from the wrong
// end. AND ITS HONEST LIMIT, because it is a real one: the cap is ONE material per clip. The file
// carries the exposed surface and a one-bit occupancy grid and no material volume, so there is
// nothing to ask what the stone at a given point inside a wall is made of. Cutting through the
// rotunda's porphyry floor shows the building's commonest stone, not porphyry. Fixing that means
// baking a material volume -- one byte a cell on the occupancy grid -- and is written up in
// documentation/24-clip-viewer.md.

// The shape record's size and the cutter's are the file's, not this file's opinion of them: they
// are written down once in web/js/format.js and once in tools/bake_web.cpp, and nowhere else.
import { SHAPE_BYTES, CUTTER_TEXELS } from './format.js';
// >>> matvol
// The material volume and the thickness field. The GLSL is pasted into whichever shaders want it
// and the class owns the three textures; see web/js/features/matvol.js for the signatures.
import { MATVOL_GLSL, Matvol } from './features/matvol.js';
// <<< matvol

const VERTEX_SOURCE = `#version 300 es
precision highp float;

layout(location = 0) in vec3 a_cell;
layout(location = 1) in vec2 a_size;
layout(location = 2) in float a_material;
layout(location = 3) in float a_ao;

uniform mat4 u_viewProj;
uniform vec3 u_origin;     // metres at cell (0, 0, 0)
uniform float u_scale;     // metres per voxel
uniform vec3 u_offset;     // the face's own axis, for the three faces on the far side of a cell
uniform vec3 u_eu;         // the face's two in-plane axes, in the order the baker merged along
uniform vec3 u_ev;
uniform float u_flip;      // 1 on a negative face, which is what keeps the winding consistent

out vec3 v_world;
out float v_ao;
flat out int v_material;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 uv = (u_flip > 0.5) ? corner.yx : corner;

    vec3 cell = a_cell + u_offset + u_eu * (uv.x * a_size.x) + u_ev * (uv.y * a_size.y);
    v_world = u_origin + cell * u_scale;

    int packed = int(a_ao + 0.5);
    int which = int(uv.x) + 2 * int(uv.y);
    v_ao = float((packed >> (which * 2)) & 3) / 3.0;

    v_material = int(a_material + 0.5);
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}`;

// The shading. Every constant in here is a look rather than a measurement, and it is written to
// flatter stone in daylight because that is what the facility is.
const FRAGMENT_SOURCE = `#version 300 es
precision highp float;
precision highp sampler3D;

in vec3 v_world;
in float v_ao;
flat in int v_material;

uniform sampler2D u_materials;
uniform sampler3D u_light;
uniform vec3 u_lightOrigin;
uniform vec3 u_lightScale;    // 1 / (light grid size in metres)
uniform vec3 u_lightTexel;    // half a texel, so a fetch lands on a point and not between two
uniform float u_lightBias;    // how far along the normal to sample, in metres

uniform vec3 u_normal;
uniform vec3 u_eye;
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;
uniform float u_exposure;
uniform vec4 u_clip;          // dot(world, xyz) + w > 0 is cut away
uniform float u_cutSide;      // 1 draws only what the slice removes, for the stencil pass
uniform float u_blended;      // 1 on the blended pass, where the material's opacity is used

out vec4 o_colour;

vec4 material_row(int which) {
    int at = v_material * 4 + which;
    return texelFetch(u_materials, ivec2(at & 255, at >> 8), 0);
}

float ggx(float ndh, float rough) {
    float a = rough * rough;
    float d = ndh * ndh * (a * a - 1.0) + 1.0;
    return (a * a) / (3.14159265 * d * d + 1e-7);
}

float smith(float ndv, float ndl, float rough) {
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnel(vec3 f0, float vdh) {
    float f = pow(1.0 - vdh, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

vec3 sky_colour(vec3 direction) {
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 base = mix(u_skyDown, u_skyUp, up);
    // The sun's own disc, and a wide glow round it. Both are what a rough metal reflects, and
    // without them a mirror in this viewer reflects a flat blue card.
    float towards = max(dot(direction, u_sun), 0.0);
    base += u_sunColour * pow(towards, 400.0) * 3.0;
    base += u_sunColour * pow(towards, 8.0) * 0.05;
    return base;
}

// ACES, in the fitted form. A path tracer's output needs a curve like this and so does anything
// with a sun in it: without one every lit face of pale limestone is the same clipped white.
vec3 tonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    float side = dot(v_world, u_clip.xyz) + u_clip.w;
    if (u_cutSide > 0.5) {
        if (side <= 0.0) discard;
    } else if (side > 0.0) {
        discard;
    }

    vec4 base = material_row(0);
    vec4 surface = material_row(1);
    vec4 depth = material_row(2);
    vec4 extra = material_row(3);

    vec3 albedo = base.rgb * base.rgb;            // close enough to sRGB, and one multiply
    float opacity = base.a;
    float rough = clamp(surface.r, 0.045, 1.0);
    float metal = surface.g;
    float iorByte = surface.b * 255.0;
    float emissive = surface.a;
    float translucency = depth.a;

    int flags = int(extra.b * 255.0 + 0.5);
    int coat = int(extra.a * 255.0 + 0.5);
    float clearcoat = float(coat & 15) / 15.0;
    float sheen = float((coat >> 4) & 15) / 15.0;

    vec3 N = u_normal;
    vec3 V = normalize(u_eye - v_world);
    if (dot(N, V) < 0.0) N = -N;                  // the far side of a pane, seen through the near
    vec3 L = u_sun;
    vec3 H = normalize(L + V);

    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 1e-4);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);

    // What the baker cast: how much of the sun and how much of the sky reach this place. Sampled
    // a little along the normal so that a face reads the air in front of it rather than the stone
    // it is the surface of.
    vec3 at = (v_world + N * u_lightBias - u_lightOrigin) * u_lightScale;
    at = clamp(at + u_lightTexel, vec3(0.0), vec3(1.0));
    vec2 visible = texture(u_light, at).rg;
    float sunVisible = visible.r;
    float skyVisible = visible.g;

    // Corner occlusion is a voxel's own shape and the light grid is the room it stands in; both
    // are needed, and neither substitutes for the other.
    float occluded = mix(0.35, 1.0, v_ao) * mix(0.25, 1.0, skyVisible);

    float dielectric = (iorByte > 0.5)
        ? pow((iorByte / 128.0) / (2.0 + iorByte / 128.0), 2.0)
        : 0.04;
    vec3 f0 = mix(vec3(dielectric), albedo, metal);

    vec3 ambient = mix(u_skyDown, u_skyUp, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) * 0.5;
    vec3 direct = u_sunColour * ndl * sunVisible;

    vec3 diffuse = albedo * (1.0 - metal) * (direct + ambient * occluded);

    // Translucent matter lights from behind: leaves, thin marble, wax. One term, and it is the
    // only place in this shader where light arrives through something.
    if (translucency > 0.0) {
        float through = max(dot(-N, L), 0.0) * 0.6 + 0.25;
        diffuse += albedo * translucency * through * u_sunColour * sunVisible;
    }

    // Brushed metal. A voxel world has no UVs and no tangent frame, so a VisualRecord can name
    // exactly one thing here — which of the three world axes the grain runs along — and that is
    // what bits 3 and 4 hold. The grain is projected into the face and the highlight is stretched
    // across it, which is the whole visible difference between brushed bronze and brown paint.
    // On a face the grain runs straight out of, there is no direction left and it stays round.
    float distribution = ggx(ndh, rough);
    int brush = (flags >> 3) & 3;
    if (brush != 0) {
        vec3 axis = (brush == 1) ? vec3(1.0, 0.0, 0.0)
                  : (brush == 2) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
        vec3 along = axis - N * dot(N, axis);
        float reach = length(along);
        if (reach > 0.05) {
            along /= reach;
            vec3 across = cross(N, along);
            float alpha = rough * rough;
            float ax = max(alpha * 0.35, 0.002);
            float ay = max(alpha * 1.60, 0.002);
            float th = dot(along, H) / ax;
            float bh = dot(across, H) / ay;
            float d = th * th + bh * bh + ndh * ndh;
            distribution = 1.0 / (3.14159265 * ax * ay * d * d + 1e-7);
        }
    }

    vec3 specular = fresnel(f0, vdh) * distribution * smith(ndv, ndl, rough) /
                    (4.0 * ndv * max(ndl, 1e-4) + 1e-4) * direct * ndl;

    // The sky, reflected. A rough surface takes a blurred sky, which here is the sky colour along
    // the reflection lerped towards the flat ambient — cheap, and it is what makes bronze read as
    // metal rather than as brown paint.
    vec3 R = reflect(-V, N);
    vec3 reflected = mix(sky_colour(R), ambient, rough * rough);
    vec3 ambientSpecular = reflected * fresnel(f0, ndv) * mix(0.25, 1.0, skyVisible) *
                           mix(0.4, 1.0, v_ao);

    vec3 colour = diffuse + specular + ambientSpecular;

    if (clearcoat > 0.0) {
        float lacquer = ggx(ndh, 0.1) * smith(ndv, ndl, 0.1) / (4.0 * ndv * max(ndl, 1e-4) + 1e-4);
        colour += clearcoat * 0.25 * (lacquer * direct * ndl + reflected * 0.15 * skyVisible);
    }
    if (sheen > 0.0) {
        float rim = pow(1.0 - ndv, 3.0);
        colour += sheen * rim * albedo * (ambient * skyVisible + direct * 0.3);
    }
    if (emissive > 0.0) {
        int tint = int(extra.r * 255.0 + 0.5) | (int(extra.g * 255.0 + 0.5) << 8);
        vec3 glow = vec3(float((tint >> 11) & 31) / 31.0,
                         float((tint >> 5) & 63) / 63.0,
                         float(tint & 31) / 31.0);
        colour += glow * emissive * 6.0;
    }

    // Beer-Lambert, over a thickness nobody knows. A rasteriser has no path length, so this takes
    // the material's absorption as a tint on what shows through and says so rather than pretending
    // to integrate anything.
    float alpha = (u_blended < 0.5) ? 1.0 : opacity;
    if (alpha < 1.0) {
        vec3 absorb = vec3(1.0) - depth.rgb * 0.75;
        colour *= absorb;
        // Glancing angles of a pane are more reflective and less see-through, which is the one
        // thing about glass everybody notices when it is missing.
        alpha = clamp(alpha + (1.0 - alpha) * pow(1.0 - ndv, 4.0), 0.0, 1.0);
    }

    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), alpha);
}`;

const SKY_VERTEX = `#version 300 es
precision highp float;
uniform mat4 u_invViewProj;
uniform vec3 u_eye;
out vec3 v_ray;
void main() {
    vec2 corner = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1));
    vec4 far = u_invViewProj * vec4(corner, 1.0, 1.0);
    v_ray = far.xyz / far.w - u_eye;
    gl_Position = vec4(corner, 1.0, 1.0);
}`;

const SKY_FRAGMENT = `#version 300 es
precision highp float;
in vec3 v_ray;
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;
uniform float u_exposure;
out vec4 o_colour;
vec3 tonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 direction = normalize(v_ray);
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 colour = mix(u_skyDown, u_skyUp, up);
    float towards = max(dot(direction, u_sun), 0.0);
    colour += u_sunColour * pow(towards, 900.0) * 4.0;
    colour += u_sunColour * pow(towards, 8.0) * 0.05;
    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), 1.0);
}`;



// --- the cut face ----------------------------------------------------------------------------
//
// One quad on the slice plane, drawn through the stencil parity the pass before it built, shaded
// as cut voxels rather than as a poured slab. See the note at the top of this file for why it is
// a parity cap and not a flat fill, and for the one material it cannot know.
const CAP_VERTEX = `#version 300 es
precision highp float;
uniform mat4 u_viewProj;
uniform vec3 u_capOrigin;   // a corner of the plane, in metres
uniform vec3 u_capU;        // and its two spans, so the quad covers the whole clip
uniform vec3 u_capV;
out vec3 v_world;
void main() {
    vec2 corner = vec2((gl_VertexID & 1), (gl_VertexID >> 1));
    v_world = u_capOrigin + u_capU * corner.x + u_capV * corner.y;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}`;

const CAP_FRAGMENT = `#version 300 es
precision highp float;
precision highp sampler3D;
in vec3 v_world;
uniform vec3 u_capColour;
uniform vec3 u_capNormal;
uniform float u_capVoxel;     // metres per voxel, for the lattice
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;
uniform float u_exposure;
uniform sampler3D u_light;
uniform vec3 u_lightOrigin;
uniform vec3 u_lightScale;
uniform vec3 u_lightTexel;
// >>> matvol
uniform sampler2D u_materials;
${MATVOL_GLSL}
// <<< matvol
out vec4 o_colour;

vec3 tonemap(vec3 c) { return (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14); }

void main() {
    // The lattice. A cut through a voxel model is a grid of little square faces, and this is the
    // whole of what makes the cap read as sliced voxels rather than as a plane through them. The
    // line is drawn in the two axes that lie IN the plane, and its width is a fixed fraction of a
    // voxel so it neither vanishes at a coarse clip nor swamps a fine one.
    vec3 cell = v_world / max(u_capVoxel, 1e-5);
    vec3 edge = abs(fract(cell) - 0.5);
    vec3 wide = fwidth(cell);
    // The plane's own axis is excluded: along it every point of the cap has the same coordinate,
    // so its fract() is a constant and would either darken the whole cap or none of it.
    vec3 keep = 1.0 - abs(u_capNormal);
    float line = 0.0;
    for (int i = 0; i < 3; ++i) {
        float k = keep[i];
        if (k < 0.5) continue;
        line = max(line, 1.0 - smoothstep(0.0, max(wide[i], 1e-5) * 1.5, 0.5 - edge[i]));
    }

    // Sampled half a voxel OUT of the cut, on the side the slice opened, and not into the stone.
    //
    // Into the stone is where the matter is and it is the wrong place to ask: a lattice point
    // buried in a wall has no light of its own, so a cap read from there comes out black. That is
    // not even honest -- the cut face did not exist until the slider made it, and what lights it
    // is the opening, not the rock it was quarried from.
    vec3 at = (v_world + u_capNormal * u_capVoxel * 0.5 - u_lightOrigin) * u_lightScale;
    at = clamp(at + u_lightTexel, vec3(0.0), vec3(1.0));
    vec2 visible = texture(u_light, at).rg;

    // ...and a floor under the ambient term, for the same reason. Cut into the middle of a solid
    // podium and the point just outside the cut is still buried, so its sky term is nearly nothing
    // and the cross-section disappears into the background. A quarter is enough to read the shape
    // by and low enough that a cap in a sunlit wall is still obviously brighter than one four
    // metres inside the building.
    // >>> matvol
    // WHAT THE STONE AT THIS POINT ACTUALLY IS.
    //
    // This was one colour for the whole clip -- u_capColour, the commonest opaque material by
    // area -- because the file had no material volume and there was nothing to ask. There is one
    // now, so a cut through the rotunda's floor comes out porphyry where the bands are, lapis in
    // the ring and verde in the outer field, instead of the building's limestone everywhere.
    // (No back-quotes anywhere in here: this is inside a template string.)
    //
    // Half a CELL back from the plane rather than on it: the cut face did not exist until the
    // slider made it, the point exactly on it is on the boundary between the cell that was kept
    // and the cell that was thrown away, and floor() there answers whichever way the arithmetic
    // falls. The plane itself is tried second, for a sheet thinner than a cell where stepping back
    // walks straight out of it, and the clip's commonest stone is still the answer when both miss.
    vec3 albedo = u_capColour;
    int material = ws_material_at(v_world - u_capNormal * 0.5 / u_matvolCells);
    if (material < 0) material = ws_material_at(v_world);
    if (material >= 0) {
        int row = material * 4;
        vec4 base = texelFetch(u_materials, ivec2(row & 255, row >> 8), 0);
        albedo = base.rgb * base.rgb;   // sRGB in the file, linear here, like every other colour
    }
    // <<< matvol

    vec3 N = u_capNormal;
    vec3 ambient = mix(u_skyDown, u_skyUp, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) *
                   max(visible.g, 0.25);
    float ndl = max(dot(N, u_sun), 0.0);
    vec3 colour = albedo * (ambient + u_sunColour * ndl * visible.r);
    // The lattice darkens rather than lightens: a joint between two voxels is a place light does
    // not reach, which is the same reason every other seam in this viewer is dark.
    colour *= mix(1.0, 0.72, line);
    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), 1.0);
}`;

// --- the clip before it was voxels ---------------------------------------------------------
//
// One instanced box per shape the author wrote, and the fragment shader sphere-traces that shape's
// own signed distance inside it. Nothing here is voxelised: what you see is the true surface at
// whatever distance you look from, which is the entire point of the view.
//
// The shape's own space is reached by a 3x4 matrix the baker accumulated on the way down the
// field, so `eval`'s own descent is what places it and nothing had to be inverted.
//
// **It shows the RESOLVED shape, not the ingredients.** This used to march every leaf alone, with
// a sign saying whether a `difference` took it away, and draw the taken-away ones in red. So a
// doorway was a red slab standing in front of its wall instead of a hole through it, and every
// overlap anybody had written was on screen as raw overlapping primitives. Each shape now carries
// a range into a pool of CUTTERS -- the leaves of every `difference` subtrahend above it, filtered
// to the ones whose box actually reaches it -- and the march does `d = max(d, -d_cutter)` for each
// one. That is exact subtraction, it costs a fetch and an evaluation per cutter per step rather
// than a walk of the whole tree, and there is no colour for "this one is a hole" any more because
// the holes are holes.
//
// The pool is an RGBA32F texture, six texels a cutter, because WebGL2 has no storage buffers.
const SHAPE_VERTEX = `#version 300 es
precision highp float;

layout(location = 0) in vec4 a_row0;
layout(location = 1) in vec4 a_row1;
layout(location = 2) in vec4 a_row2;
layout(location = 3) in vec4 a_p0;
layout(location = 4) in vec4 a_p1;
layout(location = 5) in vec4 a_kind;   // op, cut_start, scale, cut_count
layout(location = 6) in vec3 a_low;
layout(location = 7) in vec3 a_high;

uniform mat4 u_viewProj;

out vec3 v_world;
flat out mat3 v_linear;
flat out vec3 v_offset;
flat out vec4 v_p0;
flat out vec4 v_p1;
flat out vec4 v_kind;
flat out vec3 v_low;
flat out vec3 v_high;

void main() {
    // A cube as a fourteen-vertex strip, so a shape costs one instance and no index buffer.
    int id = gl_VertexID;
    const int strip[14] = int[14](3, 7, 1, 5, 4, 7, 6, 3, 2, 1, 0, 4, 2, 6);
    int corner = strip[id];
    vec3 unit = vec3(float(corner & 1), float((corner >> 1) & 1), float((corner >> 2) & 1));
    v_world = mix(a_low, a_high, unit);

    v_linear = mat3(a_row0.xyz, a_row1.xyz, a_row2.xyz);   // columns; transposed on use
    v_offset = vec3(a_row0.w, a_row1.w, a_row2.w);
    v_p0 = a_p0;
    v_p1 = a_p1;
    v_kind = a_kind;
    v_low = a_low;
    v_high = a_high;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}`;

const SHAPE_FRAGMENT = `#version 300 es
precision highp float;

in vec3 v_world;
flat in mat3 v_linear;
flat in vec3 v_offset;
flat in vec4 v_p0;
flat in vec4 v_p1;
flat in vec4 v_kind;
flat in vec3 v_low;
flat in vec3 v_high;

uniform vec3 u_eye;
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;
uniform float u_exposure;
uniform vec4 u_clip;
uniform mat4 u_viewProj;
uniform highp sampler2D u_cutters;   // six RGBA32F texels a cutter, texelFetch only
uniform int u_cutterWidth;           // texels across, so an index becomes a row and a column

out vec4 o_colour;

// One shape may subtract at most this many, and the baker warns when it had to drop some. A GLSL
// ES loop wants a constant upper bound, so this is that bound as well as the cap.
const int MAX_CUTTERS = 16;

// The baker's own numbering, not the enum's. See web_op in tools/bake_web.cpp for why the enum's
// values must never reach this file. (No back-quotes in here: this is inside a template string.)
const int OP_SPHERE = 0;
const int OP_BOX = 1;
const int OP_CYLINDER = 2;
const int OP_CAPSULE = 3;
const int OP_TORUS = 4;
const int OP_CONE = 5;
const int OP_PLANE = 6;
const int OP_ELLIPSOID = 7;

vec3 along(int axis, vec3 p) {
    return (axis == 0) ? p.yzx : ((axis == 1) ? p.xzy : p.xyz);
}

float shape_distance(int op, vec4 a, vec4 b, vec3 p) {
    if (op == OP_SPHERE) {
        return length(p - a.xyz) - a.w;
    } else if (op == OP_BOX) {
        vec3 d = abs(p - a.xyz) - vec3(a.w, b.x, b.y) + vec3(b.z);
        return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, 0.0)) - b.z;
    } else if (op == OP_ELLIPSOID) {
        vec3 r = vec3(a.w, b.x, b.y);
        vec3 q = (p - a.xyz) / max(r, vec3(1e-5));
        float k = length(q);
        return (k - 1.0) * min(r.x, min(r.y, r.z));
    } else if (op == OP_CYLINDER) {
        vec3 q = along(int(b.y + 0.5), p - a.xyz);
        vec2 d = abs(vec2(length(q.xy), q.z)) - vec2(a.w, b.x);
        return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
    } else if (op == OP_CAPSULE) {
        vec3 p0 = a.xyz;
        vec3 p1 = vec3(a.w, b.x, b.y);
        vec3 pa = p - p0, ba = p1 - p0;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-9), 0.0, 1.0);
        return length(pa - ba * h) - b.z;
    } else if (op == OP_TORUS) {
        vec3 q = along(int(b.y + 0.5), p - a.xyz);
        vec2 t = vec2(length(q.xy) - a.w, q.z);
        return length(t) - b.x;
    } else if (op == OP_CONE) {
        vec3 q = along(int(b.y + 0.5), p - a.xyz);
        float h = b.x;
        float r = a.w;
        vec2 d = vec2(length(q.xy) - r * clamp(1.0 - q.z / max(h, 1e-5), 0.0, 1.0),
                      abs(q.z - h * 0.5) - h * 0.5);
        return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
    } else if (op == OP_PLANE) {
        return dot(p, a.xyz) - a.w;
    }
    return 1e9;
}

// World -> this shape's own space, transposed out of the three attribute rows. Set once in main
// because the distance below is asked for it seven times a step.
mat3 g_linear;
vec3 g_offset;
float g_scale;

vec4 cutter_texel(int index) {
    return texelFetch(u_cutters, ivec2(index % u_cutterWidth, index / u_cutterWidth), 0);
}

// The shape as the clip actually resolves it: itself, minus everything the differences above it
// take away. max(d, -d_cutter) is exact subtraction, and it is what turns a red slab standing in
// front of a wall into a doorway through it.
float resolved_distance(vec3 world) {
    float d = shape_distance(int(v_kind.x + 0.5), v_p0, v_p1, g_linear * world + g_offset) * g_scale;
    int start = int(v_kind.y + 0.5);
    int count = int(v_kind.w + 0.5);
    for (int i = 0; i < MAX_CUTTERS; ++i) {
        if (i >= count) break;
        int base = (start + i) * 6;
        vec4 r0 = cutter_texel(base + 0);
        vec4 r1 = cutter_texel(base + 1);
        vec4 r2 = cutter_texel(base + 2);
        vec4 ca = cutter_texel(base + 3);
        vec4 cb = cutter_texel(base + 4);
        vec4 ck = cutter_texel(base + 5);   // op, scale, unused, unused
        mat3 cl = mat3(vec3(r0.x, r1.x, r2.x), vec3(r0.y, r1.y, r2.y), vec3(r0.z, r1.z, r2.z));
        float dc = shape_distance(int(ck.x + 0.5), ca, cb, cl * world + vec3(r0.w, r1.w, r2.w)) *
                   max(ck.y, 1e-4);
        d = max(d, -dc);
    }
    return d;
}

vec3 tonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    if (dot(v_world, u_clip.xyz) + u_clip.w > 0.0) discard;

    vec3 direction = normalize(v_world - u_eye);
    // Into the shape's own space, where its distance is written.
    g_linear = mat3(vec3(v_linear[0].x, v_linear[1].x, v_linear[2].x),
                    vec3(v_linear[0].y, v_linear[1].y, v_linear[2].y),
                    vec3(v_linear[0].z, v_linear[1].z, v_linear[2].z));
    g_offset = v_offset;
    g_scale = max(v_kind.z, 1e-4);

    float travel = 0.0;
    // The far side of this shape's own box, so a march can never run past what it is allowed to
    // draw and into somebody else's.
    vec3 inverse = 1.0 / max(abs(direction), vec3(1e-6)) * sign(direction + vec3(1e-9));
    vec3 t1 = (v_low - v_world) * inverse;
    vec3 t2 = (v_high - v_world) * inverse;
    float exit = min(min(max(t1.x, t2.x), max(t1.y, t2.y)), max(t1.z, t2.z));

    bool hit = false;
    vec3 at = v_world;
    for (int i = 0; i < 96; ++i) {
        at = v_world + direction * travel;
        float d = resolved_distance(at);
        if (d < 0.0015) { hit = true; break; }
        travel += max(d, 0.0015);
        if (travel > exit) break;
    }
    if (!hit) discard;

    // The gradient is taken in WORLD space now rather than in the shape's own and rotated back,
    // because the surface under the ray may belong to a cutter rather than to the shape: the wall
    // of a doorway is the doorway's normal, not the wall's.
    vec2 h = vec2(0.002, 0.0);
    vec3 N = normalize(vec3(
        resolved_distance(at + h.xyy) - resolved_distance(at - h.xyy),
        resolved_distance(at + h.yxy) - resolved_distance(at - h.yxy),
        resolved_distance(at + h.yyx) - resolved_distance(at - h.yyx)));
    vec3 V = normalize(u_eye - at);
    if (dot(N, V) < 0.0) N = -N;

    // One colour. Red used to mean "a difference takes this away", and it had a job when a
    // subtrahend was drawn as a solid of its own; now that a hole is a hole there is nothing left
    // for it to say.
    vec3 albedo = vec3(0.62, 0.60, 0.56);

    vec3 ambient = mix(u_skyDown, u_skyUp, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) * 0.5;
    float ndl = max(dot(N, u_sun), 0.0);
    vec3 colour = albedo * (ambient + u_sunColour * ndl * 0.7);
    // A rim, because a construction view is read by its silhouettes.
    colour += albedo * pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.5;

    vec4 clipPos = u_viewProj * vec4(at, 1.0);
    gl_FragDepth = (clipPos.z / clipPos.w) * 0.5 + 0.5;
    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), 1.0);
}`;

function compile(gl, type, source, name) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(name + ': ' + gl.getShaderInfoLog(shader));
    }
    return shader;
}

function link(gl, vertexSource, fragmentSource, name) {
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexSource, name + ' vertex'));
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentSource, name + ' fragment'));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(name + ': ' + gl.getProgramInfoLog(program));
    }
    const uniforms = {};
    const count = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < count; ++i) {
        const info = gl.getActiveUniform(program, i);
        uniforms[info.name] = gl.getUniformLocation(program, info.name);
    }
    return { program, uniforms };
}

// --- a very small amount of matrix arithmetic ------------------------------------------------

export function perspective(out, fovY, aspect, near, far) {
    const f = 1 / Math.tan(fovY / 2);
    out.fill(0);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (far + near) / (near - far);
    out[11] = -1;
    out[14] = (2 * far * near) / (near - far);
    return out;
}

export function lookAt(out, eye, at, up) {
    let zx = eye[0] - at[0], zy = eye[1] - at[1], zz = eye[2] - at[2];
    let length = Math.hypot(zx, zy, zz) || 1;
    zx /= length; zy /= length; zz /= length;
    let xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
    length = Math.hypot(xx, xy, xz) || 1;
    xx /= length; xy /= length; xz /= length;
    const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    out[0] = xx; out[1] = yx; out[2] = zx; out[3] = 0;
    out[4] = xy; out[5] = yy; out[6] = zy; out[7] = 0;
    out[8] = xz; out[9] = yz; out[10] = zz; out[11] = 0;
    out[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
    out[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
    out[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
    out[15] = 1;
    return out;
}

export function multiply(out, a, b) {
    for (let c = 0; c < 4; ++c) {
        const b0 = b[c * 4], b1 = b[c * 4 + 1], b2 = b[c * 4 + 2], b3 = b[c * 4 + 3];
        out[c * 4 + 0] = a[0] * b0 + a[4] * b1 + a[8] * b2 + a[12] * b3;
        out[c * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + a[13] * b3;
        out[c * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
        out[c * 4 + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
    }
    return out;
}

export function invert(out, m) {
    const a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3];
    const a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
    const a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11];
    const a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
    const b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10;
    const b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;
    const b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    const b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;
    const b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31;
    const b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (!det) return out;
    det = 1 / det;
    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
    out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
    out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
    out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
    out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
    out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
    out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
    out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
    out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
    out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
    out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
    out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
    out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
    out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
    out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
    return out;
}

// The six faces, matching bake_web.cpp's order: +X -X +Y -Y +Z -Z.
const FACES = [
    { normal: [1, 0, 0], offset: [1, 0, 0], eu: [0, 1, 0], ev: [0, 0, 1], flip: 0 },
    { normal: [-1, 0, 0], offset: [0, 0, 0], eu: [0, 1, 0], ev: [0, 0, 1], flip: 1 },
    { normal: [0, 1, 0], offset: [0, 1, 0], eu: [0, 0, 1], ev: [1, 0, 0], flip: 0 },
    { normal: [0, -1, 0], offset: [0, 0, 0], eu: [0, 0, 1], ev: [1, 0, 0], flip: 1 },
    { normal: [0, 0, 1], offset: [0, 0, 1], eu: [1, 0, 0], ev: [0, 1, 0], flip: 0 },
    { normal: [0, 0, -1], offset: [0, 0, 0], eu: [1, 0, 0], ev: [0, 1, 0], flip: 1 },
];

export class Renderer {
    constructor(canvas) {
        const gl = canvas.getContext('webgl2', {
            alpha: false,
            antialias: true,
            depth: true,
            stencil: true,
            powerPreference: 'high-performance',
            preserveDrawingBuffer: false,
        });
        if (!gl) throw new Error('this browser has no WebGL 2');
        this.canvas = canvas;
        this.gl = gl;

        this.surface = link(gl, VERTEX_SOURCE, FRAGMENT_SOURCE, 'surface');
        this.sky = link(gl, SKY_VERTEX, SKY_FRAGMENT, 'sky');
        this.cap = link(gl, CAP_VERTEX, CAP_FRAGMENT, 'cap');
        this.shapes = link(gl, SHAPE_VERTEX, SHAPE_FRAGMENT, 'shapes');

        this.vao = gl.createVertexArray();
        this.buffer = gl.createBuffer();
        this.shapeVao = gl.createVertexArray();
        this.shapeBuffer = gl.createBuffer();
        this.shapeCount = 0;
        this.cutters = gl.createTexture();
        this.cutterCount = 0;
        this.cutterWidth = 1;
        this.materials = gl.createTexture();
        this.light = gl.createTexture();
        // >>> matvol
        this.matvol = new Matvol(gl);
        this.matvol.uploadEmpty();
        // <<< matvol
        this.clip = null;

        this.viewProj = new Float32Array(16);
        this.invViewProj = new Float32Array(16);
        this.projection = new Float32Array(16);
        this.view = new Float32Array(16);

        // A sun at fifty degrees in the south-east, which is where the baker cast its shadows
        // from. The two have to agree or a shadow lies on the ground pointing the wrong way.
        this.sun = [0.42, 0.80, -0.43];
        const length = Math.hypot(this.sun[0], this.sun[1], this.sun[2]);
        this.sun = this.sun.map((v) => v / length);
        // Radiance, not a colour picker's idea of one. The sun is three times the sky it stands
        // in, which is the ratio that makes a lit face and a shaded face look like the same stone
        // in two places rather than like two materials — the first attempt had them within a fifth
        // of each other and the whole building read as a paper model.
        this.sunColour = [3.30, 3.10, 2.78];
        this.skyUp = [0.30, 0.47, 0.92];
        this.skyDown = [0.62, 0.67, 0.74];
        this.exposure = 1.0;

        this.stats = { draws: 0, quads: 0 };
    }

    // Everything that lands on the card for one clip: one vertex buffer, one material texture, one
    // light volume. Called again when the clip is rebuilt, so it frees before it allocates.
    setClip(clip) {
        const gl = this.gl;
        this.clip = clip;

        gl.bindVertexArray(this.vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
        const bytes = clip.opaque.byteLength + clip.transparent.byteLength;
        gl.bufferData(gl.ARRAY_BUFFER, bytes, gl.STATIC_DRAW);
        if (clip.opaque.byteLength > 0) gl.bufferSubData(gl.ARRAY_BUFFER, 0, clip.opaque);
        if (clip.transparent.byteLength > 0) {
            gl.bufferSubData(gl.ARRAY_BUFFER, clip.opaque.byteLength, clip.transparent);
        }
        this.transparentBase = clip.opaque.byteLength;
        for (let i = 0; i < 4; ++i) {
            gl.enableVertexAttribArray(i);
            gl.vertexAttribDivisor(i, 1);
        }
        gl.bindVertexArray(null);

        // The colour the cut face is painted, and it is the clip's own rather than a constant.
        //
        // The commonest opaque material BY AREA -- a quad is a merged rectangle, so counting quads
        // would let a thousand little trim pieces outvote a wall. Area is what the eye sees and it
        // is what the cap stands in for. It is ONE colour for the whole clip because the file has
        // no material volume to ask; the note at the top of this file says what that costs.
        {
            const area = new Map();
            const q = new DataView(clip.opaque.buffer, clip.opaque.byteOffset,
                                   clip.opaque.byteLength);
            for (let i = 0; i < clip.opaqueQuads; ++i) {
                const at = i * 16;
                const w = q.getUint16(at + 6, true) + 1;
                const h = q.getUint16(at + 8, true) + 1;
                const m = q.getUint16(at + 10, true);
                area.set(m, (area.get(m) || 0) + w * h);
            }
            let best = -1, most = -1;
            for (const [m, a] of area) if (a > most) { most = a; best = m; }
            // sRGB in the file, linear in the shader, like every other colour here.
            this.capColour = (best >= 0 && best < clip.materialCount)
                ? [0, 1, 2].map(k => Math.pow(clip.materials[best * 16 + k] / 255, 2.2))
                : [0.36, 0.34, 0.32];
        }

        // The clip as it was written. SHAPE_BYTES a shape, straight into a buffer.
        this.shapeCount = clip.shapeCount;
        if (this.shapeCount > 0) {
            gl.bindVertexArray(this.shapeVao);
            gl.bindBuffer(gl.ARRAY_BUFFER, this.shapeBuffer);
            gl.bufferData(gl.ARRAY_BUFFER, clip.shapes, gl.STATIC_DRAW);
            // op 0..3, cut_start 4..7, scale 8..11, placement 12..59, parameters 60..91,
            // box 92..115, cut_count 116..119. Written by the loop at the end of bake_root.
            const stride = SHAPE_BYTES;
            const layout = [
                [0, 4, 12], [1, 4, 28], [2, 4, 44],   // the 3x4 placement, a row to an attribute
                [3, 4, 60], [4, 4, 76],               // eight parameters
                [6, 3, 92], [7, 3, 104],              // the box it lands in
            ];
            // The head is op, cut_start and cut_count (uints) with scale (a float) among them; the
            // shader wants all four as floats, so they are converted here rather than
            // reinterpreted there.
            const head = new Float32Array(this.shapeCount * 4);
            const view = new DataView(clip.shapes.buffer, clip.shapes.byteOffset,
                                      clip.shapes.byteLength);
            for (let i = 0; i < this.shapeCount; ++i) {
                head[i * 4 + 0] = view.getUint32(i * stride, true);
                head[i * 4 + 1] = view.getUint32(i * stride + 4, true);
                head[i * 4 + 2] = view.getFloat32(i * stride + 8, true);
                head[i * 4 + 3] = view.getUint32(i * stride + 116, true);
            }
            this.shapeHead = this.shapeHead || gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, this.shapeHead);
            gl.bufferData(gl.ARRAY_BUFFER, head, gl.STATIC_DRAW);
            gl.enableVertexAttribArray(5);
            gl.vertexAttribPointer(5, 4, gl.FLOAT, false, 16, 0);
            gl.vertexAttribDivisor(5, 1);

            gl.bindBuffer(gl.ARRAY_BUFFER, this.shapeBuffer);
            for (const [index, size, offset] of layout) {
                gl.enableVertexAttribArray(index);
                gl.vertexAttribPointer(index, size, gl.FLOAT, false, stride, offset);
                gl.vertexAttribDivisor(index, 1);
            }
            gl.bindVertexArray(null);
        }

        // The cutter pool. WebGL2 has no storage buffer, so it is an RGBA32F texture read with
        // texelFetch — six texels a cutter, and a shape's range into it comes down the instanced
        // attributes. There is always a texture, even a 1x1 one, because a sampler bound to
        // nothing is undefined behaviour and not a blank hole.
        this.cutterCount = clip.cutterCount || 0;
        {
            const texels = Math.max(1, this.cutterCount * CUTTER_TEXELS);
            // A multiple of CUTTER_TEXELS so a cutter never straddles two rows, which keeps the
            // index arithmetic in the shader to one divide and one modulo.
            this.cutterWidth = Math.min(1020, texels);
            const height = Math.ceil(texels / this.cutterWidth);
            const padded = new Float32Array(this.cutterWidth * height * 4);
            if (this.cutterCount > 0) {
                // .slice() rather than a view: the pool starts wherever the blocks before it
                // ended, which is not a multiple of four bytes, and Float32Array wants alignment.
                padded.set(new Float32Array(clip.cutters.slice().buffer));
            }
            gl.bindTexture(gl.TEXTURE_2D, this.cutters);
            gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, this.cutterWidth, height, 0, gl.RGBA,
                          gl.FLOAT, padded);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        }

        // Materials, verbatim: each VisualRecord is four RGBA8 texels and the shader fetches the
        // row it wants. Nothing is converted on the way in, which is the point — the browser reads
        // the same sixteen bytes the world stores.
        const texels = clip.materialCount * 4;
        const width = 256;
        const height = Math.max(1, Math.ceil(texels / width));
        const padded = new Uint8Array(width * height * 4);
        padded.set(clip.materials.subarray(0, Math.min(clip.materials.length, padded.length)));
        gl.bindTexture(gl.TEXTURE_2D, this.materials);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                      padded);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        gl.bindTexture(gl.TEXTURE_3D, this.light);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RG8, clip.lightDims[0], clip.lightDims[1],
                      clip.lightDims[2], 0, gl.RG, gl.UNSIGNED_BYTE, clip.light);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);

        // >>> matvol
        // What the matter inside the clip is, and how thick it is. Straight from the file to three
        // textures -- see web/js/features/matvol.js. A clip baked before this existed has no MVOL
        // chunk and gets an empty volume, which answers "no matter" everywhere and leaves the cap
        // on its one colour.
        this.matvol.upload(clip);
        // <<< matvol
    }

    attributesAt(byteOffset) {
        const gl = this.gl;
        gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
        gl.vertexAttribPointer(0, 3, gl.UNSIGNED_SHORT, false, 16, byteOffset + 0);
        gl.vertexAttribPointer(1, 2, gl.UNSIGNED_SHORT, false, 16, byteOffset + 6);
        gl.vertexAttribPointer(2, 1, gl.UNSIGNED_SHORT, false, 16, byteOffset + 10);
        gl.vertexAttribPointer(3, 1, gl.UNSIGNED_BYTE, false, 16, byteOffset + 12);
    }

    // One pass over the mesh: six ranges, one per face, each with its own normal and basis.
    drawFaces(uniforms, starts, base, blended) {
        const gl = this.gl;
        for (let face = 0; face < 6; ++face) {
            const count = starts[face + 1] - starts[face];
            if (count === 0) continue;
            const f = FACES[face];
            gl.uniform3fv(uniforms.u_normal, f.normal);
            gl.uniform3fv(uniforms.u_offset, f.offset);
            gl.uniform3fv(uniforms.u_eu, f.eu);
            gl.uniform3fv(uniforms.u_ev, f.ev);
            gl.uniform1f(uniforms.u_flip, f.flip);
            if (uniforms.u_blended) gl.uniform1f(uniforms.u_blended, blended);
            this.attributesAt(base + starts[face] * 16);
            gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);
            this.stats.draws += 1;
            this.stats.quads += count;
        }
    }

    setShared(uniforms, camera) {
        const gl = this.gl;
        const clip = this.clip;
        gl.uniformMatrix4fv(uniforms.u_viewProj, false, this.viewProj);
        gl.uniform3fv(uniforms.u_origin, clip.origin);
        gl.uniform1f(uniforms.u_scale, 1 / clip.metre);
        gl.uniform3fv(uniforms.u_eye, camera.eye);
        gl.uniform3fv(uniforms.u_sun, this.sun);
        gl.uniform3fv(uniforms.u_sunColour, this.sunColour);
        gl.uniform3fv(uniforms.u_skyUp, this.skyUp);
        gl.uniform3fv(uniforms.u_skyDown, this.skyDown);
        gl.uniform1f(uniforms.u_exposure, this.exposure);

        const size = [
            clip.lightDims[0] * clip.lightCell,
            clip.lightDims[1] * clip.lightCell,
            clip.lightDims[2] * clip.lightCell,
        ];
        gl.uniform3fv(uniforms.u_lightOrigin, clip.origin);
        gl.uniform3fv(uniforms.u_lightScale, [1 / size[0], 1 / size[1], 1 / size[2]]);
        gl.uniform3fv(uniforms.u_lightTexel, [
            0.5 / clip.lightDims[0], 0.5 / clip.lightDims[1], 0.5 / clip.lightDims[2],
        ]);
        gl.uniform1f(uniforms.u_lightBias, clip.lightCell);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.materials);
        gl.uniform1i(uniforms.u_materials, 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_3D, this.light);
        gl.uniform1i(uniforms.u_light, 1);
    }

    // The clip as it was written, instead of as it came out. One instanced box per shape, each
    // sphere-traced in its own space, so nothing here has a resolution.
    drawShapes(camera, plane) {
        const gl = this.gl;
        if (!this.shapeCount) return;
        gl.bindVertexArray(this.shapeVao);
        gl.useProgram(this.shapes.program);
        const u = this.shapes.uniforms;
        gl.uniformMatrix4fv(u.u_viewProj, false, this.viewProj);
        gl.uniform3fv(u.u_eye, camera.eye);
        gl.uniform3fv(u.u_sun, this.sun);
        gl.uniform3fv(u.u_sunColour, this.sunColour);
        gl.uniform3fv(u.u_skyUp, this.skyUp);
        gl.uniform3fv(u.u_skyDown, this.skyDown);
        gl.uniform1f(u.u_exposure, this.exposure);
        gl.uniform4fv(u.u_clip, plane);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.cutters);
        gl.uniform1i(u.u_cutters, 0);
        gl.uniform1i(u.u_cutterWidth, this.cutterWidth);

        gl.enable(gl.DEPTH_TEST);
        gl.depthMask(true);
        // No culling: the eye is inside a shape's own box as often as not, and a box whose front
        // faces are gone is a shape that vanishes when you walk up to it.
        gl.disable(gl.CULL_FACE);
        // And no blending: every shape is opaque now. The half-transparent red was what a
        // subtrahend drawn as a solid of its own needed to not hide the stone behind it, and there
        // are no subtrahends drawn as solids any more.
        gl.disable(gl.BLEND);
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 14, this.shapeCount);
        gl.bindVertexArray(null);
        this.stats.draws += 1;
    }

    // `slice` is null, or { axis: 0..2, sign: +1 | -1, at: metres }. The plane keeps everything on
    // the side the sign points away from, so dragging the slider walks the cut through the clip.
    render(camera, slice, showShapes) {
        const gl = this.gl;
        const clip = this.clip;
        this.stats.draws = 0;
        this.stats.quads = 0;
        if (!clip) return;

        const width = this.canvas.width;
        const height = this.canvas.height;
        gl.viewport(0, 0, width, height);

        // `fovFor` and not `fov`: the vertical angle is derived from the shape of the window so
        // that the horizontal one clears a floor. On a phone held upright that is the difference
        // between seeing a room and seeing a letterbox. See Controls.fovFor.
        const aspect = width / Math.max(1, height);
        perspective(this.projection, camera.fovFor ? camera.fovFor(aspect) : camera.fov, aspect,
                    camera.near, camera.far);
        lookAt(this.view, camera.eye, camera.at, [0, 1, 0]);
        multiply(this.viewProj, this.projection, this.view);
        invert(this.invViewProj, this.viewProj);

        gl.clearColor(0, 0, 0, 1);
        gl.clearDepth(1);
        gl.clearStencil(0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);

        // --- sky -------------------------------------------------------------------------------
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);
        gl.disable(gl.BLEND);
        gl.disable(gl.STENCIL_TEST);
        gl.useProgram(this.sky.program);
        gl.uniformMatrix4fv(this.sky.uniforms.u_invViewProj, false, this.invViewProj);
        gl.uniform3fv(this.sky.uniforms.u_eye, camera.eye);
        gl.uniform3fv(this.sky.uniforms.u_sun, this.sun);
        gl.uniform3fv(this.sky.uniforms.u_sunColour, this.sunColour);
        gl.uniform3fv(this.sky.uniforms.u_skyUp, this.skyUp);
        gl.uniform3fv(this.sky.uniforms.u_skyDown, this.skyDown);
        gl.uniform1f(this.sky.uniforms.u_exposure, this.exposure);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        // dot(world, xyz) + w > 0 is cut away. With no slice the plane is put where nothing can
        // reach it, so the same shader runs either way and there is no branch to get wrong.
        const plane = [0, 0, 0, -1];
        if (slice) {
            plane[slice.axis] = slice.sign;
            plane[3] = -slice.sign * slice.at;
        }

        if (showShapes) {
            this.drawShapes(camera, plane);
            return;
        }

        gl.bindVertexArray(this.vao);
        gl.useProgram(this.surface.program);
        this.setShared(this.surface.uniforms, camera);
        gl.uniform4fv(this.surface.uniforms.u_clip, plane);
        gl.uniform1f(this.surface.uniforms.u_cutSide, 0);

        // --- opaque ----------------------------------------------------------------------------
        gl.enable(gl.DEPTH_TEST);
        gl.depthFunc(gl.LESS);
        gl.depthMask(true);
        gl.frontFace(gl.CCW);
        gl.cullFace(gl.BACK);
        // Back faces are what the inside of a clip IS. Unsliced, nothing can see one and culling
        // them is free; sliced, every surface the cut opens up is seen from behind, and culling
        // them is the difference between looking into a room and looking through the building.
        if (slice) {
            gl.disable(gl.CULL_FACE);
        } else {
            gl.enable(gl.CULL_FACE);
        }
        this.drawFaces(this.surface.uniforms, clip.opaqueFace, 0, 0);

        // --- the cut face ----------------------------------------------------------------------
        //
        // Two passes: parity, then one quad through it. `u_cutSide` flips the surface shader to the
        // DISCARDED side, so the stencil counts the faces the slice threw away -- entering matter
        // and leaving it -- and lands on odd wherever the plane is inside stone.
        //
        // WHICH SIDE THE EYE IS ON DECIDES WHETHER THERE IS A CAP AT ALL, and the sign of that
        // test is the whole of what makes parity work. The count has to be of the surfaces BETWEEN
        // THE EYE AND THE PLANE, and those are exactly the ones the slice threw away -- which is
        // why the parity pass draws the discarded side and why it only means anything when the eye
        // is over there looking in. A ray from such an eye enters matter and does not leave it
        // again before the plane precisely when the plane is inside stone, so odd parity IS the
        // cut face.
        //
        // From the other side there is nothing between the eye and the plane to count, the parity
        // comes out zero everywhere, and there is no cap -- correctly, because from there the plane
        // is behind the matter that was kept and a cap would be hidden by it anyway.
        //
        // Getting this backwards costs nothing visible except that no cap ever appears, which is
        // indistinguishable from not having written one. It was backwards here first.
        const eyeSide = slice
            ? (camera.eye[0] * plane[0] + camera.eye[1] * plane[1] + camera.eye[2] * plane[2] +
               plane[3])
            : -1;
        if (slice && eyeSide > 0 && clip.opaqueQuads > 0) {
            gl.enable(gl.STENCIL_TEST);
            gl.stencilMask(0xFF);
            gl.stencilFunc(gl.ALWAYS, 0, 0xFF);
            gl.stencilOp(gl.KEEP, gl.INVERT, gl.INVERT);
            gl.colorMask(false, false, false, false);
            gl.depthMask(false);
            gl.disable(gl.DEPTH_TEST);
            gl.disable(gl.CULL_FACE);
            gl.uniform1f(this.surface.uniforms.u_cutSide, 1);
            this.drawFaces(this.surface.uniforms, clip.opaqueFace, 0, 0);
            gl.uniform1f(this.surface.uniforms.u_cutSide, 0);
            gl.colorMask(true, true, true, true);

            // ...and the quad, only where the parity came out odd.
            gl.stencilFunc(gl.EQUAL, 1, 1);
            gl.stencilOp(gl.KEEP, gl.KEEP, gl.KEEP);
            gl.enable(gl.DEPTH_TEST);
            gl.depthMask(true);
            gl.useProgram(this.cap.program);
            const u = this.cap.uniforms;
            const a = slice.axis;
            const b = (a + 1) % 3, c = (a + 2) % 3;
            const lo = [clip.origin[0], clip.origin[1], clip.origin[2]];
            const span = [clip.dims[0] / clip.metre, clip.dims[1] / clip.metre,
                          clip.dims[2] / clip.metre];
            const originV = [lo[0], lo[1], lo[2]];
            originV[a] = slice.at;
            const spanA = [0, 0, 0]; spanA[b] = span[b];
            const spanB = [0, 0, 0]; spanB[c] = span[c];
            // Facing the eye, which is the side the slice opened. Pointed the other way -- into
            // the stone that was kept -- the sun term is zero, the sky mix reads the ground, and
            // the cap comes out black. It did.
            const normal = [0, 0, 0]; normal[a] = slice.sign;
            gl.uniformMatrix4fv(u.u_viewProj, false, this.viewProj);
            gl.uniform3fv(u.u_capOrigin, originV);
            gl.uniform3fv(u.u_capU, spanA);
            gl.uniform3fv(u.u_capV, spanB);
            gl.uniform3fv(u.u_capColour, this.capColour);
            gl.uniform3fv(u.u_capNormal, normal);
            gl.uniform1f(u.u_capVoxel, 1 / clip.metre);
            gl.uniform3fv(u.u_sun, this.sun);
            gl.uniform3fv(u.u_sunColour, this.sunColour);
            gl.uniform3fv(u.u_skyUp, this.skyUp);
            gl.uniform3fv(u.u_skyDown, this.skyDown);
            gl.uniform1f(u.u_exposure, this.exposure);
            const size = [clip.lightDims[0] * clip.lightCell, clip.lightDims[1] * clip.lightCell,
                          clip.lightDims[2] * clip.lightCell];
            gl.uniform3fv(u.u_lightOrigin, clip.origin);
            gl.uniform3fv(u.u_lightScale, [1 / size[0], 1 / size[1], 1 / size[2]]);
            gl.uniform3fv(u.u_lightTexel, [0.5 / clip.lightDims[0], 0.5 / clip.lightDims[1],
                                           0.5 / clip.lightDims[2]]);
            gl.activeTexture(gl.TEXTURE1);
            gl.bindTexture(gl.TEXTURE_3D, this.light);
            gl.uniform1i(u.u_light, 1);
            // >>> matvol
            // The material table, so the cap can paint the stone the volume names, and the volume
            // itself on units 2, 3 and 4.
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, this.materials);
            gl.uniform1i(u.u_materials, 0);
            this.matvol.bind(u, 2);
            // <<< matvol
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
            gl.disable(gl.STENCIL_TEST);
            this.stats.draws += 1;

            gl.bindVertexArray(this.vao);
            gl.useProgram(this.surface.program);
        }

        // The cut shows the mesh's own faces as well as the cap.
        //
        // It used to fill the cross-section with a flat grey quad, found with a stencil parity
        // pass, so that a sliced wall read as solid stone rather than as a sheet of paper. That is
        // a defensible picture of a building and it is the wrong one HERE: it paints over exactly
        // the faces somebody dragged the slider to see, and what you get is a clean grey plane
        // where the inside of the clip should be. Reported as "make the slicing show the inside of
        // the clip, not culling the faces of sliced voxels", which is the whole of it.
        //
        // So the cut shows the mesh's own faces and the only thing that changes is that the pass
        // above stops culling: see `render`'s cull decision. A wall cut through shows its two
        // surfaces and the gap between them, because that is what the voxels are.

        // --- glass -----------------------------------------------------------------------------
        if (clip.transparentQuads > 0) {
            gl.enable(gl.BLEND);
            gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE,
                                 gl.ONE_MINUS_SRC_ALPHA);
            gl.depthMask(false);
            if (slice) {
                gl.disable(gl.CULL_FACE);
            } else {
                gl.enable(gl.CULL_FACE);
            }
            this.drawFaces(this.surface.uniforms, clip.transparentFace, this.transparentBase, 1);
            gl.depthMask(true);
            gl.disable(gl.BLEND);
        }

        gl.bindVertexArray(null);
    }
}
