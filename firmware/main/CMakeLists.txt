idf_component_register(
    SRCS
        "main.c"
        "clips.c"
        "face.c"
        "behavior.c"
        "touch.c"
        "motion.c"
        "audio.c"
        "sounds.c"
    INCLUDE_DIRS "."
)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

# Pack the body frames and face anchors from ../../assets into one binary
# table at build time, then link it into the app as
# _binary_muse_frames_bin_start / _end.
set(MUSE_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(MUSE_ASSETS_DIR "${MUSE_REPO_DIR}/assets")
set(MUSE_PACKER "${MUSE_REPO_DIR}/tools/pack_frames.py")
set(MUSE_FRAMES_BIN "${CMAKE_CURRENT_BINARY_DIR}/muse_frames.bin")

file(GLOB MUSE_FRAME_JPEGS CONFIGURE_DEPENDS "${MUSE_ASSETS_DIR}/frames/*.jpg")
if(NOT MUSE_FRAME_JPEGS)
    message(FATAL_ERROR "No frames found in ${MUSE_ASSETS_DIR}/frames")
endif()

idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT "${MUSE_FRAMES_BIN}"
    COMMAND ${python} "${MUSE_PACKER}" "${MUSE_ASSETS_DIR}/frames" "${MUSE_ASSETS_DIR}/anchors.json" "${MUSE_FRAMES_BIN}"
    DEPENDS ${MUSE_FRAME_JPEGS} "${MUSE_ASSETS_DIR}/anchors.json" "${MUSE_PACKER}"
    COMMENT "Packing Muse frames and face anchors"
    VERBATIM
)
add_custom_target(muse_frames DEPENDS "${MUSE_FRAMES_BIN}")
target_add_binary_data(${COMPONENT_LIB} "${MUSE_FRAMES_BIN}" BINARY DEPENDS muse_frames)
