if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()

set(DLSS_MODULE_CPP "${MCVR_SOURCE_DIR}/src/core/render/modules/world/dlss/dlss_module.cpp")
set(DLSS_WRAPPER_HPP "${MCVR_SOURCE_DIR}/src/core/render/modules/world/dlss/dlss_wrapper.hpp")
set(DLSS_WRAPPER_CPP "${MCVR_SOURCE_DIR}/src/core/render/modules/world/dlss/dlss_wrapper.cpp")

foreach(required_file IN ITEMS "${DLSS_MODULE_CPP}" "${DLSS_WRAPPER_HPP}" "${DLSS_WRAPPER_CPP}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing DLSS resource-contract source: ${required_file}")
    endif()
endforeach()

file(READ "${DLSS_MODULE_CPP}" dlss_module_source)
file(READ "${DLSS_WRAPPER_HPP}" dlss_wrapper_hpp_source)
file(READ "${DLSS_WRAPPER_CPP}" dlss_wrapper_cpp_source)

foreach(required_marker IN ITEMS
        sampledInputBarrier
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
        VK_IMAGE_LAYOUT_GENERAL
        VK_ACCESS_2_SHADER_WRITE_BIT)
    string(FIND "${dlss_module_source}" "${required_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "DLSS resource-state contract is missing ${required_marker}")
    endif()
endforeach()

set(dlss_wrapper_source "${dlss_wrapper_hpp_source}\n${dlss_wrapper_cpp_source}")
foreach(required_marker IN ITEMS
        m_resetPending
        "reset||m_resetPending"
        "m_resetPending=false")
    string(FIND "${dlss_wrapper_source}" "${required_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "DLSS first-evaluation reset contract is missing ${required_marker}")
    endif()
endforeach()
