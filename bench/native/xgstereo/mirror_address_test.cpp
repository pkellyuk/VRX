// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// GPU-write isolation through reflected physical pointers in push constants.
#include "render_support.h"
#include "push_pointers.h"
#ifdef XG_VALIDATE_SPIRV
#include <spirv-tools/libspirv.hpp>
#endif
using namespace xg::render_test;
static std::vector<uint32_t> Compute(bool integer=false) {
    using xg::Emit;
    std::vector<uint32_t> w{spv::MagicNumber,0x00010300,0,22,0};
    Emit(w,spv::OpCapability,{spv::CapabilityShader});
    if(integer) Emit(w,spv::OpCapability,{spv::CapabilityInt64});
    Emit(w,spv::OpCapability,{spv::CapabilityPhysicalStorageBufferAddresses});
    const char extension[]="SPV_KHR_physical_storage_buffer";
    std::vector<uint32_t> str((sizeof(extension)+3)/4,0); memcpy(str.data(),extension,sizeof(extension));
    w.push_back((uint32_t(str.size()+1)<<16)|spv::OpExtension); w.insert(w.end(),str.begin(),str.end());
    Emit(w,spv::OpMemoryModel,{spv::AddressingModelPhysicalStorageBuffer64,spv::MemoryModelGLSL450});
    xg::test::Name(w,spv::ExecutionModelGLCompute,13,{});
    Emit(w,spv::OpExecutionMode,{13,spv::ExecutionModeLocalSize,1,1,1});
    Emit(w,spv::OpDecorate,{4,spv::DecorationBlock});
    Emit(w,spv::OpMemberDecorate,{4,0,spv::DecorationOffset,0});
    Emit(w,spv::OpDecorate,{6,spv::DecorationBlock});
    Emit(w,spv::OpMemberDecorate,{6,0,spv::DecorationOffset,0});
    Emit(w,spv::OpTypeVoid,{1}); Emit(w,spv::OpTypeFunction,{2,1});
    Emit(w,spv::OpTypeInt,{3,32,0}); Emit(w,spv::OpTypeStruct,{4,3});
    Emit(w,spv::OpTypePointer,{5,spv::StorageClassPhysicalStorageBuffer,4});
    if(integer) Emit(w,spv::OpTypeInt,{20,64,0});
    Emit(w,spv::OpTypeStruct,{6,integer?20u:5u}); Emit(w,spv::OpTypePointer,{7,spv::StorageClassPushConstant,6});
    Emit(w,spv::OpVariable,{7,8,spv::StorageClassPushConstant});
    Emit(w,spv::OpTypePointer,{9,spv::StorageClassPushConstant,integer?20u:5u});
    Emit(w,spv::OpTypePointer,{10,spv::StorageClassPhysicalStorageBuffer,3});
    Emit(w,spv::OpConstant,{3,11,0}); Emit(w,spv::OpConstant,{3,12,1});
    Emit(w,spv::OpFunction,{1,13,spv::FunctionControlMaskNone,2}); Emit(w,spv::OpLabel,{14});
    Emit(w,spv::OpAccessChain,{9,15,8,11}); Emit(w,spv::OpLoad,{integer?20u:5u,16,15});
    if(integer) Emit(w,spv::OpConvertUToPtr,{5,21,16});
    Emit(w,spv::OpAccessChain,{10,17,integer?21u:16u,11});
    Emit(w,spv::OpLoad,{3,18,17,spv::MemoryAccessAlignedMask,4});
    Emit(w,spv::OpIAdd,{3,19,18,12});
    Emit(w,spv::OpStore,{17,19,spv::MemoryAccessAlignedMask,4});
    Emit(w,spv::OpReturn,{}); Emit(w,spv::OpFunctionEnd,{}); return w;
}
int main(int argc,char** argv) {
    try {
        const bool integer=argc==2 && std::string(argv[1])=="--integer";
        auto code=Compute(integer);
#ifdef XG_VALIDATE_SPIRV
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
        validator.SetMessageConsumer([](spv_message_level_t,const char*,const spv_position_t&,const char* text){std::fprintf(stderr,"%s\n",text);});
        Require(validator.Validate(code),"valid typed physical pointer compute shader");
#endif
        auto reflected=xg::ReflectPushPointers(code.data(),code.size());
        Require(reflected.supported && reflected.offsets==std::vector<uint32_t>{0},"reflect only physical pointer at byte zero");
        SetEnvironmentVariableA("VRX_XG_EYE","mirror"); SetEnvironmentVariableA("VRX_XG_SHIFT","0.125");
        SetEnvironmentVariableA("VRX_XG_TRANSPORT","gpu");
        const auto session=L"address-"+std::to_wstring(GetCurrentProcessId()); SetEnvironmentVariableW(L"VRX_XG_SESSION",session.c_str());
        SetEnvironmentVariableW(L"VRX_XG_TUNING",nullptr);
        Gpu gpu; gpu.Init(true,1,false,false,true,integer);
#define FN(f) auto f=reinterpret_cast<PFN_##f>(gpu.getDeviceProc(gpu.device,#f)); Require(f!=nullptr,#f)
        FN(vkGetBufferDeviceAddress); FN(vkCreateComputePipelines); FN(vkCmdPushConstants); FN(vkCmdPushConstants2KHR); FN(vkCmdDispatch);
#undef FN
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=8;
        bi.usage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer buffer{}; Vk(gpu.vkCreateBuffer(gpu.device,&bi,nullptr,&buffer),"create pointer target");
        VkMemoryRequirements needs{}; gpu.vkGetBufferMemoryRequirements(gpu.device,buffer,&needs);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.pNext=&flags; ai.allocationSize=needs.size;
        ai.memoryTypeIndex=gpu.MemoryType(needs.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        VkDeviceMemory memory{}; Vk(gpu.vkAllocateMemory(gpu.device,&ai,nullptr,&memory),"allocate pointer target");
        Vk(gpu.vkBindBufferMemory(gpu.device,buffer,memory,0),"bind pointer target");
        void* mapped{}; Vk(gpu.vkMapMemory(gpu.device,memory,0,VK_WHOLE_SIZE,0,&mapped),"map pointer target");
        ((uint32_t*)mapped)[0]=0; ((uint32_t*)mapped)[1]=0; gpu.vkUnmapMemory(gpu.device,memory);
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; addressInfo.buffer=buffer;
        const auto address=vkGetBufferDeviceAddress(gpu.device,&addressInfo); Require(address!=0,"app sees real address");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize=code.size()*4; si.pCode=code.data();
        VkShaderModule shader{}; Vk(gpu.vkCreateShaderModule(gpu.device,&si,nullptr,&shader),"create compute shader");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.pushConstantRangeCount=1; li.pPushConstantRanges=&range;
        VkPipelineLayout layout{}; Vk(gpu.vkCreatePipelineLayout(gpu.device,&li,nullptr,&layout),"compute layout");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout=layout;
        ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=shader; ci.stage.pName="main";
        VkPipeline pipeline{}; Vk(vkCreateComputePipelines(gpu.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline),"compute pipeline");
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex=gpu.family; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool{}; Vk(gpu.vkCreateCommandPool(gpu.device,&pi,nullptr,&pool),"compute pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.commandBufferCount=1; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer cb{}; Vk(gpu.vkAllocateCommandBuffers(gpu.device,&ca,&cb),"compute commands");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; Vk(gpu.vkCreateFence(gpu.device,&fi,nullptr,&fence),"compute fence");
        for (uint32_t expected=1;expected<=3;++expected) {
            if (expected>1) { Vk(gpu.vkResetFences(gpu.device,1,&fence),"reset compute fence"); Vk(gpu.vkResetCommandBuffer(cb,0),"reset compute commands"); }
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Vk(gpu.vkBeginCommandBuffer(cb,&begin),"begin compute commands");
            gpu.vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            // App bytes must remain unchanged, including the raw address value.
            auto pushed=address+(integer?4:0);
            if (expected&1) vkCmdPushConstants(cb,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,&pushed);
            else { VkPushConstantsInfo info{VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO}; info.layout=layout; info.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; info.size=8; info.pValues=&pushed; vkCmdPushConstants2KHR(cb,&info); }
            Require(pushed==address+(integer?4:0),"push translation leaves app memory unchanged");
            vkCmdDispatch(cb,1,1,1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            gpu.vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            Vk(gpu.vkEndCommandBuffer(cb),"end compute commands");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
            Vk(gpu.vkQueueSubmit(gpu.queue,1,&submit,fence),"submit typed pointer dispatch");
            Vk(gpu.vkWaitForFences(gpu.device,1,&fence,VK_TRUE,5000000000ull),"both eyes complete");
            Vk(gpu.vkMapMemory(gpu.device,memory,0,VK_WHOLE_SIZE,0,&mapped),"read pointer result");
            const auto actual=((uint32_t*)mapped)[integer?1:0]; gpu.vkUnmapMemory(gpu.device,memory);
            std::printf("GPU pointer isolation dispatch %u: A=%u (shared target would be %u)\n",expected,actual,expected*2);
            Require(actual==expected,"B writes its own resource; cannot double-write A through a raw address");
        }
        gpu.vkDestroyFence(gpu.device,fence,nullptr); gpu.vkDestroyCommandPool(gpu.device,pool,nullptr);
        gpu.vkDestroyPipeline(gpu.device,pipeline,nullptr); gpu.vkDestroyPipelineLayout(gpu.device,layout,nullptr); gpu.vkDestroyShaderModule(gpu.device,shader,nullptr);
        gpu.vkDestroyBuffer(gpu.device,buffer,nullptr); gpu.vkFreeMemory(gpu.device,memory,nullptr);
        std::puts("PASS: typed GPU pointers and independent compute history, legacy/core-alias push constants"); return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
