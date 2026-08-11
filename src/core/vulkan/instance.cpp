#include "core/render/streamline_runtime.hpp"

#include "core/logging.hpp"
#include "core/failure_state.hpp"
#include "core/diagnostics/device_loss_trace.hpp"
#include "core/render/renderer.hpp"
#include "core/vulkan/instance.hpp"

#include "core/render/modules/world/dlss/dlss_wrapper.hpp"
#include "core/render/modules/world/xess_upscaler/xess_wrapper.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <unordered_set>
#include <vector>

const char *DEBUG_LAYER = "VK_LAYER_KHRONOS_validation";

auto instanceCout() {
    return mcvr::log::info("Instance");
}

auto instanceCerr() {
    return mcvr::log::error("Instance");
}

// Debug callback
VkBool32 debugCallback(VkDebugReportFlagsEXT flags,
                       VkDebugReportObjectTypeEXT objType,
                       uint64_t srcObject,
                       size_t location,
                       int32_t msgCode,
                       const char *pLayerPrefix,
                       const char *pMsg,
                       void *pUserData) {
    try {
        if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
            instanceCerr() << "ERROR: [" << pLayerPrefix << "] Code " << msgCode << " : " << pMsg << std::endl;
        } else if (flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) {
            instanceCerr() << "WARNING: [" << pLayerPrefix << "] Code " << msgCode << " : " << pMsg << std::endl;
        }
    } catch (...) {
        // Vulkan callbacks are C ABI boundaries and must not propagate exceptions.
    }

    return VK_FALSE;
}

vk::Instance::Instance() {
    GLFW_Init();

    if (const auto result = volkInitialize(); result != VK_SUCCESS) {
        mcvr::log::error("Instance") << "volkInitialize failed" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::initialization, result, "volkInitialize");
    }

    mcvr::StreamlineRuntime::get().initialize(Renderer::folderPath / "dlss");

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VulkanClear";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ClearScreenEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::set<std::string> extStorage;
    std::vector<std::string> dlssRequiredExtensions;
    bool dlssRequirementQuerySuccess = false;
#ifdef MCVR_ENABLE_XESS
    std::vector<std::string> xessRequiredExtensions;
    bool xessRequirementQuerySuccess = false;
#endif

    // Get instance extensions required by GLFW to draw to window
    unsigned int glfwExtensionCount;
    const char **glfwExtensions;
    glfwExtensions = GLFW_GetRequiredInstanceExtensions(&glfwExtensionCount);
#ifdef DEBUG
    instanceCout() << "glfw extensions:" << std::endl;
#endif
    for (int i = 0; i < glfwExtensionCount; i++) {
#ifdef DEBUG
        instanceCout() << "\t" << glfwExtensions[i] << std::endl;
#endif
        extStorage.insert(glfwExtensions[i]);
    }

    // DLSS extensions
    std::vector<VkExtensionProperties> dlssSRExtensions;
    std::vector<std::string> dlssSRRequired;
    bool dlssSRQuery = NVSDK_NGX_SUCCEED(NgxContext::getDlssRRRequiredInstanceExtensions(dlssSRExtensions, NVSDK_NGX_Feature_SuperSampling));
    for (const auto &extension : dlssSRExtensions) {
        dlssSRRequired.emplace_back(extension.extensionName);
        extStorage.insert(extension.extensionName);
    }
    std::vector<VkExtensionProperties> dlssFGExtensions;
    std::vector<std::string> dlssFGRequired;
    bool dlssFGQuery = NVSDK_NGX_SUCCEED(NgxContext::getDlssRRRequiredInstanceExtensions(dlssFGExtensions, NVSDK_NGX_Feature_FrameGeneration));
    for (const auto &extension : dlssFGExtensions) {
        dlssFGRequired.emplace_back(extension.extensionName);
        extStorage.insert(extension.extensionName);
    }

    std::vector<VkExtensionProperties> dlssExtensions;
    NVSDK_NGX_Result dlssExtensionQueryResult = NgxContext::getDlssRRRequiredInstanceExtensions(dlssExtensions);
    if (NVSDK_NGX_SUCCEED(dlssExtensionQueryResult)) {
        dlssRequirementQuerySuccess = true;
#ifdef DEBUG
        instanceCout() << "dlss extensions:" << std::endl;
#endif
        for (const auto &dlssExtension : dlssExtensions) {
#ifdef DEBUG
            instanceCout() << "\t" << dlssExtension.extensionName << std::endl;
#endif
            extStorage.insert(dlssExtension.extensionName);
            dlssRequiredExtensions.emplace_back(dlssExtension.extensionName);
        }
    } else {
        instanceCerr() << "failed to query dlss instance extensions; skipping." << std::endl;
    }

#ifdef MCVR_ENABLE_XESS
    std::vector<const char *> xessExtensions;
    uint32_t xessMinApiVersion = 0;
    if (mcvr::XeSSWrapper::getRequiredInstanceExtensions(xessExtensions, &xessMinApiVersion)) {
        xessRequirementQuerySuccess = true;
#    ifdef DEBUG
        instanceCout() << "xess extensions:" << std::endl;
#    endif
        for (const char *extension : xessExtensions) {
#    ifdef DEBUG
            instanceCout() << "\t" << extension << std::endl;
#    endif
            extStorage.insert(extension);
            xessRequiredExtensions.emplace_back(extension);
        }

        if (xessMinApiVersion > appInfo.apiVersion) { appInfo.apiVersion = xessMinApiVersion; }
    } else {
        instanceCerr() << "xess instance extensions unavailable; skipping." << std::endl;
    }
#endif

    // dynamic vertex input state ext
    // repeated for dlss, but make sure
    extStorage.insert(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

#ifdef DEBUG
    extStorage.insert(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    if (mcvr::diagnostics::device_loss::enabled()) extStorage.insert(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // Check for extensions
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

    if (extensionCount == 0) {
        instanceCerr() << "no extensions supported!" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::initialization, VK_ERROR_EXTENSION_NOT_PRESENT,
                             "vkEnumerateInstanceExtensionProperties(no extensions)");
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());
    std::unordered_set<std::string> availableExtensionSet;
    availableExtensionSet.reserve(availableExtensions.size());
    for (const auto &availableExtension : availableExtensions) {
        availableExtensionSet.insert(availableExtension.extensionName);
    }

#ifdef DEBUG
    instanceCout() << "supported extensions:" << std::endl;
    for (const auto &extension : availableExtensions) {
        instanceCout() << "\t" << extension.extensionName << std::endl;
    }
#endif

    auto areRequiredExtensionsSupported = [&](const std::vector<std::string> &requiredExtensions) {
        for (const auto &requiredExtension : requiredExtensions) {
            if (availableExtensionSet.find(requiredExtension) == availableExtensionSet.end()) { return false; }
        }
        return true;
    };

    dlssInstanceExtensionsCompatible_ =
        dlssRequirementQuerySuccess && areRequiredExtensionsSupported(dlssRequiredExtensions);
    dlssSRCompatible_ = dlssSRQuery && areRequiredExtensionsSupported(dlssSRRequired);
    dlssFGCompatible_ = dlssFGQuery && areRequiredExtensionsSupported(dlssFGRequired);
    if (!dlssInstanceExtensionsCompatible_) {
        instanceCerr() << "dlss instance extension requirements are not fully satisfied." << std::endl;
    }

#ifdef MCVR_ENABLE_XESS
    xessInstanceExtensionsCompatible_ =
        xessRequirementQuerySuccess && areRequiredExtensionsSupported(xessRequiredExtensions);
    if (!xessInstanceExtensionsCompatible_) {
        instanceCerr() << "xess instance extension requirements are not fully satisfied." << std::endl;
    }
#endif

    std::vector<const char *> extensions;
    for (const auto &extension : extStorage) {
        if (availableExtensionSet.find(extension) == availableExtensionSet.end()) {
            instanceCerr() << "extension not supported, skipping: " << extension << std::endl;
            continue;
        }
        extensions.push_back(extension.c_str());
    }

#ifdef DEBUG
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
    const bool validationLayerAvailable =
        std::any_of(availableLayers.begin(), availableLayers.end(), [](const auto &layer) {
            return std::strcmp(layer.layerName, DEBUG_LAYER) == 0;
        });

    instanceCout() << "selected extensions:" << std::endl;
    for (const auto &extension : extensions) { instanceCout() << "\t" << extension << std::endl; }
#endif

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

#ifdef DEBUG
    if (validationLayerAvailable) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &DEBUG_LAYER;
    }

    // VkValidationFeatureEnableEXT enables[] = {
    //     VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
    //     VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
    // };

    // VkValidationFeaturesEXT validationFeatures = {};
    // validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    // validationFeatures.enabledValidationFeatureCount = 2;
    // validationFeatures.pEnabledValidationFeatures = enables;

    // createInfo.pNext = &validationFeatures;
#endif

    // Initialize Vulkan instance
    if (const auto result = vkCreateInstance(&createInfo, nullptr, &instance_); result != VK_SUCCESS) {
        instanceCerr() << "failed to create instance!" << std::endl;
        mcvr::failure::raise(mcvr::failure::Kind::initialization, result, "vkCreateInstance");
    } else {
#ifdef DEBUG
        instanceCout() << "created vulkan instance" << std::endl;
#endif
    }

    volkLoadInstance(instance_);
    mcvr::StreamlineRuntime::get().hookInstance(instance_);

}

vk::Instance::~Instance() {
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

#ifdef DEBUG
    instanceCout() << "instance deconstructed" << std::endl;
#endif
}

VkInstance &vk::Instance::vkInstance() {
    return instance_;
}

bool vk::Instance::isDlssInstanceExtensionsCompatible() const {
    return dlssInstanceExtensionsCompatible_;
}

bool vk::Instance::isXessInstanceExtensionsCompatible() const {
    return xessInstanceExtensionsCompatible_;
}
