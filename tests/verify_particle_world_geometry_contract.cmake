if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()

set(VANILLA_WORLD_RGEN
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/world.rgen")
set(ADVANCED_WORLD_RGEN
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/world.rgen")
set(ADVANCED_PRIMARY_TRACE
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/primary_trace.glsl")

foreach(SHADER_PATH IN ITEMS
        "${VANILLA_WORLD_RGEN}"
        "${ADVANCED_WORLD_RGEN}"
        "${ADVANCED_PRIMARY_TRACE}")
    if(NOT EXISTS "${SHADER_PATH}")
        message(FATAL_ERROR "Missing particle world-geometry shader: ${SHADER_PATH}")
    endif()

    file(READ "${SHADER_PATH}" SHADER_TEXT)

    foreach(FORBIDDEN_MASK IN ITEMS
            "WORLD_MASK | PLAYER_MASK | PRIORITY_MASK | BOAT_WATER_MASK"
            "WORLD_MASK | PRIORITY_MASK | BOAT_WATER_MASK")
        string(FIND "${SHADER_TEXT}" "${FORBIDDEN_MASK}" FORBIDDEN_INDEX)
        if(NOT FORBIDDEN_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Particle geometry is omitted from a continued camera ray in ${SHADER_PATH}: ${FORBIDDEN_MASK}")
        endif()
    endforeach()
endforeach()

foreach(WORLD_RGEN IN ITEMS "${VANILLA_WORLD_RGEN}" "${ADVANCED_WORLD_RGEN}")
    file(READ "${WORLD_RGEN}" WORLD_RGEN_TEXT)

    string(REGEX MATCH
        "mask = WORLD_MASK \\| PLAYER_MASK[^\r\n]*\\| PARTICLE_MASK;"
        DEEP_SECONDARY_MASK
        "${WORLD_RGEN_TEXT}")
    if(DEEP_SECONDARY_MASK STREQUAL "")
        message(FATAL_ERROR
            "Deep secondary rays must retain PARTICLE_MASK in ${WORLD_RGEN}")
    endif()

    string(FIND "${WORLD_RGEN_TEXT}"
        "mask = WORLD_MASK | PLAYER_MASK | PARTICLE_MASK | CLOUD_MASK;"
        SHARC_MASK_INDEX)
    if(SHARC_MASK_INDEX EQUAL -1)
        message(FATAL_ERROR
            "SHARC update rays must retain PARTICLE_MASK in ${WORLD_RGEN}")
    endif()
endforeach()

message(STATUS "Particle world-geometry ray-mask contract verified")
