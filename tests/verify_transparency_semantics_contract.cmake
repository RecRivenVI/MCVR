if(NOT DEFINED MCVR_SOURCE_DIR OR NOT DEFINED JAVA_PROJECT_ROOT_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR and JAVA_PROJECT_ROOT_DIR are required")
endif()

get_filename_component(RADIANCE_SOURCE_DIR "${JAVA_PROJECT_ROOT_DIR}" ABSOLUTE)

set(PBR_CONSUMER
    "${RADIANCE_SOURCE_DIR}/src/main/java/com/radiance/client/vertex/PBRVertexConsumer.java")
file(READ "${PBR_CONSUMER}" PBR_CONSUMER_TEXT)
foreach(REQUIRED_TEXT IN ITEMS
    "ALPHA_MODE_TRANSMISSION = 2"
    "ALPHA_MODE_COVERAGE = 10"
    "ALPHA_MODE_ADDITIVE = 11"
    "multiPhase.name.equals(\"translucent\")"
    "multiPhase.name.equals(\"translucent_moving_block\")"
    "RenderStateShard.ADDITIVE_TRANSPARENCY.equals"
    "return ALPHA_MODE_COVERAGE"
)
    string(FIND "${PBR_CONSUMER_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing Java transparency classification contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

set(ALPHA_MODE_SHADER "${MCVR_SOURCE_DIR}/src/shader/util/alpha_mode.glsl")
file(READ "${ALPHA_MODE_SHADER}" ALPHA_MODE_TEXT)
foreach(REQUIRED_TEXT IN ITEMS
    "ALPHA_MODE_TRANSMISSION = 2u"
    "ALPHA_MODE_COVERAGE = 10u"
    "ALPHA_MODE_ADDITIVE = 11u"
    "isTransmissionAlphaMode"
    "isCoverageAlphaMode"
    "isAdditiveAlphaMode"
)
    string(FIND "${ALPHA_MODE_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing shader alpha-mode contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/shader/util/labpbr.glsl" LABPBR_TEXT)
string(FIND "${LABPBR_TEXT}" "allowAlphaTransmission && texAlbedo.a" EXPLICIT_TRANSMISSION_INDEX)
if(EXPLICIT_TRANSMISSION_INDEX EQUAL -1)
    message(FATAL_ERROR "LabPBR still infers transmission without an explicit material semantic")
endif()

foreach(ANY_HIT IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/default.rahit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/default.rahit"
)
    file(READ "${ANY_HIT}" ANY_HIT_TEXT)
    foreach(REQUIRED_TEXT IN ITEMS
        "isCoverageAlphaMode(alphaMode)"
        "rand(mainRay.seed) >= alpha"
        "isAdditiveAlphaMode(alphaMode)"
        "ignoreIntersectionEXT"
    )
        string(FIND "${ANY_HIT_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR "Missing stochastic coverage contract '${REQUIRED_TEXT}': ${ANY_HIT}")
        endif()
    endforeach()
endforeach()

foreach(SURFACE_SHADER IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/default.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/no_height.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/world_no_reflect.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/surface_eval.glsl"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/world_no_reflect.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/default.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/no_height.rchit"
)
    file(READ "${SURFACE_SHADER}" SURFACE_TEXT)
    string(FIND "${SURFACE_TEXT}" "isTransmissionAlphaMode(alphaMode)" TRANSMISSION_INDEX)
    string(FIND "${SURFACE_TEXT}" "isCoverageAlphaMode(alphaMode)" COVERAGE_INDEX)
    if(TRANSMISSION_INDEX EQUAL -1 OR COVERAGE_INDEX EQUAL -1)
        message(FATAL_ERROR "Surface shader does not separate coverage from transmission: ${SURFACE_SHADER}")
    endif()
endforeach()

foreach(SHADOW_SHADER IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/shadow.rahit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/shadow.rahit"
)
    file(READ "${SHADOW_SHADER}" SHADOW_TEXT)
    foreach(REQUIRED_TEXT IN ITEMS
        "isAdditiveAlphaMode(alphaMode)"
        "isCoverageAlphaMode(alphaMode)"
        "shadowRay.throughput *= vec3(1.0 - alpha)"
        "isTransmissionAlphaMode(alphaMode)"
    )
        string(FIND "${SHADOW_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR "Missing transparent-shadow contract '${REQUIRED_TEXT}': ${SHADOW_SHADER}")
        endif()
    endforeach()
endforeach()

foreach(TRANSPARENT_ONLY IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/transparent_only.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/transparent_only.rchit"
)
    file(READ "${TRANSPARENT_ONLY}" TRANSPARENT_ONLY_TEXT)
    # The explicit Flywheel blend branches replaced the former single predicate.
    # Check the actual additive and source-over bodies, including no attenuation
    # for additive/glint/lightning and multiplicative crumbling.
    foreach(MODE IN ITEMS ADDITIVE FLYWHEEL_LIGHTNING FLYWHEEL_GLINT FLYWHEEL_CRUMBLING FLYWHEEL_TRANSLUCENT)
        string(REGEX MATCH "if \\(alphaMode == ALPHA_MODE_${MODE}\\) \\{([^}]*)\\}" MODE_BLOCK "${TRANSPARENT_ONLY_TEXT}")
        if(NOT MODE_BLOCK)
            message(FATAL_ERROR "Missing explicit ${MODE} blend branch: ${TRANSPARENT_ONLY}")
        endif()
        if(MODE STREQUAL "FLYWHEEL_TRANSLUCENT")
            string(FIND "${MODE_BLOCK}" "mainRay.throughput *= vec3(1.0 - alpha)" ATTENUATION)
            if(ATTENUATION EQUAL -1)
                message(FATAL_ERROR "Source-over must attenuate throughput: ${TRANSPARENT_ONLY}")
            endif()
        elseif(MODE STREQUAL "FLYWHEEL_CRUMBLING")
            string(FIND "${MODE_BLOCK}" "mainRay.throughput *= 2.0 * shadedRgb" ATTENUATION)
            if(ATTENUATION EQUAL -1)
                message(FATAL_ERROR "Crumbling must preserve multiplicative blend: ${TRANSPARENT_ONLY}")
            endif()
        else()
            string(FIND "${MODE_BLOCK}" "mainRay.throughput *=" ATTENUATION)
            string(FIND "${MODE_BLOCK}" "mainRay.radiance +=" CONTRIBUTION)
            if(NOT ATTENUATION EQUAL -1 OR CONTRIBUTION EQUAL -1)
                message(FATAL_ERROR "${MODE} must add radiance without attenuating throughput: ${TRANSPARENT_ONLY}")
            endif()
        endif()
    endforeach()
endforeach()

set(PACK_CONFIGS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/configs.json"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/configs.json"
)
foreach(CONFIG_PATH IN LISTS PACK_CONFIGS)
    file(READ "${CONFIG_PATH}" CONFIG_TEXT)
    foreach(GROUP_NAME IN ITEMS entity_translucent_emissive energy_swirl dragon_rays)
        string(FIND "${CONFIG_TEXT}" "\"${GROUP_NAME}\"" GROUP_INDEX)
        if(GROUP_INDEX EQUAL -1)
            message(FATAL_ERROR "Special transparent group is not routed explicitly: ${GROUP_NAME} in ${CONFIG_PATH}")
        endif()
    endforeach()
    string(FIND "${CONFIG_TEXT}" "\"eyes\"" EYES_GROUP_INDEX)
    if(NOT EYES_GROUP_INDEX EQUAL -1)
        message(FATAL_ERROR "Eyes must be composed as a same-surface emissive coating, not routed as independent transparent geometry: ${CONFIG_PATH}")
    endif()
    string(FIND "${CONFIG_TEXT}" "world_no_reflect.rahit" STALE_ANY_HIT_INDEX)
    if(NOT STALE_ANY_HIT_INDEX EQUAL -1)
        message(FATAL_ERROR "World-no-reflect still bypasses alpha semantics: ${CONFIG_PATH}")
    endif()
endforeach()
