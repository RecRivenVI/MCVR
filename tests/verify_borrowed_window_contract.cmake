foreach(required_file IN ITEMS
        "${MCVR_SOURCE_DIR}/src/core/all_extern.hpp"
        "${MCVR_SOURCE_DIR}/src/core/middleware/com_radiance_client_proxy_vulkan_RendererProxy.cpp"
        "${MCVR_SOURCE_DIR}/src/core/vulkan/window.cpp")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing borrowed-window contract source: ${required_file}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/all_extern.hpp" extern_source)
file(READ
    "${MCVR_SOURCE_DIR}/src/core/middleware/com_radiance_client_proxy_vulkan_RendererProxy.cpp"
    renderer_proxy_source)
file(READ "${MCVR_SOURCE_DIR}/src/core/vulkan/window.cpp" window_source)

if(NOT extern_source MATCHES "PFN_glfwGetWindowAttrib")
    message(FATAL_ERROR "The GLFW runtime contract does not expose glfwGetWindowAttrib")
endif()
if(NOT renderer_proxy_source MATCHES "gp\\(\"glfwGetWindowAttrib\"\\)")
    message(FATAL_ERROR "Renderer initialization does not bind glfwGetWindowAttrib")
endif()
if(NOT window_source MATCHES "GLFW_GetWindowAttrib\\(window_, GLFW_CLIENT_API\\)")
    message(FATAL_ERROR "Borrowed GLFW windows are not checked for GLFW_NO_API")
endif()

string(FIND "${window_source}"
    "vk::Window::Window(std::shared_ptr<Instance> instance, GLFWwindow *window_)"
    borrowed_constructor_index)
string(FIND "${window_source}" "vk::Window::~Window()" window_destructor_index)
if(borrowed_constructor_index EQUAL -1 OR window_destructor_index EQUAL -1 OR
        window_destructor_index LESS_EQUAL borrowed_constructor_index)
    message(FATAL_ERROR "Cannot isolate the borrowed-window constructor")
endif()
math(EXPR borrowed_constructor_length "${window_destructor_index} - ${borrowed_constructor_index}")
string(SUBSTRING "${window_source}" ${borrowed_constructor_index} ${borrowed_constructor_length}
    borrowed_constructor_source)
if(borrowed_constructor_source MATCHES "GLFW_Terminate\\(" OR
        borrowed_constructor_source MATCHES "exit\\(")
    message(FATAL_ERROR "Borrowed-window construction must not terminate the host process")
endif()

if(NOT window_source MATCHES "GLFW_Terminate\\(" OR
        NOT window_source MATCHES "failure::raise\\(mcvr::failure::Kind::initialization")
    message(FATAL_ERROR "Owned-window failures must release GLFW and propagate through the native failure boundary")
endif()
if(window_source MATCHES "exit\\(" OR window_source MATCHES "abort\\(")
    message(FATAL_ERROR "Window construction must not terminate the host process")
endif()
