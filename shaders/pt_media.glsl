// Light that is scattered on the way rather than at the end of the journey.
//
// Everything in this renderer assumes light travels through a vacuum between surfaces. It does
// not: a shaft of sun through a high window is visible *in the air*, and that image -- the beam
// coming down through the oculus of the rotunda onto the floor -- is the single picture the
// test facility exists to produce. Without media it is a bright patch on the floor and nothing
// in between.
//
// Rules for whoever implements it:
//   - it costs a march it cannot have. The way in is the same one transmission took: march once
//     inside a loop, or sample the medium along a ray that is already being traced rather than
//     tracing a new one.
//   - it must be per voxel face wherever it caches anything, like everything else here.
//   - a scene with no medium in it must pay nothing. The common case is clear air.

vec3 apply_media(vec3 radiance, vec3 origin, vec3 dir, float distance) {
    // Placeholder: clear air, which is what the renderer has always assumed.
    return radiance;
}