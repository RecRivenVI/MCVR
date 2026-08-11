#include "core/render/streamline_runtime.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <dbghelp.h>
#include "streamline_evaluate_probe.hpp"
#include "streamline_present_probe.hpp"

// Opt-in hardware smoke test, never launched by ordinary CTest. No game/window.
int main(int argc, char **argv) {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *exception)->LONG {
        auto process=GetCurrentProcess();
        SymInitialize(process,nullptr,TRUE);
        STACKFRAME64 stack{};
        auto context=*exception->ContextRecord;
        stack.AddrPC={context.Rip,0,AddrModeFlat}; stack.AddrStack={context.Rsp,0,AddrModeFlat}; stack.AddrFrame={context.Rbp,0,AddrModeFlat};
        std::cerr<<"EXCEPTION code="<<std::hex<<exception->ExceptionRecord->ExceptionCode<<" address="<<context.Rip<<std::endl;
        for (int i=0;i<32;++i) {
            auto base=SymGetModuleBase64(process,stack.AddrPC.Offset); char name[MAX_PATH]{};
            GetModuleFileNameA((HMODULE)base,name,MAX_PATH);
            std::cerr<<name<<"+0x"<<std::hex<<stack.AddrPC.Offset-base<<std::endl;
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,GetCurrentThread(),&stack,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)) break;
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    if (argc != 2) return 2;
    if (volkInitialize()!=VK_SUCCESS) return 3;
    auto &sl=mcvr::StreamlineRuntime::get();
    if (!sl.initialize(std::filesystem::path(argv[1]))) return 4;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instanceInfo.pApplicationInfo=&app;
    const char *instanceExtensions[]{VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    instanceInfo.enabledExtensionCount=2; instanceInfo.ppEnabledExtensionNames=instanceExtensions;
    VkInstance instance{};
    if (vkCreateInstance(&instanceInfo,nullptr,&instance)!=VK_SUCCESS) return 5;
    volkLoadInstance(instance); sl.hookInstance(instance);
    uint32_t count=0; vkEnumeratePhysicalDevices(instance,&count,nullptr);
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance,&count,devices.data());
    VkPhysicalDevice physical{};
    for (auto candidate:devices) {
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate,&properties);
        if (properties.vendorID==0x10de) { physical=candidate; std::cout<<properties.deviceName<<std::endl; break; }
    }
    if (!physical) return 6;
    uint32_t familyCount=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount); vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,families.data());
    uint32_t family=0; while (family<familyCount && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    float priority=1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex=family; queue.queueCount=1; queue.pQueuePriorities=&priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount=1; deviceInfo.pQueueCreateInfos=&queue;
    const char *deviceExtensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    deviceInfo.enabledExtensionCount=1; deviceInfo.ppEnabledExtensionNames=deviceExtensions;
    VkDevice device{};
    if (vkCreateDevice(physical,&deviceInfo,nullptr,&device)!=VK_SUCCESS) return 7;
    volkLoadDevice(device); sl.hookDevice(device,physical);
    bool ok=true;
    for (auto feature:{sl::kFeatureDLSS,sl::kFeatureDLSS_RR,sl::kFeatureDLSS_G,sl::kFeatureReflex}) {
        const bool available=sl.supported(feature); std::cout<<"feature="<<feature<<" supported="<<available<<std::endl; ok &= available;
    }
    if (sl.slDLSSGetOptimalSettings && sl.slDLSSDGetOptimalSettings) {
        sl::DLSSOptions sr{}; sr.mode=sl::DLSSMode::eBalanced; sr.outputWidth=2560; sr.outputHeight=1440;
        sl::DLSSOptimalSettings srSize{}; ok &= sl.check(sl.slDLSSGetOptimalSettings(sr,srSize),"probe SR sizes");
        sl::DLSSDOptions rr{}; rr.mode=sl::DLSSMode::eBalanced; rr.outputWidth=2560; rr.outputHeight=1440;
        sl::DLSSDOptimalSettings rrSize{}; ok &= sl.check(sl.slDLSSDGetOptimalSettings(rr,rrSize),"probe RR sizes");
        std::cout<<"SR="<<srSize.optimalRenderWidth<<'x'<<srSize.optimalRenderHeight<<" RR="<<rrSize.optimalRenderWidth<<'x'<<rrSize.optimalRenderHeight<<std::endl;
        ok &= srSize.optimalRenderWidth>0 && rrSize.optimalRenderWidth>0;
    } else ok=false;
    if (ok) {
        try { evaluateProbe(physical,device,family); presentProbe(instance,physical,device,family); }
        catch (const std::exception &error) { std::cerr<<error.what()<<std::endl; ok=false; }
    }
    sl.beginFrame(1); sl.marker(sl::PCLMarker::eSimulationStart); sl.marker(sl::PCLMarker::eSimulationEnd);
    vkDeviceWaitIdle(device); sl.shutdown(); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    std::cout << (ok?"PASS: device/capabilities/size queries; no image-quality claim":"FAIL") <<std::endl;
    return ok?0:8;
}
