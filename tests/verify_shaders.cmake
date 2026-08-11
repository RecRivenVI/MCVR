file(GLOB_RECURSE SPIRV_FILES "${MCVR_SHADER_DIR}/*.spv")
if(NOT SPIRV_FILES)
    message(FATAL_ERROR "No compiled SPIR-V shaders found under ${MCVR_SHADER_DIR}")
endif()

foreach(SPIRV_FILE IN LISTS SPIRV_FILES)
    file(SIZE "${SPIRV_FILE}" SPIRV_SIZE)
    if(SPIRV_SIZE EQUAL 0)
        message(FATAL_ERROR "Compiled shader is empty: ${SPIRV_FILE}")
    endif()
endforeach()

foreach(OBSOLETE_SPIRV IN ITEMS
    "${MCVR_SHADER_DIR}/world/post_render/world_post_star_frag.spv"
    "${MCVR_SHADER_DIR}/world/post_render/world_post_star_vert.spv"
    "${MCVR_SHADER_DIR}/world/post_render/world_post_frag.spv"
    "${MCVR_SHADER_DIR}/world/post_render/world_post_vert.spv"
    "${MCVR_SHADER_DIR}/world/post_render/world_post_text_frag.spv"
    "${MCVR_SHADER_DIR}/world/post_render/world_post_text_vert.spv"
    "${MCVR_SHADER_DIR}/world/post_render/light_map_frag.spv"
    "${MCVR_SHADER_DIR}/world/post_render/light_map_vert.spv"
)
    if(EXISTS "${OBSOLETE_SPIRV}")
        message(FATAL_ERROR "Obsolete non-fullscreen post-render shader survived cleanup: ${OBSOLETE_SPIRV}")
    endif()
endforeach()

foreach(PACK_NAME IN ITEMS vanilla-pt advanced)
    set(PACK_ARCHIVE "${MCVR_INSTALL_DIR}/shaders/world/ray_tracing/${PACK_NAME}.zip")
    if(NOT EXISTS "${PACK_ARCHIVE}")
        message(FATAL_ERROR "Missing built-in shader pack: ${PACK_ARCHIVE}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar tf "${PACK_ARCHIVE}"
        RESULT_VARIABLE PACK_LIST_RESULT
        OUTPUT_VARIABLE PACK_CONTENTS
        ERROR_VARIABLE PACK_LIST_ERROR
    )
    if(NOT PACK_LIST_RESULT EQUAL 0)
        message(FATAL_ERROR "Cannot inspect ${PACK_ARCHIVE}: ${PACK_LIST_ERROR}")
    endif()
    foreach(REQUIRED_ENTRY IN ITEMS
        "priority/priority.rgen"
        "priority/outline.rchit"
        "priority/text.rchit"
        "priority/composite.comp"
    )
        string(FIND "${PACK_CONTENTS}" "${REQUIRED_ENTRY}" ENTRY_INDEX)
        if(ENTRY_INDEX EQUAL -1)
            message(FATAL_ERROR "${PACK_NAME} is missing priority geometry shader: ${REQUIRED_ENTRY}")
        endif()
    endforeach()
    if(PACK_CONTENTS MATCHES "post_render/(render_(star|text|outline)|post_outline)")
        message(FATAL_ERROR "${PACK_NAME} still packages a legacy non-fullscreen post-render shader")
    endif()
endforeach()
