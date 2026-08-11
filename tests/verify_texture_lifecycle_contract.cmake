file(READ "${MCVR_SOURCE_DIR}/src/core/render/textures.cpp" TEXTURES_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "Textures::downloadTexture"
    "vkCmdCopyImageToBuffer"
    "downloadFromBuffer"
    "Textures::releaseTexture"
    "releasedTextureFallbacks_"
    "waitRenderQueueIdle"
    "bindTextureAndReleasedAliases"
)
    string(FIND "${TEXTURES_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing Vulkan texture lifecycle contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

string(FIND "${TEXTURES_SOURCE}" "level >= texture->mipLevels()" MIP_LEVEL_GUARD_INDEX)
if(MIP_LEVEL_GUARD_INDEX EQUAL -1)
    message(FATAL_ERROR "Texture readback must reject invalid mip levels before shifting dimensions")
endif()

file(READ "${MCVR_SOURCE_DIR}/src/core/middleware/com_radiance_client_proxy_vulkan_TextureProxy.cpp" TEXTURE_PROXY_SOURCE)
foreach(REQUIRED_TEXT IN ITEMS
    "TextureProxy_downloadTexture"
    "TextureProxy_releaseTextureIdNative"
)
    string(FIND "${TEXTURE_PROXY_SOURCE}" "${REQUIRED_TEXT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Missing JNI texture lifecycle contract: ${REQUIRED_TEXT}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/render/render_framework.cpp" FRAMEWORK_SOURCE)
string(FIND "${FRAMEWORK_SOURCE}" "dstBuffer->downloadFromBuffer()" INVALIDATE_INDEX)
if(INVALIDATE_INDEX EQUAL -1)
    message(FATAL_ERROR "Screenshot readback must invalidate non-coherent host memory before copying")
endif()
