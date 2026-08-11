add_executable(mcvr_contract_test contract_test.cpp)
target_compile_features(mcvr_contract_test PRIVATE cxx_std_23)
target_include_directories(mcvr_contract_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${MCVR_JNI_INCLUDE_DIR}"
    "${JNI_INCLUDE_DIRS}"
)
target_compile_definitions(mcvr_contract_test PRIVATE
    MCVR_EXPECTED_JNI_HEADER_COUNT=${MCVR_JNI_HEADER_COUNT}
)

add_test(NAME mcvr.contract COMMAND mcvr_contract_test)

add_executable(mcvr_texture_upload_region_test texture_upload_region_test.cpp)
target_compile_features(mcvr_texture_upload_region_test PRIVATE cxx_std_23)
target_include_directories(mcvr_texture_upload_region_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.texture-upload-region COMMAND mcvr_texture_upload_region_test)

add_executable(mcvr_submitted_geometry_cache_test submitted_geometry_cache_test.cpp)
target_compile_features(mcvr_submitted_geometry_cache_test PRIVATE cxx_std_23)
target_include_directories(mcvr_submitted_geometry_cache_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.submitted-geometry-cache COMMAND mcvr_submitted_geometry_cache_test)

add_executable(mcvr_entity_vertex_packing_test entity_vertex_packing_test.cpp
    "${PROJECT_SOURCE_DIR}/src/core/vulkan/vertex.cpp")
target_compile_features(mcvr_entity_vertex_packing_test PRIVATE cxx_std_23)
target_compile_definitions(mcvr_entity_vertex_packing_test PRIVATE VK_NO_PROTOTYPES)
target_include_directories(mcvr_entity_vertex_packing_test PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include" "${VOLK_INCLUDE_DIR}"
    "${VMA_INCLUDE_DIR}" "${GLFW_INCLUDE_DIR}" "${GLM_INCLUDE_DIR}" "${STB_INCLUDE_DIR}")
add_test(NAME mcvr.entity-vertex-packing COMMAND mcvr_entity_vertex_packing_test)

add_executable(mcvr_instancing_contract_test instancing_contract_test.cpp)
target_compile_features(mcvr_instancing_contract_test PRIVATE cxx_std_23)
target_include_directories(mcvr_instancing_contract_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}")
add_test(NAME mcvr.instancing-contract COMMAND mcvr_instancing_contract_test)

add_executable(mcvr_framebuffer_readback_contract_test framebuffer_readback_contract_test.cpp)
target_compile_features(mcvr_framebuffer_readback_contract_test PRIVATE cxx_std_23)
target_include_directories(mcvr_framebuffer_readback_contract_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}")
add_test(NAME mcvr.framebuffer-readback-contract COMMAND mcvr_framebuffer_readback_contract_test)

add_executable(mcvr_pipeline_cache_header_test pipeline_cache_header_test.cpp)
target_compile_features(mcvr_pipeline_cache_header_test PRIVATE cxx_std_23)
target_include_directories(mcvr_pipeline_cache_header_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include"
)
add_test(NAME mcvr.pipeline-cache-header COMMAND mcvr_pipeline_cache_header_test)

add_executable(mcvr_framebuffer_contract_test framebuffer_contract_test.cpp)
target_compile_features(mcvr_framebuffer_contract_test PRIVATE cxx_std_23)
target_compile_definitions(mcvr_framebuffer_contract_test PRIVATE VK_NO_PROTOTYPES)
target_include_directories(mcvr_framebuffer_contract_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include"
    "${VOLK_INCLUDE_DIR}"
    "${VMA_INCLUDE_DIR}"
    "${GLFW_INCLUDE_DIR}"
    "${GLM_INCLUDE_DIR}"
    "${STB_INCLUDE_DIR}"
)
add_test(NAME mcvr.framebuffer-contract COMMAND mcvr_framebuffer_contract_test)

find_package(Vulkan REQUIRED)
find_program(MCVR_TEST_GLSLANG NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(MCVR_TESSELLATION_TEST_SPIRV)
foreach(STAGE IN ITEMS vert tesc tese frag)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tessellation_patch.${STAGE}")
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/tessellation_patch_${STAGE}.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${MCVR_TEST_GLSLANG}" -V "${SOURCE}" --target-env vulkan1.2 -o "${OUTPUT}"
        DEPENDS "${SOURCE}" VERBATIM)
    list(APPEND MCVR_TESSELLATION_TEST_SPIRV "${OUTPUT}")
endforeach()
add_custom_target(mcvr_tessellation_test_shaders DEPENDS ${MCVR_TESSELLATION_TEST_SPIRV})
add_executable(mcvr_tessellation_gpu_test tessellation_gpu_test.cpp)
target_compile_features(mcvr_tessellation_gpu_test PRIVATE cxx_std_23)
target_link_libraries(mcvr_tessellation_gpu_test PRIVATE Vulkan::Vulkan)
add_dependencies(mcvr_tessellation_gpu_test mcvr_tessellation_test_shaders)
add_test(NAME mcvr.tessellation-gpu COMMAND mcvr_tessellation_gpu_test ${MCVR_TESSELLATION_TEST_SPIRV})
set_tests_properties(mcvr.tessellation-gpu PROPERTIES SKIP_RETURN_CODE 77)
foreach(STAGE IN ITEMS vert frag)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/custom_vertex_array.${STAGE}")
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/custom_vertex_array_${STAGE}.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${MCVR_TEST_GLSLANG}" -V "${SOURCE}" --target-env vulkan1.2 -o "${OUTPUT}"
        DEPENDS "${SOURCE}" VERBATIM)
    list(APPEND MCVR_CUSTOM_VERTEX_ARRAY_TEST_SPIRV "${OUTPUT}")
endforeach()
add_custom_target(mcvr_custom_vertex_array_test_shaders DEPENDS ${MCVR_CUSTOM_VERTEX_ARRAY_TEST_SPIRV})
add_executable(mcvr_custom_vertex_array_gpu_test custom_vertex_array_gpu_test.cpp)
target_compile_features(mcvr_custom_vertex_array_gpu_test PRIVATE cxx_std_23)
target_link_libraries(mcvr_custom_vertex_array_gpu_test PRIVATE Vulkan::Vulkan)
add_dependencies(mcvr_custom_vertex_array_gpu_test mcvr_custom_vertex_array_test_shaders)
add_test(NAME mcvr.custom-vertex-array-gpu COMMAND mcvr_custom_vertex_array_gpu_test ${MCVR_CUSTOM_VERTEX_ARRAY_TEST_SPIRV})
set_tests_properties(mcvr.custom-vertex-array-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mcvr_exposure_gpu_test exposure_gpu_test.cpp)
target_compile_features(mcvr_exposure_gpu_test PRIVATE cxx_std_23)
target_link_libraries(mcvr_exposure_gpu_test PRIVATE Vulkan::Vulkan)
if(TARGET shaders)
    add_dependencies(mcvr_exposure_gpu_test shaders)
endif()
add_test(NAME mcvr.exposure-gpu
    COMMAND mcvr_exposure_gpu_test
        "${PROJECT_BINARY_DIR}/src/shader/shaders/world/tone_mapping/exposure_comp.spv"
)
set_tests_properties(mcvr.exposure-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mcvr_framebuffer_gpu_test framebuffer_gpu_test.cpp)
target_compile_features(mcvr_framebuffer_gpu_test PRIVATE cxx_std_23)
target_include_directories(mcvr_framebuffer_gpu_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(mcvr_framebuffer_gpu_test PRIVATE Vulkan::Vulkan)
if(TARGET shaders)
    add_dependencies(mcvr_framebuffer_gpu_test shaders)
endif()
add_test(NAME mcvr.framebuffer-gpu
    COMMAND mcvr_framebuffer_gpu_test
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_vert.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_frag.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_2_frag.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/main_depth_frag.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/framebuffer_depth_blit_frag.spv"
)
set_tests_properties(mcvr.framebuffer-gpu PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME mcvr.post-color-sync-gpu
    COMMAND mcvr_framebuffer_gpu_test
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_vert.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_frag.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_2_frag.spv"
        --post-sync unused
)
set_tests_properties(mcvr.post-color-sync-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME mcvr.jni-coverage
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_JNI_INCLUDE_DIR=${MCVR_JNI_INCLUDE_DIR}
        -DMCVR_MIDDLEWARE_DIR=${PROJECT_SOURCE_DIR}/src/core/middleware
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_jni_coverage.cmake
)
add_test(NAME mcvr.shaders
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SHADER_DIR=${PROJECT_BINARY_DIR}/src/shader/shaders
        -DMCVR_INSTALL_DIR=${MCVR_INSTALL_LIB_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_shaders.cmake
)
add_test(NAME mcvr.borrowed-window
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_borrowed_window_contract.cmake
)
add_test(NAME mcvr.world-text-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_world_text_contract.cmake
)
add_test(NAME mcvr.glint-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_glint_contract.cmake
)
add_test(NAME mcvr.emissive-overlay-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_emissive_overlay_contract.cmake
)
add_test(NAME mcvr.vanilla-effects-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_vanilla_effects_contract.cmake
)
add_test(NAME mcvr.dlss-resource-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_dlss_resource_contract.cmake
)
add_test(NAME mcvr.texture-lifecycle-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_texture_lifecycle_contract.cmake
)
add_test(NAME mcvr.transparency-semantics-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DMCVR_JNI_INCLUDE_DIR=${MCVR_JNI_INCLUDE_DIR}
        -DJAVA_PROJECT_ROOT_DIR=${JAVA_PROJECT_ROOT_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_transparency_semantics_contract.cmake
)
add_test(NAME mcvr.particle-world-geometry-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_particle_world_geometry_contract.cmake
)
add_test(NAME mcvr.entity-line-safety-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_entity_line_safety_contract.cmake
)
add_test(NAME mcvr.native-image-upload-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DMCVR_JNI_INCLUDE_DIR=${MCVR_JNI_INCLUDE_DIR}
        -DJAVA_PROJECT_ROOT_DIR=${JAVA_PROJECT_ROOT_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_native_image_upload_contract.cmake
)
add_test(NAME mcvr.resource-reload-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DMCVR_JNI_INCLUDE_DIR=${MCVR_JNI_INCLUDE_DIR}
        -DJAVA_PROJECT_ROOT_DIR=${JAVA_PROJECT_ROOT_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_resource_reload_contract.cmake
)

add_executable(mcvr_world_mesh_contract_test world_mesh_contract_test.cpp)
target_compile_features(mcvr_world_mesh_contract_test PRIVATE cxx_std_23)
target_include_directories(mcvr_world_mesh_contract_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MCVR_JNI_INCLUDE_DIR}" "${JNI_INCLUDE_DIRS}")
add_test(NAME mcvr.world-mesh-contract COMMAND mcvr_world_mesh_contract_test)

# These executables use runtime assert() contracts; Release must execute them too.
foreach(CONTRACT_TARGET IN ITEMS mcvr_contract_test mcvr_pipeline_cache_header_test
        mcvr_framebuffer_contract_test mcvr_world_mesh_contract_test)
    if(TARGET ${CONTRACT_TARGET})
        if(MSVC)
            target_compile_options(${CONTRACT_TARGET} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${CONTRACT_TARGET} PRIVATE -UNDEBUG)
        endif()
    endif()
endforeach()

add_executable(mcvr_scene_scope_test scene_scope_test.cpp)
target_compile_features(mcvr_scene_scope_test PRIVATE cxx_std_23)
target_include_directories(mcvr_scene_scope_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.scene-scope COMMAND mcvr_scene_scope_test)
add_executable(mcvr_scene_camera_test scene_camera_test.cpp)
target_compile_features(mcvr_scene_camera_test PRIVATE cxx_std_23)
target_include_directories(mcvr_scene_camera_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}")
add_test(NAME mcvr.scene-camera COMMAND mcvr_scene_camera_test)

add_executable(mcvr_ui_coverage_contract_test ui_coverage_contract_test.cpp)
target_link_libraries(mcvr_ui_coverage_contract_test PRIVATE Vulkan::Vulkan)
target_compile_features(mcvr_ui_coverage_contract_test PRIVATE cxx_std_23)
target_include_directories(mcvr_ui_coverage_contract_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.ui-coverage-contract COMMAND mcvr_ui_coverage_contract_test)

add_executable(mcvr_ponder_budget_test ponder_budget_test.cpp)
target_compile_features(mcvr_ponder_budget_test PRIVATE cxx_std_23)
target_include_directories(mcvr_ponder_budget_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.ponder-budget COMMAND mcvr_ponder_budget_test)

add_executable(mcvr_texture_name_pool_test texture_name_pool_test.cpp)
target_compile_features(mcvr_texture_name_pool_test PRIVATE cxx_std_23)
target_include_directories(mcvr_texture_name_pool_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.texture-name-pool COMMAND mcvr_texture_name_pool_test)

add_executable(mcvr_texture_binding_snapshots_test texture_binding_snapshots_test.cpp)
target_compile_features(mcvr_texture_binding_snapshots_test PRIVATE cxx_std_23)
target_include_directories(mcvr_texture_binding_snapshots_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.texture-binding-snapshots COMMAND mcvr_texture_binding_snapshots_test)

add_executable(mcvr_upload_staging_budget_test upload_staging_budget_test.cpp)
target_compile_features(mcvr_upload_staging_budget_test PRIVATE cxx_std_23)
target_include_directories(mcvr_upload_staging_budget_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.upload-staging-budget COMMAND mcvr_upload_staging_budget_test)

add_executable(mcvr_device_loss_trace_test device_loss_trace_test.cpp)
target_include_directories(mcvr_device_loss_trace_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include"
)
add_test(NAME mcvr.device-loss-trace COMMAND mcvr_device_loss_trace_test)
set_tests_properties(mcvr.device-loss-trace PROPERTIES ENVIRONMENT "MCVR_DEVICE_LOSS_TRACE=1")

add_executable(mcvr_failure_state_contract_test failure_state_contract_test.cpp)
target_compile_features(mcvr_failure_state_contract_test PRIVATE cxx_std_23)
target_include_directories(mcvr_failure_state_contract_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include"
)
target_compile_definitions(mcvr_failure_state_contract_test PRIVATE
    MCVR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME mcvr.camera-ray-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_camera_ray_contract.cmake
)
add_executable(mcvr_external_chunk_handle_test external_chunk_handle_test.cpp)
target_compile_features(mcvr_external_chunk_handle_test PRIVATE cxx_std_23)
target_include_directories(mcvr_external_chunk_handle_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.external-chunk-handle COMMAND mcvr_external_chunk_handle_test)
add_test(NAME mcvr.ponder-resource-contract
    COMMAND ${CMAKE_COMMAND}
        -DMCVR_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_ponder_resource_contract.cmake
)
add_test(NAME mcvr.failure-state-contract COMMAND mcvr_failure_state_contract_test)
if(MSVC)
    target_compile_options(mcvr_failure_state_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(mcvr_failure_state_contract_test PRIVATE -UNDEBUG)
endif()

if(WIN32)
    add_executable(mcvr_streamline_probe EXCLUDE_FROM_ALL streamline_probe.cpp
        "${PROJECT_SOURCE_DIR}/src/core/render/streamline_runtime.cpp")
    target_link_libraries(mcvr_streamline_probe PRIVATE volk::volk dbghelp)
    target_compile_definitions(mcvr_streamline_probe PRIVATE VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(mcvr_streamline_probe PRIVATE "${MCVR_STREAMLINE_INCLUDE_DIR}"
        "${GLM_INCLUDE_DIR}" "${VMA_INCLUDE_DIR}" "${GLFW_INCLUDE_DIR}" "${VOLK_INCLUDE_DIR}" "${STB_INCLUDE_DIR}")
endif()

add_executable(mcvr_ui_frame_batch_test ui_frame_batch_test.cpp)
target_compile_features(mcvr_ui_frame_batch_test PRIVATE cxx_std_23)
target_include_directories(mcvr_ui_frame_batch_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.ui-frame-batch COMMAND mcvr_ui_frame_batch_test)

add_executable(mcvr_pending_uploads_test pending_uploads_test.cpp)
target_compile_features(mcvr_pending_uploads_test PRIVATE cxx_std_23)
target_include_directories(mcvr_pending_uploads_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include")
add_test(NAME mcvr.pending-uploads COMMAND mcvr_pending_uploads_test)

add_executable(mcvr_ui_geometry_cache_test ui_geometry_cache_test.cpp)
target_compile_features(mcvr_ui_geometry_cache_test PRIVATE cxx_std_23)
target_include_directories(mcvr_ui_geometry_cache_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}")
add_test(NAME mcvr.ui-geometry-cache COMMAND mcvr_ui_geometry_cache_test)

add_test(NAME mcvr.fg-ui-gpu
    COMMAND mcvr_framebuffer_gpu_test
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_vert.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_frag.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_2_frag.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/main_depth_frag.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/framebuffer_depth_blit_frag.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/world/upscaler/dlss_ui_coverage_comp.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/world/upscaler/fg_hudless_blur_comp.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/world/upscaler/dlss_ui_reset_comp.spv"
        "${PROJECT_BINARY_DIR}/src/shader/shaders/world/upscaler/fg_hudless_weight_comp.spv"
)
set_tests_properties(mcvr.fg-ui-gpu PROPERTIES SKIP_RETURN_CODE 77)

set(EXECUTION_PROBE_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/execution_probe.spv")
add_custom_command(OUTPUT "${EXECUTION_PROBE_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${MCVR_TEST_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/execution_probe.comp"
        --target-env vulkan1.2 -o "${EXECUTION_PROBE_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/execution_probe.comp" VERBATIM)
add_custom_target(mcvr_execution_probe_shader DEPENDS "${EXECUTION_PROBE_SPIRV}")
add_dependencies(mcvr_framebuffer_gpu_test mcvr_execution_probe_shader)
add_test(NAME mcvr.execution-buffer-gpu COMMAND mcvr_framebuffer_gpu_test
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_vert.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_frag.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_2_frag.spv"
    --execution "${EXECUTION_PROBE_SPIRV}")
set_tests_properties(mcvr.execution-buffer-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mcvr_streamline_evaluate_result_test streamline_evaluate_result_test.cpp)
target_compile_features(mcvr_streamline_evaluate_result_test PRIVATE cxx_std_23)
target_include_directories(mcvr_streamline_evaluate_result_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MCVR_STREAMLINE_INCLUDE_DIR}")
add_test(NAME mcvr.streamline-evaluate-result COMMAND mcvr_streamline_evaluate_result_test)

add_executable(mcvr_material_faces_test material_faces_test.cpp)
target_compile_features(mcvr_material_faces_test PRIVATE cxx_std_23)
target_include_directories(mcvr_material_faces_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/extern/vulkan_headers/include")
add_test(NAME mcvr.material-faces COMMAND mcvr_material_faces_test)

set(MATERIAL_FACES_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/material_faces.spv")
add_custom_command(OUTPUT "${MATERIAL_FACES_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${MCVR_TEST_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/material_faces.comp"
        "-I${PROJECT_SOURCE_DIR}/src/shader" --target-env vulkan1.2 -o "${MATERIAL_FACES_SPIRV}"
    DEPENDS shaders/material_faces.comp "${PROJECT_SOURCE_DIR}/src/shader/util/material_faces.glsl"
        "${PROJECT_SOURCE_DIR}/src/shader/util/ui_primary_visibility.glsl" VERBATIM)
add_executable(mcvr_material_faces_gpu_test material_faces_gpu_test.cpp "${MATERIAL_FACES_SPIRV}")
target_compile_features(mcvr_material_faces_gpu_test PRIVATE cxx_std_23)
target_compile_definitions(mcvr_material_faces_gpu_test PRIVATE VK_NO_PROTOTYPES)
target_include_directories(mcvr_material_faces_gpu_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(mcvr_material_faces_gpu_test PRIVATE volk::volk)
add_test(NAME mcvr.material-faces-gpu COMMAND mcvr_material_faces_gpu_test "${MATERIAL_FACES_SPIRV}")
set_tests_properties(mcvr.material-faces-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mcvr_chunk_scheduling_test chunk_scheduling_test.cpp)
target_compile_features(mcvr_chunk_scheduling_test PRIVATE cxx_std_23)
target_include_directories(mcvr_chunk_scheduling_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.chunk-scheduling COMMAND mcvr_chunk_scheduling_test)

add_executable(mcvr_chunk_trace_test chunk_trace_test.cpp)
target_compile_features(mcvr_chunk_trace_test PRIVATE cxx_std_23)
target_include_directories(mcvr_chunk_trace_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.chunk-trace-disabled COMMAND mcvr_chunk_trace_test)
set_tests_properties(mcvr.chunk-trace-disabled PROPERTIES ENVIRONMENT "RADIANCE_CHUNK_TRACE=0")
add_test(NAME mcvr.chunk-trace-enabled COMMAND mcvr_chunk_trace_test)
set_tests_properties(mcvr.chunk-trace-enabled PROPERTIES ENVIRONMENT "RADIANCE_CHUNK_TRACE=1")

add_test(NAME mcvr.ponder-coverage-gpu COMMAND mcvr_framebuffer_gpu_test
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_vert.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_frag.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_2_frag.spv"
    --ponder-coverage "${PROJECT_BINARY_DIR}/src/shader/shaders/preview/ponder_composite_comp.spv")
set_tests_properties(mcvr.ponder-coverage-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mcvr_ui_history_retirement_test ui_history_retirement_test.cpp)
target_compile_features(mcvr_ui_history_retirement_test PRIVATE cxx_std_23)
target_include_directories(mcvr_ui_history_retirement_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.ui-history-retirement COMMAND mcvr_ui_history_retirement_test)

add_executable(mcvr_deferred_frame_commands_test deferred_frame_commands_test.cpp)
target_compile_features(mcvr_deferred_frame_commands_test PRIVATE cxx_std_23)
target_include_directories(mcvr_deferred_frame_commands_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.deferred-frame-commands COMMAND mcvr_deferred_frame_commands_test)

set(DIAGRAM_SURFACE_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/diagram_surface.spv")
add_custom_command(OUTPUT "${DIAGRAM_SURFACE_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${MCVR_TEST_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/diagram_surface.comp"
        "-I${PROJECT_SOURCE_DIR}/src/shader" --target-env vulkan1.2 -o "${DIAGRAM_SURFACE_SPIRV}"
    DEPENDS shaders/diagram_surface.comp "${PROJECT_SOURCE_DIR}/src/shader/util/diagram_math.glsl"
        "${PROJECT_SOURCE_DIR}/src/shader/util/surface_overlay.glsl"
        "${PROJECT_SOURCE_DIR}/src/shader/util/priority_coverage.glsl"
        "${PROJECT_SOURCE_DIR}/src/shader/util/text_mode.glsl"
        "${PROJECT_SOURCE_DIR}/src/shader/util/alpha_mode.glsl" VERBATIM)
add_custom_target(mcvr_diagram_surface_shader DEPENDS "${DIAGRAM_SURFACE_SPIRV}")
add_dependencies(mcvr_framebuffer_gpu_test mcvr_diagram_surface_shader)
add_test(NAME mcvr.diagram-surface-gpu COMMAND mcvr_framebuffer_gpu_test
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_vert.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_frag.spv"
    "${PROJECT_BINARY_DIR}/src/shader/shaders/overlay/clear_2_frag.spv"
    --diagram-surface "${DIAGRAM_SURFACE_SPIRV}")
set_tests_properties(mcvr.diagram-surface-gpu PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mcvr_audit_sink_test audit_sink_test.cpp)
target_compile_features(mcvr_audit_sink_test PRIVATE cxx_std_23)
target_include_directories(mcvr_audit_sink_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME mcvr.audit-sink COMMAND mcvr_audit_sink_test)
