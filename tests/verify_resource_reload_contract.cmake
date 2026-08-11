if(NOT DEFINED MCVR_SOURCE_DIR OR NOT DEFINED JAVA_PROJECT_ROOT_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR and JAVA_PROJECT_ROOT_DIR are required")
endif()

get_filename_component(RADIANCE_SOURCE_DIR "${JAVA_PROJECT_ROOT_DIR}" ABSOLUTE)

file(READ "${MCVR_SOURCE_DIR}/src/core/render/textures.cpp" TEXTURES_TEXT)
foreach(REQUIRED_TEXT IN ITEMS
    "VkResult Textures::beginResourceReload()"
    "VkResult Textures::endResourceReload()"
    "resourceReloadRetainedImages_"
    "destinationImages"
    "VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR"
    "framework->pipeline()->onResourceReload()"
)
    string(FIND "${TEXTURES_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing native resource-reload contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

set(RENDERER_PROXY
    "${RADIANCE_SOURCE_DIR}/src/main/java/com/radiance/client/proxy/vulkan/RendererProxy.java")
file(READ "${RENDERER_PROXY}" RENDERER_PROXY_TEXT)
foreach(REQUIRED_TEXT IN ITEMS
    "beginResourceReloadNative"
    "endResourceReloadNative"
    "synchronized (TextureProxy.class)"
)
    string(FIND "${RENDERER_PROXY_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing Java resource-reload JNI contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

set(RELOAD_COORDINATOR
    "${RADIANCE_SOURCE_DIR}/src/main/java/com/radiance/client/texture/ResourceReloadCoordinator.java")
file(READ "${RELOAD_COORDINATOR}" RELOAD_COORDINATOR_TEXT)
foreach(REQUIRED_TEXT IN ITEMS
    "RendererProxy.beginResourceReload()"
    "RendererProxy.endResourceReload(finishNative)"
    "claimAuxiliaryTexture"
)
    string(FIND "${RELOAD_COORDINATOR_TEXT}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing Java resource-generation contract: ${REQUIRED_TEXT}")
    endif()
endforeach()
