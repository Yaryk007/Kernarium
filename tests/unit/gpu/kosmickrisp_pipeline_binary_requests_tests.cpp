// SPDX-License-Identifier: GPL-2.0-or-later
//
// What a caller of vkCreatePipelineBinariesKHR chooses is checked before it is
// read, copied or dereferenced, and a request the runtime cannot serve is
// refused with VK_ERROR_VALIDATION_FAILED (or VK_ERROR_OUT_OF_HOST_MEMORY for
// data too large to allocate) and a null handle; none of it ends the process.
//
// A pipeline binary an application imports names itself by a key, and the key
// says how long it is. VkPipelineBinaryKeyKHR::keySize is the one field of the
// import that no generated validation bounds (the registry marks it
// noautovalidity), and a Venus guest sends it as a plain number beside a key
// array of exactly VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR bytes. The Vulkan
// runtime used to copy keySize bytes into the binary's own 32-byte key, guarded
// only by an assert that release builds compile out.
//
// Every key the driver hands out is 32 bytes, so an import under any other size
// is refused before anything is copied or allocated from it. A key of 32 bytes
// still makes a binary whose key and data read back as imported, and a
// pipeline created from the imported binaries (and no shader module) works.
//
// The data of an imported binary is allocated beside the binary, so a data
// size within the binary's own size of SIZE_MAX, added to it, wrapped to a
// small allocation that the copy overran.
//
// The binaries come from keys and data to import, a pipeline, or a pipeline
// create info, and this driver has no internal cache to look a create info up
// in. A request with none of them, or with only a create info, used to fall
// into the import and read the one it does not have.
//
// A pipeline's key is derived from the pipeline create info that begins the
// chain of a VkPipelineCreateInfoKHR, which the runtime reads by that
// structure's type. A chain that is empty, or that begins with another
// structure, names no pipeline: reading it as a graphics pipeline's create
// info took its stages from fields of another layout, so it is refused instead.
// The key of the device, a create info of NULL, is served.
//
// A shader stage that names no shader (no module, nothing chained, no
// identifier) or no entry point name is invalid, and the runtime's hash of the
// stage used to read the identifier and measure the name it does not have. The
// pipeline is refused with an error and a null handle, and the key of such a
// pipeline is answered, not crashed on.
//
// A partly failed import returns the first failure and keeps the binaries it
// made before it, which the caller owns (the Venus renderer relies on this, and
// destroys them when it does not hand them on). VkPipelineBinaryCreateInfoKHR's
// pPipelineCreateInfo is looked up only where pipelineBinaryInternalCache is
// true, which this driver is not, so the chain it reads there is not reached.
//
// Each row runs in a process of its own and the parent reports it, so a row
// that crashes the driver is one failed row rather than the end of the test:
// on a driver without the checks, 33 and 4096 report success over a corrupted
// heap, 0xffffffff reads past the key's buffer, and the other rows
// dereference null or copy past their allocation.

#include "require.hpp"
#include "vulkan_draw_test.hpp"

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

extern char **environ;

namespace {

using uml::gpu::test::check;
using uml::gpu::test::instance_function;
using uml::gpu::test::structure;
using uml::testing::fail;
using uml::testing::require;

constexpr uint32_t keyBytes = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
static_assert(keyBytes == 32, "the runtime's key is one 32-byte hash");

// A compute shader whose main does nothing: the smallest module a pipeline
// can be made from.
constexpr uint32_t emptyCompute[] = {
    0x07230203, 0x00010300, 0, 5, 0,   // SPIR-V 1.3, ids below 5
    0x00020011, 1,                     // OpCapability Shader
    0x0003000e, 0, 1,                  // OpMemoryModel Logical GLSL450
    0x0005000f, 5, 4, 0x6e69616d, 0,   // OpEntryPoint GLCompute %4 "main"
    0x00060010, 4, 17, 1, 1, 1,        // OpExecutionMode %4 LocalSize 1 1 1
    0x00020013, 2,                     // %2 = OpTypeVoid
    0x00030021, 3, 2,                  // %3 = OpTypeFunction %2
    0x00050036, 2, 4, 0, 3,            // %4 = OpFunction %2 None %3
    0x000200f8, 1,                     // %1 = OpLabel
    0x000100fd,                        // OpReturn
    0x00010038,                        // OpFunctionEnd
};

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr getDevice = nullptr;

    template <typename T>
    T deviceFunction(const char *name) const
    {
        auto function = reinterpret_cast<T>(getDevice(device, name));
        require(function != nullptr, name);
        return function;
    }

#define CALL(name, ...) deviceFunction<PFN_vk##name>("vk" #name)(__VA_ARGS__)
#define CHECK(name, ...) check(CALL(name, __VA_ARGS__), "vk" #name)

    Context() { initialize(); }
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    ~Context()
    {
        if (device)
            CALL(DestroyDevice, device, nullptr);
        if (instance)
            instance_function<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(instance, nullptr);
    }

    void initialize()
    {
        auto application = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.pApplicationName = "pipeline binary requests regression";
        application.apiVersion = VK_API_VERSION_1_3;
        auto info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        info.pApplicationInfo = &application;
        check(instance_function<PFN_vkCreateInstance>(instance, "vkCreateInstance")(&info, nullptr, &instance),
              "vkCreateInstance");
        getDevice = instance_function<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
        auto enumerate = instance_function<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        check(enumerate(instance, &count, nullptr), "physical device count");
        require(count == 1, "expected one directly linked native provider");
        VkPhysicalDevice physical;
        check(enumerate(instance, &count, &physical), "physical device");
        auto driver = structure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
        auto properties = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        properties.pNext = &driver;
        instance_function<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2")(
            physical, &properties);
        require(driver.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP, "unexpected native provider");

        auto binaries = structure<VkPhysicalDevicePipelineBinaryFeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR);
        auto maintenance5 = structure<VkPhysicalDeviceMaintenance5FeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
        auto features = structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        features.pNext = &binaries;
        binaries.pNext = &maintenance5;
        instance_function<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2")(physical,
                                                                                                    &features);
        require(binaries.pipelineBinaries && maintenance5.maintenance5,
                "the driver does not offer pipeline binaries and the pipeline flags that capture them");
        binaries = structure<VkPhysicalDevicePipelineBinaryFeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR);
        maintenance5 = structure<VkPhysicalDeviceMaintenance5FeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
        binaries.pipelineBinaries = VK_TRUE;
        binaries.pNext = &maintenance5;
        maintenance5.maintenance5 = VK_TRUE;

        auto enumerateExtensions =
            instance_function<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
        check(enumerateExtensions(physical, nullptr, &count, nullptr), "extension count");
        std::vector<VkExtensionProperties> extensions(count);
        check(enumerateExtensions(physical, nullptr, &count, extensions.data()), "extensions");
        std::vector<const char *> enabled = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
                                             VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
        for (const auto &extension : extensions)
            if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
                enabled.push_back("VK_KHR_portability_subset");

        const float priority = 1;
        auto queue = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        auto deviceInfo = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceInfo.pNext = &binaries;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queue;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
        deviceInfo.ppEnabledExtensionNames = enabled.data();
        check(instance_function<PFN_vkCreateDevice>(instance, "vkCreateDevice")(physical, &deviceInfo, nullptr,
                                                                                &device),
              "vkCreateDevice");
    }

    // Imports one binary whose key claims `keySize` bytes and whose data claims
    // `claimedDataSize` bytes (`data.size()` when not given), over keys and
    // data that are `keyBytes` and `data.size()` long, and returns the call's
    // result. The key structure sits at the start of a buffer of its own that
    // is far larger than any key, so a driver's read of keySize bytes stays
    // inside this process whatever it is; only a write can go wrong.
    VkResult import(uint32_t keySize, const std::vector<uint8_t> &data, VkPipelineBinaryKHR &binary,
                    std::optional<size_t> claimedDataSize = std::nullopt) const
    {
        std::vector<uint8_t> keyBuffer(size_t{1} << 20);
        auto *key = reinterpret_cast<VkPipelineBinaryKeyKHR *>(keyBuffer.data());
        key->sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
        key->keySize = keySize;
        for (uint32_t i = 0; i < keyBytes; ++i)
            key->key[i] = static_cast<uint8_t>(0x40 + i);
        VkPipelineBinaryDataKHR binaryData{};
        binaryData.dataSize = claimedDataSize.value_or(data.size());
        binaryData.pData = const_cast<uint8_t *>(data.data());
        VkPipelineBinaryKeysAndDataKHR keysAndData{};
        keysAndData.binaryCount = 1;
        keysAndData.pPipelineBinaryKeys = key;
        keysAndData.pPipelineBinaryData = &binaryData;
        auto createInfo = structure<VkPipelineBinaryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR);
        createInfo.pKeysAndDataInfo = &keysAndData;
        auto handles = structure<VkPipelineBinaryHandlesInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR);
        handles.pipelineBinaryCount = 1;
        handles.pPipelineBinaries = &binary;
        binary = VK_NULL_HANDLE;
        return CALL(CreatePipelineBinariesKHR, device, &createInfo, nullptr, &handles);
    }

    // The key and data a binary reports.
    struct Readback {
        VkPipelineBinaryKeyKHR key;
        std::vector<uint8_t> data;
    };

    Readback readBack(VkPipelineBinaryKHR binary) const
    {
        auto info = structure<VkPipelineBinaryDataInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR);
        info.pipelineBinary = binary;
        Readback result{structure<VkPipelineBinaryKeyKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR), {}};
        size_t size = 0;
        CHECK(GetPipelineBinaryDataKHR, device, &info, &result.key, &size, nullptr);
        result.data.resize(size);
        CHECK(GetPipelineBinaryDataKHR, device, &info, &result.key, &size, result.data.data());
        require(size == result.data.size(), "the binary's data size changed between the two calls");
        return result;
    }
};

std::vector<uint8_t> sampleData()
{
    return {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
}

// A key that nothing has written: any byte of it that is still 0xaa was not.
VkPipelineBinaryKeyKHR unwrittenKey()
{
    auto key = structure<VkPipelineBinaryKeyKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR);
    key.keySize = 0xaaaaaaaau;
    std::memset(key.key, 0xaa, sizeof(key.key));
    return key;
}

// The key of the device, which names no pipeline, is served.
void keyOfTheDevice()
{
    Context context;
    VkPipelineBinaryKeyKHR key = unwrittenKey();
    check(context.CALL(GetPipelineKeyKHR, context.device, nullptr, &key), "the key of the device");
    require(key.keySize == keyBytes, "the key of the device is not a key's size");
    VkPipelineBinaryKeyKHR again = unwrittenKey();
    check(context.CALL(GetPipelineKeyKHR, context.device, nullptr, &again), "the key of the device, again");
    require(std::memcmp(key.key, again.key, keyBytes) == 0, "the key of the device changed");
}

// A compute pipeline's create info chained first names it, and its key differs
// from the device's and is stable.
void keyOfAComputePipeline()
{
    Context context;
    auto layoutInfo = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(context.CALL(CreatePipelineLayout, context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    auto moduleInfo = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    moduleInfo.codeSize = sizeof(emptyCompute);
    moduleInfo.pCode = emptyCompute;
    VkShaderModule module = VK_NULL_HANDLE;
    check(context.CALL(CreateShaderModule, context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");

    auto compute = structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    compute.stage = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute.stage.module = module;
    compute.stage.pName = "main";
    compute.layout = layout;
    auto pipelineInfo = structure<VkPipelineCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
    pipelineInfo.pNext = &compute;

    VkPipelineBinaryKeyKHR key = unwrittenKey();
    check(context.CALL(GetPipelineKeyKHR, context.device, &pipelineInfo, &key), "the key of a compute pipeline");
    require(key.keySize == keyBytes, "the key of a compute pipeline is not a key's size");
    VkPipelineBinaryKeyKHR again = unwrittenKey();
    check(context.CALL(GetPipelineKeyKHR, context.device, &pipelineInfo, &again), "the key of a compute pipeline, again");
    require(std::memcmp(key.key, again.key, keyBytes) == 0, "the key of a compute pipeline changed");
    VkPipelineBinaryKeyKHR device = unwrittenKey();
    check(context.CALL(GetPipelineKeyKHR, context.device, nullptr, &device), "the key of the device");
    require(std::memcmp(key.key, device.key, keyBytes) != 0, "a compute pipeline has the key of the device");

    context.CALL(DestroyShaderModule, context.device, module, nullptr);
    context.CALL(DestroyPipelineLayout, context.device, layout, nullptr);
}

// A create info whose chain does not begin with a pipeline's is refused and
// leaves the key as it was.
void keyRefused(bool emptyChain)
{
    Context context;
    auto robustness = structure<VkPipelineRobustnessCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO);
    auto pipelineInfo = structure<VkPipelineCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
    if (!emptyChain)
        pipelineInfo.pNext = &robustness;
    VkPipelineBinaryKeyKHR key = unwrittenKey();
    const VkResult result = context.CALL(GetPipelineKeyKHR, context.device, &pipelineInfo, &key);
    require(result == VK_ERROR_VALIDATION_FAILED,
            std::string(emptyChain ? "a create info with no chain" : "a create info that begins with another structure") +
                " was not refused: " + std::to_string(result));
    require(key.keySize == 0xaaaaaaaau, "a refused key request wrote the key");
}

// A key of the wrong size in the second entry fails the call after the first
// binary was made: the first handle is valid and the second null, and the
// call returns the failure.
void partialImportKeepsItsSuccesses()
{
    Context context;
    const std::vector<uint8_t> data = sampleData();
    std::array<VkPipelineBinaryKeyKHR, 2> keys{};
    std::array<VkPipelineBinaryDataKHR, 2> blobs{};
    for (size_t i = 0; i < keys.size(); ++i) {
        keys[i] = structure<VkPipelineBinaryKeyKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR);
        keys[i].keySize = keyBytes;
        for (uint32_t b = 0; b < keyBytes; ++b)
            keys[i].key[b] = static_cast<uint8_t>(0x40 + b + i);
        blobs[i].dataSize = data.size();
        blobs[i].pData = const_cast<uint8_t *>(data.data());
    }
    keys[1].keySize = keyBytes - 1;
    VkPipelineBinaryKeysAndDataKHR keysAndData{};
    keysAndData.binaryCount = 2;
    keysAndData.pPipelineBinaryKeys = keys.data();
    keysAndData.pPipelineBinaryData = blobs.data();
    auto createInfo = structure<VkPipelineBinaryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR);
    createInfo.pKeysAndDataInfo = &keysAndData;
    std::array<VkPipelineBinaryKHR, 2> binaries{VK_NULL_HANDLE, VK_NULL_HANDLE};
    auto handles = structure<VkPipelineBinaryHandlesInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR);
    handles.pipelineBinaryCount = 2;
    handles.pPipelineBinaries = binaries.data();
    const VkResult result = context.CALL(CreatePipelineBinariesKHR, context.device, &createInfo, nullptr, &handles);
    require(result == VK_ERROR_VALIDATION_FAILED,
            "a key of the wrong size in the second entry was not the call's result: " + std::to_string(result));
    require(binaries[0] != VK_NULL_HANDLE, "the binary made before the failure was not kept");
    require(binaries[1] == VK_NULL_HANDLE, "the entry that failed has a handle");
    require(context.readBack(binaries[0]).data == data, "the binary made before the failure reads back differently");
    context.CALL(DestroyPipelineBinaryKHR, context.device, binaries[0], nullptr);
}

// A key whose size is not a key's is refused before anything is copied or
// allocated from it, and leaves no binary behind.
void refused(uint32_t keySize)
{
    Context context;
    VkPipelineBinaryKHR binary = VK_NULL_HANDLE;
    const VkResult result = context.import(keySize, sampleData(), binary);
    if (binary != VK_NULL_HANDLE)
        context.deviceFunction<PFN_vkDestroyPipelineBinaryKHR>("vkDestroyPipelineBinaryKHR")(context.device, binary,
                                                                                            nullptr);
    require(result == VK_ERROR_VALIDATION_FAILED,
            "a key of " + std::to_string(keySize) + " bytes was not refused: " + std::to_string(result));
    require(binary == VK_NULL_HANDLE, "a refused key left a binary behind");
}

// A data size the binary and its data cannot be allocated in is refused like an
// allocation that cannot be satisfied, and leaves no binary behind.
void dataSizeOverflow(size_t dataSize)
{
    Context context;
    VkPipelineBinaryKHR binary = VK_NULL_HANDLE;
    const VkResult result = context.import(keyBytes, sampleData(), binary, dataSize);
    if (binary != VK_NULL_HANDLE)
        context.deviceFunction<PFN_vkDestroyPipelineBinaryKHR>("vkDestroyPipelineBinaryKHR")(context.device, binary,
                                                                                            nullptr);
    require(result == VK_ERROR_OUT_OF_HOST_MEMORY,
            "a data size of " + std::to_string(dataSize) + " bytes was not refused: " + std::to_string(result));
    require(binary == VK_NULL_HANDLE, "a refused data size left a binary behind");
}

// A request that names no source for its binaries, or only a pipeline create
// info this driver has no internal cache to look up, is refused.
void noSource(bool createInfoOnly)
{
    Context context;
    auto flags = structure<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
    auto pipelineInfo = structure<VkPipelineCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
    pipelineInfo.pNext = &flags;
    auto createInfo = structure<VkPipelineBinaryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR);
    if (createInfoOnly)
        createInfo.pPipelineCreateInfo = &pipelineInfo;
    VkPipelineBinaryKHR binary = VK_NULL_HANDLE;
    auto handles = structure<VkPipelineBinaryHandlesInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR);
    handles.pipelineBinaryCount = 1;
    handles.pPipelineBinaries = &binary;
    const VkResult result = context.CALL(CreatePipelineBinariesKHR, context.device, &createInfo, nullptr, &handles);
    if (binary != VK_NULL_HANDLE)
        context.deviceFunction<PFN_vkDestroyPipelineBinaryKHR>("vkDestroyPipelineBinaryKHR")(context.device, binary,
                                                                                            nullptr);
    require(result == VK_ERROR_VALIDATION_FAILED,
            std::string(createInfoOnly ? "a request with only a pipeline create info"
                                       : "a request with no source") +
                " was not refused: " + std::to_string(result));
    require(binary == VK_NULL_HANDLE, "a refused request left a binary behind");
}

// A key of the size every key has makes a binary that reads back as imported.
void accepted()
{
    Context context;
    VkPipelineBinaryKHR binary = VK_NULL_HANDLE;
    const std::vector<uint8_t> data = sampleData();
    check(context.import(keyBytes, data, binary), "importing a key of the key size");
    require(binary != VK_NULL_HANDLE, "no binary for a key of the key size");
    const auto readback = context.readBack(binary);
    require(readback.key.keySize == keyBytes, "the binary's key is not a key's size");
    for (uint32_t i = 0; i < keyBytes; ++i)
        require(readback.key.key[i] == static_cast<uint8_t>(0x40 + i), "the binary's key differs from the imported one");
    require(readback.data == data, "the binary's data differs from the imported data");
    context.deviceFunction<PFN_vkDestroyPipelineBinaryKHR>("vkDestroyPipelineBinaryKHR")(context.device, binary,
                                                                                        nullptr);
}

// The binaries a pipeline captured, imported by their own keys and data, make
// a pipeline again with no shader module: the import still works after the
// check.
void importedBinariesMakeAPipeline()
{
    Context context;
    auto layoutInfo = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(context.CALL(CreatePipelineLayout, context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");

    auto moduleInfo = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    moduleInfo.codeSize = sizeof(emptyCompute);
    moduleInfo.pCode = emptyCompute;
    VkShaderModule module = VK_NULL_HANDLE;
    check(context.CALL(CreateShaderModule, context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");

    // The pipeline the binaries are captured from.
    auto flags = structure<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR);
    flags.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
    auto pipelineInfo = structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipelineInfo.pNext = &flags;
    pipelineInfo.stage = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout;
    VkPipeline source = VK_NULL_HANDLE;
    check(context.CALL(CreateComputePipelines, context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &source),
          "vkCreateComputePipelines");

    auto createInfo = structure<VkPipelineBinaryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR);
    createInfo.pipeline = source;
    auto handles = structure<VkPipelineBinaryHandlesInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR);
    check(context.CALL(CreatePipelineBinariesKHR, context.device, &createInfo, nullptr, &handles),
          "counting the pipeline's binaries");
    require(handles.pipelineBinaryCount > 0, "the pipeline has no binaries to capture");
    std::vector<VkPipelineBinaryKHR> captured(handles.pipelineBinaryCount);
    handles.pPipelineBinaries = captured.data();
    check(context.CALL(CreatePipelineBinariesKHR, context.device, &createInfo, nullptr, &handles),
          "capturing the pipeline's binaries");

    std::vector<VkPipelineBinaryKeyKHR> keys;
    std::vector<std::vector<uint8_t>> blobs;
    for (VkPipelineBinaryKHR binary : captured) {
        auto readback = context.readBack(binary);
        require(readback.key.keySize == keyBytes, "a captured binary's key is not a key's size");
        keys.push_back(readback.key);
        blobs.push_back(std::move(readback.data));
    }
    std::vector<VkPipelineBinaryDataKHR> data(blobs.size());
    for (size_t i = 0; i < blobs.size(); ++i) {
        data[i].dataSize = blobs[i].size();
        data[i].pData = blobs[i].data();
    }

    // Import them under the keys they reported.
    VkPipelineBinaryKeysAndDataKHR keysAndData{};
    keysAndData.binaryCount = static_cast<uint32_t>(keys.size());
    keysAndData.pPipelineBinaryKeys = keys.data();
    keysAndData.pPipelineBinaryData = data.data();
    auto importInfo = structure<VkPipelineBinaryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR);
    importInfo.pKeysAndDataInfo = &keysAndData;
    std::vector<VkPipelineBinaryKHR> imported(keys.size());
    auto importHandles = structure<VkPipelineBinaryHandlesInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR);
    importHandles.pipelineBinaryCount = static_cast<uint32_t>(imported.size());
    importHandles.pPipelineBinaries = imported.data();
    check(context.CALL(CreatePipelineBinariesKHR, context.device, &importInfo, nullptr, &importHandles),
          "importing the captured binaries");
    for (VkPipelineBinaryKHR binary : imported)
        require(binary != VK_NULL_HANDLE, "an imported binary has no handle");

    // A pipeline from the imported binaries alone: the specification has the
    // driver ignore the stage's shader module, so none is given.
    auto binaryInfo = structure<VkPipelineBinaryInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR);
    binaryInfo.binaryCount = static_cast<uint32_t>(imported.size());
    binaryInfo.pPipelineBinaries = imported.data();
    auto fromBinaries = pipelineInfo;
    fromBinaries.pNext = &binaryInfo;
    fromBinaries.stage.module = VK_NULL_HANDLE;
    VkPipeline rebuilt = VK_NULL_HANDLE;
    check(context.CALL(CreateComputePipelines, context.device, VK_NULL_HANDLE, 1, &fromBinaries, nullptr, &rebuilt),
          "a pipeline from the imported binaries");
    require(rebuilt != VK_NULL_HANDLE, "the imported binaries made no pipeline");

    context.CALL(DestroyPipeline, context.device, rebuilt, nullptr);
    for (VkPipelineBinaryKHR binary : imported)
        context.CALL(DestroyPipelineBinaryKHR, context.device, binary, nullptr);
    for (VkPipelineBinaryKHR binary : captured)
        context.CALL(DestroyPipelineBinaryKHR, context.device, binary, nullptr);
    context.CALL(DestroyPipeline, context.device, source, nullptr);
    context.CALL(DestroyShaderModule, context.device, module, nullptr);
    context.CALL(DestroyPipelineLayout, context.device, layout, nullptr);
}

// A compute create info whose stage's module is null and whose chain names no
// shader, or whose entry point name is absent.
struct StageWithNoShader {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo moduleInfo = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    VkComputePipelineCreateInfo info = structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

    StageWithNoShader(Context &context, bool chainedModule, const char *name)
    {
        auto layoutInfo = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        check(context.CALL(CreatePipelineLayout, context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
        moduleInfo.codeSize = sizeof(emptyCompute);
        moduleInfo.pCode = emptyCompute;
        info.stage = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = VK_NULL_HANDLE;
        if (chainedModule)
            info.stage.pNext = &moduleInfo;
        info.stage.pName = name;
        info.layout = layout;
    }
};

void stageRefused(bool chainedModule, const char *name)
{
    Context context;
    StageWithNoShader stage(context, chainedModule, name);
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        context.CALL(CreateComputePipelines, context.device, VK_NULL_HANDLE, 1, &stage.info, nullptr, &pipeline);
    require(result != VK_SUCCESS, "a stage that names no shader or no entry point was made into a pipeline");
    require(pipeline == VK_NULL_HANDLE, "a refused pipeline has a handle");
    context.CALL(DestroyPipelineLayout, context.device, stage.layout, nullptr);
}

// The key of such a pipeline hashes what it has: it is answered and stable.
void keyOfAStageWithNoShader()
{
    Context context;
    StageWithNoShader stage(context, false, "main");
    auto pipelineInfo = structure<VkPipelineCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
    pipelineInfo.pNext = &stage.info;
    VkPipelineBinaryKeyKHR key = unwrittenKey();
    const VkResult result = context.CALL(GetPipelineKeyKHR, context.device, &pipelineInfo, &key);
    require(result == VK_SUCCESS || result == VK_ERROR_VALIDATION_FAILED,
            "the key of a stage that names no shader was answered with " + std::to_string(result));
    if (result == VK_SUCCESS)
        require(key.keySize == keyBytes, "the key of a stage that names no shader is not a key's size");
    context.CALL(DestroyPipelineLayout, context.device, stage.layout, nullptr);
}

#undef CHECK
#undef CALL

struct Row {
    const char *name;
    void (*run)();
};

const std::array<Row, 19> rows = {{
    {"refused-0", [] { refused(0); }},
    {"refused-31", [] { refused(keyBytes - 1); }},
    {"refused-33", [] { refused(keyBytes + 1); }},
    {"refused-4096", [] { refused(4096); }},
    {"refused-0xffffffff", [] { refused(0xffffffffu); }},
    {"refused-data-size-max", [] { dataSizeOverflow(SIZE_MAX); }},
    {"refused-data-size-near-max", [] { dataSizeOverflow(SIZE_MAX - 8); }},
    {"refused-no-source", [] { noSource(false); }},
    {"refused-create-info-only", [] { noSource(true); }},
    {"key-of-the-device", [] { keyOfTheDevice(); }},
    {"key-of-a-compute-pipeline", [] { keyOfAComputePipeline(); }},
    {"refused-key-of-an-empty-chain", [] { keyRefused(true); }},
    {"refused-key-of-a-robustness-chain", [] { keyRefused(false); }},
    {"partial-import-keeps-its-successes", [] { partialImportKeepsItsSuccesses(); }},
    {"accepted-32", [] { accepted(); }},
    {"imported-binaries-make-a-pipeline", [] { importedBinariesMakeAPipeline(); }},
    {"refused-stage-with-no-module", [] { stageRefused(false, "main"); }},
    {"refused-stage-with-no-entry-point-name", [] { stageRefused(true, nullptr); }},
    {"key-of-a-stage-with-no-module", [] { keyOfAStageWithNoShader(); }},
}};

// The path of this executable, which each row runs again: Metal's compiler
// service is not reachable from a forked child that has not executed anew.
std::string selfPath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    require(_NSGetExecutablePath(path.data(), &size) == 0, "the test's own path");
    path.resize(std::strlen(path.c_str()));
    return path;
}

// Runs a row in a process of its own and says how it ended; true when it
// passed.
bool runRow(const std::string &self, const Row &row)
{
    std::fflush(nullptr);
    const std::string flag = "--row";
    const std::string name = row.name;
    char *argv[] = {const_cast<char *>(self.c_str()), const_cast<char *>(flag.c_str()),
                    const_cast<char *>(name.c_str()), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawn(&child, self.c_str(), nullptr, nullptr, argv, environ);
    require(spawned == 0, "posix_spawn: " + std::string(std::strerror(spawned)));
    int status = 0;
    uml::testing::requireSyscall(waitpid(child, &status, 0) == child, "waitpid");
    const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (passed)
        std::fprintf(stderr, "row %s: ok\n", row.name);
    else if (WIFSIGNALED(status))
        std::fprintf(stderr, "row %s: FAILED, ended by signal %d\n", row.name, WTERMSIG(status));
    else
        std::fprintf(stderr, "row %s: FAILED, status %d\n", row.name, WEXITSTATUS(status));
    return passed;
}

} // namespace

// With no argument every row runs, each in its own process; with `--row NAME`,
// that row alone, in this one.
int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--row") == 0) {
        for (const Row &row : rows) {
            if (std::strcmp(argv[2], row.name) == 0) {
                row.run();
                return 0;
            }
        }
        fail("no row has the name given");
    }
    const std::string self = selfPath();
    uint32_t failures = 0;
    for (const Row &row : rows)
        failures += runRow(self, row) ? 0 : 1;
    require(failures == 0, std::to_string(failures) + " pipeline binary request rows failed");
    std::puts("PIPELINE_BINARY_REQUESTS_PASS key-sizes=0,31,33,4096,0xffffffff data-size-overflow=refused no-source=refused pipeline-key=refused-or-served partial-import=kept accepted=32 pipeline-from-import=pass");
    return 0;
}
