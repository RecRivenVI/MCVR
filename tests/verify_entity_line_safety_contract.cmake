if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()

set(ENTITIES_SOURCE "${MCVR_SOURCE_DIR}/src/core/render/entities.cpp")
set(ENTITY_JNI_SOURCE
    "${MCVR_SOURCE_DIR}/src/core/middleware/com_radiance_client_proxy_world_EntityProxy.cpp")

foreach(SOURCE_PATH IN ITEMS "${ENTITIES_SOURCE}" "${ENTITY_JNI_SOURCE}")
    if(NOT EXISTS "${SOURCE_PATH}")
        message(FATAL_ERROR "Missing entity line-safety source: ${SOURCE_PATH}")
    endif()
endforeach()

file(READ "${ENTITIES_SOURCE}" ENTITIES_TEXT)
file(READ "${ENTITY_JNI_SOURCE}" ENTITY_JNI_TEXT)

string(FIND "${ENTITIES_TEXT}" "edge length d must be > 0" THROW_INDEX)
if(NOT THROW_INDEX EQUAL -1)
    message(FATAL_ERROR "Invalid line width must not escape as an uncaught C++ exception")
endif()

foreach(REQUIRED_FRAGMENT IN ITEMS
        "Skipping line geometry with invalid width"
        "!(task.lineWidth > 0.0f) || !std::isfinite(task.lineWidth)"
        "if (!(d > 0.0) || !std::isfinite(d)) { return {false, {}}; }")
    string(FIND "${ENTITIES_TEXT}" "${REQUIRED_FRAGMENT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Entity line-width guard is missing: ${REQUIRED_FRAGMENT}")
    endif()
endforeach()

foreach(REQUIRED_FRAGMENT IN ITEMS
        "core/middleware/jni_exception.hpp"
        "jni::invokeVoid(env, \"Queue entity geometry build\""
        "jni::invokeVoid(env, \"Build entity geometry\"")
    string(FIND "${ENTITY_JNI_TEXT}" "${REQUIRED_FRAGMENT}" REQUIRED_INDEX)
    if(REQUIRED_INDEX EQUAL -1)
        message(FATAL_ERROR "Entity JNI exception boundary is missing: ${REQUIRED_FRAGMENT}")
    endif()
endforeach()

message(STATUS "Entity line-width and JNI exception-safety contract verified")
