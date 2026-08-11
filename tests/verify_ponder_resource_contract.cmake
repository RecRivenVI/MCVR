# Supplemental source guard; behavioral collection/crop/cache and GPU tests are separate.
set(PONDER_SOURCE "${MCVR_SOURCE_DIR}/src/core/middleware/com_radiance_client_proxy_vulkan_UiPathTracingProxy.cpp")
file(READ "${PONDER_SOURCE}" SOURCE)
foreach(REQUIRED IN ITEMS
        "struct CompositeFrame"
        "scene->compositeFrames.assign(framework->swapchain()->imageCount(), {})"
        "auto &composite = scene->compositeFrames.at(mainFrame->frameIndex)"
        "if (!composite.output)"
        "composite.descriptor = builder.build(device)")
    string(FIND "${SOURCE}" "${REQUIRED}" FOUND)
    if(FOUND EQUAL -1)
        message(FATAL_ERROR "Ponder per-frame composite cache is missing: ${REQUIRED}")
    endif()
endforeach()

string(FIND "${SOURCE}" "retainer.retain(descriptor)" TRANSIENT_DESCRIPTOR)
string(FIND "${SOURCE}" "retainer.retain(output)" TRANSIENT_OUTPUT)
if(NOT TRANSIENT_DESCRIPTOR EQUAL -1 OR NOT TRANSIENT_OUTPUT EQUAL -1)
    message(FATAL_ERROR "Ponder composite resources returned to transient per-draw retention")
endif()
