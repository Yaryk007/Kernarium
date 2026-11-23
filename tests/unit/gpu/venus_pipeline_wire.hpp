// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The pipeline create infos the Venus renderer tests send: a compute and a
// graphics pipeline whose stage, shader module, library and binaries the test
// chooses, in the Venus driver's layout.

#include "venus_wire.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::gpu::testing::venus {

// A compute shader whose main does nothing: the smallest module a pipeline can
// be made from.
inline constexpr std::uint32_t emptyComputeSpirv[] = {
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
inline constexpr std::size_t emptyComputeSpirvWords = sizeof(emptyComputeSpirv) / 4;

// What a pipeline create info carries beyond its stage.
struct PipelineChain {
    bool binaries = false;             // a VkPipelineBinaryInfoKHR ...
    std::uint64_t binary = 0;          // ... over this one binary
    bool library = false;              // a VkPipelineLibraryCreateInfoKHR ...
    std::uint64_t libraryPipeline = 0; // ... over this one library
};

// What names the code of a shader stage.
struct StageSource {
    std::uint64_t module = 0;          // the stage's module, 0 for none
    bool moduleInfo = false;           // a VkShaderModuleCreateInfo of the empty compute shader
    bool name = true;                  // pName "main", or none
};

// The chain of a pipeline create info, from its pNext pointer on: the
// structures it carries, each followed by the rest of the chain and then its own
// fields.
inline void encodePipelineChain(Stream& s, const PipelineChain& chain)
{
    if (chain.binaries) {
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR);
    }
    if (chain.library) {
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    }
    s.ptr(false);
    if (chain.library) {
        s.u32(1); s.arraySize(1); s.u64(chain.libraryPipeline);
    }
    if (chain.binaries) {
        s.u32(1); s.arraySize(1); s.u64(chain.binary);
    }
}

// One VkPipelineShaderStageCreateInfo.
inline void encodeStage(Stream& s, VkShaderStageFlagBits stage, const StageSource& source)
{
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    if (source.moduleInfo) {
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        s.ptr(false);
        s.u32(0);                               // flags
        s.u64(emptyComputeSpirvWords * 4);      // codeSize
        s.arraySize(emptyComputeSpirvWords);
        for (const std::uint32_t word : emptyComputeSpirv)
            s.u32(word);
    } else {
        s.ptr(false);
    }
    s.u32(0);                                   // stage flags
    s.u32(stage);
    s.u64(source.module);
    if (source.name)
        s.string("main");
    else
        s.arraySize(0);
    s.ptr(false);                               // pSpecializationInfo
}

// The fields of a VkComputePipelineCreateInfo after its chain.
inline void encodeComputeSelf(Stream& s, const StageSource& source, std::uint64_t layout)
{
    s.u32(0);                                   // flags
    encodeStage(s, VK_SHADER_STAGE_COMPUTE_BIT, source);
    s.u64(layout);
    s.u64(0);                                   // basePipelineHandle
    s.u32(0);                                   // basePipelineIndex
}

// A VkComputePipelineCreateInfo.
inline void encodeComputeInfo(Stream& s, const StageSource& source, std::uint64_t layout,
    const PipelineChain& chain = {})
{
    s.u32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    encodePipelineChain(s, chain);
    encodeComputeSelf(s, source, layout);
}

// What a graphics pipeline has beyond its chain.
struct GraphicsStages {
    bool vertex = false;               // one vertex stage ...
    StageSource source = {};           // ... named by this
};

// The fields of a VkGraphicsPipelineCreateInfo after its chain: a complete
// pipeline with no layout and no render pass, whose rasterization state discards
// every primitive, so that the runtime needs no fragment or viewport state to
// accept it and reads only what is given.
inline void encodeGraphicsSelf(Stream& s, const GraphicsStages& stages)
{
    s.u32(0);                                   // flags
    if (stages.vertex) {
        s.u32(1); s.arraySize(1);
        encodeStage(s, VK_SHADER_STAGE_VERTEX_BIT, stages.source);
    } else {
        s.u32(0); s.arraySize(0);
    }
    s.ptr(false);                               // pVertexInputState
    s.ptr(false);                               // pInputAssemblyState
    s.ptr(false);                               // pTessellationState
    s.ptr(false);                               // pViewportState
    s.ptr(true);                                // pRasterizationState
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO); s.ptr(false);
    s.u32(0);                                   // flags
    s.u32(VK_FALSE);                            // depthClampEnable
    s.u32(VK_TRUE);                             // rasterizerDiscardEnable
    s.u32(VK_POLYGON_MODE_FILL);
    s.u32(VK_CULL_MODE_NONE);
    s.u32(VK_FRONT_FACE_COUNTER_CLOCKWISE);
    s.u32(VK_FALSE);                            // depthBiasEnable
    s.f32(0.0F); s.f32(0.0F); s.f32(0.0F);      // depth bias
    s.f32(1.0F);                                // lineWidth
    s.ptr(false);                               // pMultisampleState
    s.ptr(false);                               // pDepthStencilState
    s.ptr(false);                               // pColorBlendState
    s.ptr(false);                               // pDynamicState
    s.u64(0);                                   // layout
    s.u64(0);                                   // renderPass
    s.u32(0);                                   // subpass
    s.u64(0);                                   // basePipelineHandle
    s.u32(0);                                   // basePipelineIndex
}

// A VkGraphicsPipelineCreateInfo.
inline void encodeGraphicsInfo(Stream& s, const GraphicsStages& stages,
    const PipelineChain& chain = {})
{
    s.u32(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    encodePipelineChain(s, chain);
    encodeGraphicsSelf(s, stages);
}

} // namespace uml::gpu::testing::venus
