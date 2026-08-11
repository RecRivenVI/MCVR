#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "core/vulkan/window.hpp"

#include "core/vulkan/instance.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

bool vk::Window::framebufferResized = false;

namespace {
VkResult createWindowSurface(VkInstance instance, GLFWwindow *window, VkSurfaceKHR *surface) {
#if defined(_WIN32) && defined(CORE_LIB)
    if (p_glfwGetWin32Window && vkCreateWin32SurfaceKHR) {
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hwnd=p_glfwGetWin32Window(window);
        info.hinstance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(info.hwnd,GWLP_HINSTANCE));
        return vkCreateWin32SurfaceKHR(instance,&info,nullptr,surface);
    }
#endif
    return GLFW_CreateWindowSurface(instance,window,nullptr,surface);
}
}
vk::Window::Window(std::shared_ptr<Instance> instance, uint32_t width, uint32_t height)
    : instance_(instance), width_(width), height_(height) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // TODO: enable this
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window_ = glfwCreateWindow(width_, height_, "Vulkan Window", nullptr, nullptr);
    if (!window_) {
        mcvr::log::error("Window") << "Cannot create glfw window!" << std::endl;
        GLFW_Terminate();
        mcvr::failure::raise(mcvr::failure::Kind::initialization, VK_ERROR_INITIALIZATION_FAILED,
                             "glfwCreateWindow");
    }

    VkResult result = createWindowSurface(instance_->vkInstance(), window_, &surface_);
    if (result != VK_SUCCESS) {
        mcvr::log::error("Window") << "Cannot create vulkan window surface!" << std::endl;
        GLFW_Terminate();
        mcvr::failure::raise(mcvr::failure::Kind::initialization, result, "glfwCreateWindowSurface");
    }
}

vk::Window::Window(std::shared_ptr<Instance> instance, GLFWwindow *window_) : instance_(instance), window_(window_) {
    if (window_ == nullptr) {
        throw std::invalid_argument("Cannot create a Vulkan surface for a null borrowed GLFW window");
    }
    const int clientApi = GLFW_GetWindowAttrib(window_, GLFW_CLIENT_API);
    if (clientApi != GLFW_NO_API) {
        throw std::runtime_error("Borrowed GLFW window must use GLFW_NO_API: GLFW_CLIENT_API=" +
                                 std::to_string(clientApi));
    }

    GLFW_GetWindowSize(window_, reinterpret_cast<int *>(&width_), reinterpret_cast<int *>(&height_));
    VkResult result = createWindowSurface(instance_->vkInstance(), window_, &surface_);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Cannot create Vulkan surface for borrowed GLFW window: VkResult=" +
                                 std::to_string(result));
    }
}

vk::Window::~Window() {
    if (surface_ != VK_NULL_HANDLE) { vkDestroySurfaceKHR(instance_->vkInstance(), surface_, nullptr); }

#ifdef DEBUG
    mcvr::log::info("Window") << "[Window] window deconstructed" << std::endl;
#endif
}

uint32_t vk::Window::width() {
    return width_;
}

uint32_t vk::Window::height() {
    return height_;
}

GLFWwindow *vk::Window::window() {
    return window_;
}

VkSurfaceKHR &vk::Window::vkSurface() {
    return surface_;
}
