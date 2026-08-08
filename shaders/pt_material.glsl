// What a voxel is made of, and how it answers light.
//
// Owned as its own module so that the response of a surface can be worked on without touching
// the transport that asks it. Everything here is a pure function of a voxel type: nothing in
// this file knows where the ray came from or where it is going.

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
};

Material material_of(uint type_id) {
    Material m;
    m.albedo = vec3(0.5);
    m.roughness = 0.5;
    m.metallic = 0.0;
    m.emission = vec3(0.0);
    m.opacity = 1.0;
    m.index = 1.0;
    m.absorb = vec3(0.0);

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
    // metallic, ior, emissive, then absorption and translucency, then the emissive tint.
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
    return m;
}
