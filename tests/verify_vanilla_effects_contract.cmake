set(PACK_ROOTS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced"
)

foreach(PACK_ROOT IN LISTS PACK_ROOTS)
    set(CONFIG_PATH "${PACK_ROOT}/configs.json")
    file(READ "${CONFIG_PATH}" CONFIG_TEXT)
    if(CONFIG_TEXT MATCHES "post_render_(particle|weather|text|name_tag|outline|star)")
        message(FATAL_ERROR
            "Non-fullscreen content must remain ray-traced world geometry, not post-render passes: ${CONFIG_PATH}")
    endif()
    foreach(REQUIRED_TEXT IN ITEMS
        "\"name\": \"priority_geometry\""
        "\"name\": \"priority_composite\""
        "\"pass\": \"priority_geometry\""
        "\"pass\": \"priority_composite\""
        "\"priority_outline\":"
        "priority/outline.rchit"
    )
        string(FIND "${CONFIG_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR "Missing vanilla effect contract '${REQUIRED_TEXT}' in ${CONFIG_PATH}")
        endif()
    endforeach()

    string(FIND "${CONFIG_TEXT}" "\"pass\": \"priority_geometry\"" PRIORITY_BEGIN_INDEX)
    string(FIND "${CONFIG_TEXT}" "\"pass\": \"priority_composite\"" PRIORITY_COMPOSITE_INDEX)
    if(PRIORITY_BEGIN_INDEX EQUAL -1 OR PRIORITY_COMPOSITE_INDEX LESS PRIORITY_BEGIN_INDEX)
        message(FATAL_ERROR
            "Priority world geometry must be traced before it is composited: ${CONFIG_PATH}")
    endif()
endforeach()

# Sky data is shared by both the path-traced miss shader and the Java sky state.
# These checks keep vanilla END/sunrise/horizon and starBrightness/rain
# semantics from silently regressing while the UBO remains ABI-compatible.
file(READ "${MCVR_SOURCE_DIR}/src/common/shared.hpp" SKY_SHARED_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "T_UINT moonTextureID;"
    "T_FLOAT starBrightness;"
)
    string(FIND "${SKY_SHARED_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing SkyUBO sky contract '${REQUIRED_TEXT}'")
    endif()
endforeach()

foreach(PACK_ROOT IN LISTS PACK_ROOTS)
    if(PACK_ROOT MATCHES "vanilla-pt$")
        set(MISS_SHADER "${PACK_ROOT}/world/world.rmiss")
    else()
        set(MISS_SHADER "${PACK_ROOT}/common/world.rmiss")
    endif()
    file(READ "${MISS_SHADER}" MISS_ENTRY_SOURCE)
    if(NOT MISS_ENTRY_SOURCE MATCHES "ALLOW_VOLUMETRIC_CLOUD_MISS")
        message(FATAL_ERROR "Missing world miss entry for sky contract: ${MISS_SHADER}")
    endif()
    if(PACK_ROOT MATCHES "vanilla-pt$")
        set(MISS_IMPL_SHADER "${PACK_ROOT}/common/world_miss_eval_impl.glsl")
    else()
        set(MISS_IMPL_SHADER "${PACK_ROOT}/common/world_miss_eval.glsl")
    endif()
    file(READ "${MISS_IMPL_SHADER}" MISS_SOURCE)
    foreach(REQUIRED_TEXT IN ITEMS
        "vec3 evalEndSky"
        "worldUBO.endSkyTextureID"
        "vec3 applySunriseGradient"
        "skyUBO.isSkyDark > 0"
        "rayDir.y <= 0.0"
        "float progress = clamp(skyUBO.rainGradient, 0.0, 1.0)"
    )
        string(FIND "${MISS_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR "Missing sky miss contract '${REQUIRED_TEXT}': ${MISS_IMPL_SHADER}")
        endif()
    endforeach()

    foreach(REQUIRED_TEXT IN ITEMS
        "uint starHash"
        "vec3 evalPathTracedStars"
        "skyUBO.starBrightness"
        "backgroundRadiance += evalPathTracedStars"
    )
        string(FIND "${MISS_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR "Missing path-traced star contract '${REQUIRED_TEXT}': ${PACK_ROOT}")
        endif()
    endforeach()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/render/modules/world/post_render/post_render_module.cpp" POST_RENDER_SOURCE)
foreach(FORBIDDEN_TEXT IN ITEMS
    "RenderPass::Target::Outline"
    "RenderPass::Target::Star"
    "RenderPass::Target::NameTagSeeThrough"
    outlinePostFlag
    nameTagSeeThroughPostFlag
)
    string(FIND "${POST_RENDER_SOURCE}" "${FORBIDDEN_TEXT}" FORBIDDEN_INDEX)
    if(NOT FORBIDDEN_INDEX EQUAL -1)
        message(FATAL_ERROR "Legacy non-fullscreen post-render target remains: ${FORBIDDEN_TEXT}")
    endif()
endforeach()

foreach(SHADER IN ITEMS creeper.frag invert.frag spider.frag entity_effect.vert)
    if(NOT EXISTS "${MCVR_SOURCE_DIR}/src/shader/overlay/post/${SHADER}")
        message(FATAL_ERROR "Missing entity camera effect shader: ${SHADER}")
    endif()
endforeach()

foreach(SHADER IN ITEMS
    spider_blur_main_h15.frag
    spider_blur_temp_v15.frag
    spider_blur_main_h7.frag
    spider_blur_temp_v7.frag
)
    if(NOT EXISTS "${MCVR_SOURCE_DIR}/src/shader/overlay/post/${SHADER}")
        message(FATAL_ERROR "Missing exact spider blur pass: ${SHADER}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/shader/overlay/post/spider.frag" SPIDER_SHADER)
foreach(REQUIRED_TEXT IN ITEMS
    "vec2(1.25, 2.0)"
    "vec2(2.35, 4.2)"
    "vec2(-1.1, -1.5)"
    "vec2(0.45, -4.45)"
    "vec2(-0.385, -1.29)"
    "vec2(-0.965, -1.29)"
    "vec3(1.0, 0.8, 0.8)"
)
    string(FIND "${SPIDER_SHADER}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing Minecraft 1.21.1 spider contract: ${REQUIRED_TEXT}")
    endif()
endforeach()
string(FIND "${SPIDER_SHADER}" "boxBlur" SPIDER_APPROXIMATION_INDEX)
if(NOT SPIDER_APPROXIMATION_INDEX EQUAL -1)
    message(FATAL_ERROR "Spider composition still embeds the rejected one-pass blur approximation")
endif()
foreach(REQUIRED_TEXT IN ITEMS "largeBlurImage" "smallBlurImage")
    string(FIND "${SPIDER_SHADER}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Spider composition does not sample exact blur target '${REQUIRED_TEXT}'")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/render/render_framework.cpp" FRAMEWORK_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "VK_QUERY_TYPE_TIMESTAMP"
    "vkCmdResetQueryPool"
    "vkCmdWriteTimestamp"
    "vkGetQueryPoolResults"
    "timestampPeriod"
    "completeGpuProfile"
)
    string(FIND "${FRAMEWORK_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing Vulkan frame-timer contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/middleware/com_radiance_client_proxy_vulkan_RendererProxy.cpp" RENDERER_PROXY_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "RendererProxy_beginGpuProfile"
    "RendererProxy_isGpuProfileReady"
    "RendererProxy_gpuProfileTimeNs"
    "RendererProxy_backendString"
    "properties.deviceName"
    "VK_VERSION_MAJOR(properties.apiVersion)"
)
    string(FIND "${RENDERER_PROXY_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing JNI frame-timer contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/render/entities.cpp" ENTITY_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "case World::DrawMode::TRIANGLES"
    "geometryGroupName == \"entity_translucent_emissive\""
    "geometryGroupName == \"eyes\""
    "geometryGroupName == \"beacon_beam\""
    "geometryGroupName == \"lightning\""
    "geometryGroupName == \"dragon_rays\""
    "geometryGroupName == \"priority_outline\""
)
    string(FIND "${ENTITY_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing path-traced vanilla geometry contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

foreach(SHADER IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/world_no_reflect.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/world_no_reflect.rchit"
)
    file(READ "${SHADER}" SHADER_TEXT)
    string(FIND "${SHADER_TEXT}" "tint * albedoEmission * mainRay.throughput" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "World-no-reflect shader drops explicit vertex emission: ${SHADER}")
    endif()
endforeach()

foreach(SHADER IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/transparent_only.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/transparent_only.rchit"
)
    file(READ "${SHADER}" SHADER_TEXT)
    string(FIND "${SHADER_TEXT}" "shadedRgb * alpha * albedoEmission * mainRay.throughput" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Transparent-only shader drops explicit vertex emission: ${SHADER}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/priority/outline.rchit"
    PRIORITY_OUTLINE_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "#define PRIORITY_OUTLINE_COLOR vec3(1.0)"
    "priorityRay.color = vec4(PRIORITY_OUTLINE_COLOR, 1.0)"
    "priorityRay.hitT = gl_HitTEXT"
)
    string(FIND "${PRIORITY_OUTLINE_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Priority outline color contract is missing: ${REQUIRED_TEXT}")
    endif()
endforeach()

foreach(FORBIDDEN_TEXT IN ITEMS
    "layout(set ="
    "buffer_reference"
    "gl_InstanceCustomIndexEXT"
)
    string(FIND "${PRIORITY_OUTLINE_SOURCE}" "${FORBIDDEN_TEXT}" FORBIDDEN_INDEX)
    if(NOT FORBIDDEN_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Priority outline must remain descriptor-free to preserve the priority pipeline layout: ${FORBIDDEN_TEXT}")
    endif()
endforeach()

foreach(CONFIG_FILE IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/configs.json"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/configs.json"
)
    file(READ "${CONFIG_FILE}" PRIORITY_CONFIG_SOURCE)
    foreach(REQUIRED_TEXT IN ITEMS
        "\"priority_outline_red\""
        "\"PRIORITY_OUTLINE_COLOR\": \"vec3(1.0, 0.333333333, 0.333333333)\""
    )
        string(FIND "${PRIORITY_CONFIG_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Priority outline palette contract is missing '${REQUIRED_TEXT}': ${CONFIG_FILE}")
        endif()
    endforeach()
endforeach()

foreach(PRIMARY_SHADER IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/world.rgen"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/primary_trace.glsl"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/world.rgen"
)
    file(READ "${PRIMARY_SHADER}" PRIMARY_SOURCE)
    foreach(REQUIRED_TEXT IN ITEMS
        "uint cameraEntityMask = worldUBO.isFirstPerson > 0 ? 0u : PLAYER_MASK"
        "WORLD_MASK | cameraEntityMask"
    )
        string(FIND "${PRIMARY_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
        if(REQUIRED_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Camera-entity primary/secondary visibility contract is missing '${REQUIRED_TEXT}': ${PRIMARY_SHADER}")
        endif()
    endforeach()
endforeach()

foreach(SHADOW_SOURCE_FILE IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/world.rgen"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/direct_light_eval.glsl"
)
    file(READ "${SHADOW_SOURCE_FILE}" SHADOW_VISIBILITY_SOURCE)
    string(FIND "${SHADOW_VISIBILITY_SOURCE}" "WORLD_MASK | PLAYER_MASK" PLAYER_SHADOW_INDEX)
    if(PLAYER_SHADOW_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Camera-entity geometry is missing from a shadow visibility mask: ${SHADOW_SOURCE_FILE}")
    endif()
endforeach()

foreach(SHADER IN ITEMS
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/default.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/no_height.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt/world/world.rgen"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/direct_light_eval.glsl"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/common/direct_light_visibility.glsl"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/volumetric_light/volumetric_light.rgen"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/default.rchit"
    "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced/world/no_height.rchit"
)
    file(READ "${SHADER}" SHADER_TEXT)
    string(FIND "${SHADER_TEXT}" "PARTICLE_MASK" PARTICLE_SHADOW_INDEX)
    if(PARTICLE_SHADOW_INDEX EQUAL -1)
        message(FATAL_ERROR "Particle geometry is missing from a shadow visibility mask: ${SHADER}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/render/entities.cpp" ENTITY_SOURCE)
string(FIND "${ENTITY_SOURCE}" "colorLayer.a <= 0.0f" LINE_SEPARATOR_INDEX)
if(LINE_SEPARATOR_INDEX EQUAL -1)
    message(FATAL_ERROR "Debug line-strip zero-alpha separators must not become ray-traced prisms")
endif()
