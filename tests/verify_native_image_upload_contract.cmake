if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()
if(NOT DEFINED JAVA_PROJECT_ROOT_DIR)
    message(FATAL_ERROR "JAVA_PROJECT_ROOT_DIR is required")
endif()

get_filename_component(RADIANCE_ROOT "${JAVA_PROJECT_ROOT_DIR}" ABSOLUTE)

set(TEXTURE_PROXY
    "${RADIANCE_ROOT}/src/main/java/com/radiance/client/proxy/vulkan/TextureProxy.java")
set(NATIVE_IMAGE_MIXIN
    "${RADIANCE_ROOT}/src/main/java/com/radiance/mixins/vulkan_render_integration/NativeImageMixins.java")
set(NATIVE_TEXTURES "${MCVR_SOURCE_DIR}/src/core/render/textures.cpp")

foreach(SOURCE_PATH IN ITEMS "${TEXTURE_PROXY}" "${NATIVE_IMAGE_MIXIN}" "${NATIVE_TEXTURES}")
    if(NOT EXISTS "${SOURCE_PATH}")
        message(FATAL_ERROR "Missing native-image upload contract source: ${SOURCE_PATH}")
    endif()
endforeach()

file(READ "${TEXTURE_PROXY}" TEXTURE_PROXY_TEXT)
file(READ "${NATIVE_IMAGE_MIXIN}" NATIVE_IMAGE_MIXIN_TEXT)
file(READ "${NATIVE_TEXTURES}" NATIVE_TEXTURES_TEXT)

foreach(REQUIRED_FRAGMENT IN ITEMS
        "Arrays.fill(textureIds, -1)"
        "return boundTextureIds[activeTextureUnit]")
    string(FIND "${TEXTURE_PROXY_TEXT}" "${REQUIRED_FRAGMENT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Texture binding contract is missing: ${REQUIRED_FRAGMENT}")
    endif()
endforeach()

# Supplement only: behavior is covered by Java TextureTasksTest and immutable descriptor tests.
foreach(REQUIRED_FRAGMENT IN ITEMS
        "TextureProxy.TASKS.submit(capturedOwner"
        "TextureProxy.TASKS.owner(TextureProxy.boundTexture())"
        "self.radiance$setTargetOwner(capturedOwner)"
        "NativeImage upload bypassed the texture ownership task")
    string(FIND "${NATIVE_IMAGE_MIXIN_TEXT}" "${REQUIRED_FRAGMENT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "NativeImage bound-texture recovery is missing: ${REQUIRED_FRAGMENT}")
    endif()
endforeach()

string(REGEX MATCHALL "exit\\(EXIT_FAILURE\\)" NATIVE_TEXTURE_EXIT_CALLS "${NATIVE_TEXTURES_TEXT}")
list(LENGTH NATIVE_TEXTURE_EXIT_CALLS NATIVE_TEXTURE_EXIT_COUNT)
if(NATIVE_TEXTURE_EXIT_COUNT GREATER 0)
    message(FATAL_ERROR "Texture JNI boundary must not terminate the JVM")
endif()

message(STATUS "NativeImage bound-texture upload contract verified")
