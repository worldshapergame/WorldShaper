# Copy the clip SOURCES beside the executable, and nothing else.
#
# This exists because `cmake -E copy_directory` has no way to exclude anything, and clips/ does not
# contain only sources: building a clip leaves a `.clip.world` next to it, and the facility's is well
# over half a gigabyte.
#
# Copying those too meant every build restored a stale cache into the directory the game actually
# loads from. Deleting the cache in the source tree changed nothing, because the next build put an
# old one back — so a clip fix could be correct, committed and invisible, which is exactly what
# happened with the sub-voxel displacement guard. The cache is build output; it does not travel.
#
# Run as a script at BUILD time rather than expanded at configure time, because which caches exist
# depends on what has been run since, and a glob evaluated at configure time would not know.

file(GLOB_RECURSE clip_files RELATIVE "${SRC}" "${SRC}/*")

foreach(entry IN LISTS clip_files)
    # The built world and its sidecar are the two things a clip build leaves behind.
    if(entry MATCHES "\\.clip\\.world$" OR entry MATCHES "\\.clip\\.load$")
        continue()
    endif()
    configure_file("${SRC}/${entry}" "${DST}/${entry}" COPYONLY)
endforeach()

# A cache already sitting beside the executable is LEFT ALONE, and the first version of this file
# deleted it — which was a one-time cleanup written as if it ran once, when it runs on every build.
#
# The effect was that every rebuild wiped the runtime cache and the next launch resampled the
# facility from cold. Reported as having to sit through a cold build again for no visible reason,
# and that was the reason.
#
# Not copying the caches in is sufficient on its own: a world built beside the executable belongs to
# the executable and is keyed on the clip and the compiler, so it invalidates itself when it should.
