if(WIN32)
    add_definitions(-DNOMINMAX)
    set(GLSLVALIDATOR glslangValidator.exe)
else()
    set(GLSLVALIDATOR glslangValidator)
endif(WIN32)

file(GLOB_RECURSE GLSL_SHADERS CONFIGURE_DEPENDS "*.vert" "*.frag" "*.rgen" "*.rmiss" "*.rchit" "*.rahit" "*.comp")
list(FILTER GLSL_SHADERS EXCLUDE REGEX "world/ray_tracing/internal") # exclude world shaders
file(GLOB_RECURSE GLSL_DEPENDS CONFIGURE_DEPENDS
    "../common/*.hpp"
    "../common/*.glsl"
    "util/*.glsl"
    "world/nrd/*.glsl"
    "overlay/post/*.glsl")

set(SPIRV_OUTPUT_PATH "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPIRV_OUTPUT_PATH})

set(SPIRV_OUTPUT_FILES "")

set(SHADER_FLAGS
    "$<$<CONFIG:Debug>:-g>"
    "$<$<CONFIG:Debug>:-Od>"
    "-DNRD_NORMAL_ENCODING=${NRD_NORMAL_ENCODING}"
    "-DNRD_ROUGHNESS_ENCODING=${NRD_ROUGHNESS_ENCODING}"
)

foreach(GLSL_SHADER ${GLSL_SHADERS})
    file(RELATIVE_PATH REL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" "${GLSL_SHADER}")
    get_filename_component(REL_DIR "${REL_PATH}" DIRECTORY)
    get_filename_component(FILE_NAME "${GLSL_SHADER}" NAME_WE)
    get_filename_component(FILE_EXT "${GLSL_SHADER}" LAST_EXT)
    string(REPLACE "." "" FILE_TYPE "${FILE_EXT}")

    set(OUTPUT_DIR "${SPIRV_OUTPUT_PATH}/${REL_DIR}")
    set(SPIRV_FILE "${OUTPUT_DIR}/${FILE_NAME}_${FILE_TYPE}.spv")

    add_custom_command(
        OUTPUT "${SPIRV_FILE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_DIR}"
        COMMAND ${GLSLVALIDATOR} ${SHADER_FLAGS} -V "${GLSL_SHADER}" -o "${SPIRV_FILE}" --target-env vulkan1.4 -I${PROJECT_SOURCE_DIR}/src -I${CMAKE_CURRENT_SOURCE_DIR}/util
        DEPENDS "${GLSL_SHADER}" ${GLSL_DEPENDS}
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    list(APPEND SPIRV_OUTPUT_FILES "${SPIRV_FILE}")
endforeach()

# Framebuffer clears use one output per draw-buffer slot, including GL_NONE slots.
foreach(CLEAR_ATTACHMENT_COUNT RANGE 2 32)
    set(CLEAR_SPIRV "${SPIRV_OUTPUT_PATH}/overlay/clear_${CLEAR_ATTACHMENT_COUNT}_frag.spv")
    add_custom_command(
        OUTPUT "${CLEAR_SPIRV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SPIRV_OUTPUT_PATH}/overlay"
        COMMAND ${GLSLVALIDATOR} ${SHADER_FLAGS} -V "${CMAKE_CURRENT_SOURCE_DIR}/overlay/clear.frag"
            "-DCLEAR_ATTACHMENTS=${CLEAR_ATTACHMENT_COUNT}" -o "${CLEAR_SPIRV}" --target-env vulkan1.4
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overlay/clear.frag"
        VERBATIM COMMAND_EXPAND_LISTS
    )
    list(APPEND SPIRV_OUTPUT_FILES "${CLEAR_SPIRV}")
endforeach()

# CONFIGURE_DEPENDS updates the generated commands when shader sources change,
# but CMake does not automatically delete outputs whose source was removed.
# Keep the install tree source-faithful so obsolete post-render shaders cannot
# survive in a clean staging directory or the packaged Radiance JAR.
file(GLOB_RECURSE EXISTING_SPIRV_OUTPUTS "${SPIRV_OUTPUT_PATH}/*.spv")
foreach(EXISTING_SPIRV_OUTPUT ${EXISTING_SPIRV_OUTPUTS})
    list(FIND SPIRV_OUTPUT_FILES "${EXISTING_SPIRV_OUTPUT}" EXPECTED_SPIRV_INDEX)
    if(EXPECTED_SPIRV_INDEX EQUAL -1)
        file(REMOVE "${EXISTING_SPIRV_OUTPUT}")
    endif()
endforeach()

add_custom_target(shaders ALL
    DEPENDS ${SPIRV_OUTPUT_FILES} ${GLSL_DEPENDS}
)

install(
    DIRECTORY ${SPIRV_OUTPUT_PATH}/
    DESTINATION ${MCVR_INSTALL_BIN_DIR}/res
    FILES_MATCHING PATTERN "*.spv"
)

install(
    DIRECTORY ${SPIRV_OUTPUT_PATH}/
    DESTINATION ${MCVR_INSTALL_LIB_DIR}/shaders
    FILES_MATCHING PATTERN "*.spv"
)

set(ZIP_INSTALL_DEST "${MCVR_INSTALL_LIB_DIR}/shaders/world/ray_tracing")
set(RT_INTERNAL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/world/ray_tracing/internal")
set(RT_UTIL_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/util")
set(RT_COMMON_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(RT_SHARC_SRC_DIR "${PROJECT_SOURCE_DIR}/extern/sharc/include")
set(RT_ZIP_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/ray_tracing_internal_zip")
set(RT_LEGACY_SHADER_PACK_ZIPS restir-di.zip)

foreach(RT_PACK_NAME ${MCVR_BUILT_IN_SHADER_PACKS})
    if(NOT EXISTS "${RT_INTERNAL_ROOT_DIR}/${RT_PACK_NAME}/configs.json")
        message(FATAL_ERROR "Built-in shader pack is missing configs.json: ${RT_INTERNAL_ROOT_DIR}/${RT_PACK_NAME}")
    endif()
endforeach()
message(STATUS "Installing ray tracing built-in shader packs: ${MCVR_BUILT_IN_SHADER_PACKS}")

install(CODE "
    file(MAKE_DIRECTORY \"${ZIP_INSTALL_DEST}\")
    file(REMOVE \"${ZIP_INSTALL_DEST}/internal.zip\")
    foreach(RT_LEGACY_SHADER_PACK_ZIP ${RT_LEGACY_SHADER_PACK_ZIPS})
        file(REMOVE \"${ZIP_INSTALL_DEST}/\${RT_LEGACY_SHADER_PACK_ZIP}\")
    endforeach()

    foreach(RT_PACK_NAME ${MCVR_BUILT_IN_SHADER_PACKS})
        set(RT_PACK_SRC_DIR \"${RT_INTERNAL_ROOT_DIR}/\${RT_PACK_NAME}\")
        set(RT_PACK_STAGING_DIR \"${RT_ZIP_STAGING_DIR}/\${RT_PACK_NAME}\")
        set(RT_PACK_OUTPUT \"${ZIP_INSTALL_DEST}/\${RT_PACK_NAME}.zip\")

        file(REMOVE_RECURSE \"\${RT_PACK_STAGING_DIR}\")
        file(MAKE_DIRECTORY \"\${RT_PACK_STAGING_DIR}\")
        file(COPY \"\${RT_PACK_SRC_DIR}/\" DESTINATION \"\${RT_PACK_STAGING_DIR}\")
        file(COPY \"${RT_UTIL_SRC_DIR}/\" DESTINATION \"\${RT_PACK_STAGING_DIR}/util\")
        file(COPY \"${RT_COMMON_SRC_DIR}/\" DESTINATION \"\${RT_PACK_STAGING_DIR}/common\")
        file(COPY \"${RT_SHARC_SRC_DIR}/\" DESTINATION \"\${RT_PACK_STAGING_DIR}/extern/sharc/include\")
        file(REMOVE \"\${RT_PACK_OUTPUT}\")

        file(COPY \"${RT_INTERNAL_ROOT_DIR}/vanilla-pt/priority/\" DESTINATION \"\${RT_PACK_STAGING_DIR}/priority\")

        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" -E tar \"cf\" \"\${RT_PACK_OUTPUT}\" --format=zip .
            WORKING_DIRECTORY \"\${RT_PACK_STAGING_DIR}\"
            RESULT_VARIABLE ZIP_RESULT
            OUTPUT_VARIABLE ZIP_STDOUT
            ERROR_VARIABLE ZIP_STDERR
        )

        if(NOT ZIP_RESULT EQUAL 0)
            message(FATAL_ERROR \"Failed to create \${RT_PACK_NAME}.zip (code=\${ZIP_RESULT})\\nstdout: \${ZIP_STDOUT}\\nstderr: \${ZIP_STDERR}\")
        endif()
    endforeach()
")
