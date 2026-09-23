#include "vulkan_room.h"
#include "stereo_warp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace vrx {
namespace {
void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
uint32_t MemoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No suitable Vulkan room memory type");
}
void QuatRows(const XrQuaternionf& q, float m[3][3]) {
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    m[0][0]=1-2*(y*y+z*z); m[0][1]=2*(x*y-z*w); m[0][2]=2*(x*z+y*w);
    m[1][0]=2*(x*y+z*w); m[1][1]=1-2*(x*x+z*z); m[1][2]=2*(y*z-x*w);
    m[2][0]=2*(x*z-y*w); m[2][1]=2*(y*z+x*w); m[2][2]=1-2*(x*x+y*y);
}
struct EmitParams {
    uint32_t srcW,srcH,stride,gridX,gridY,glowW,glowH,blocksX,blocksY,glowBlock,emitterCount,glowOn;
    float alpha; uint32_t pad[3]; float lightL[4];
};
static_assert(sizeof(EmitParams)==80);
struct LightParams { float bounds[4],geometry[4],shading[4],worldCount[4]; };
static_assert(sizeof(LightParams)==64);
}

void VulkanRoom::CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage) {
    out.size=size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size=size; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_,&bi,nullptr,&out.handle),"vkCreateBuffer room");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device_,out.handle,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=req.size;
    ai.memoryTypeIndex=MemoryType(gpu_,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device_,&ai,nullptr,&out.memory),"vkAllocateMemory room buffer");
    Check(vkBindBufferMemory(device_,out.handle,out.memory,0),"vkBindBufferMemory room");
    Check(vkMapMemory(device_,out.memory,0,size,0,&out.mapped),"vkMapMemory room");
}
void VulkanRoom::DestroyBuffer(Buffer& b) {
    if(b.mapped) vkUnmapMemory(device_,b.memory);
    if(b.handle) vkDestroyBuffer(device_,b.handle,nullptr);
    if(b.memory) vkFreeMemory(device_,b.memory,nullptr);
}
void VulkanRoom::CreateImage(Image& out,uint32_t width,uint32_t height,uint32_t layers,
                             VkFormat format,VkImageUsageFlags usage,VkImageViewType type) {
    out.width=width;out.height=height;out.layers=layers;out.format=format;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType=VK_IMAGE_TYPE_2D; ii.format=format; ii.extent={width,height,1};
    ii.mipLevels=1;ii.arrayLayers=layers;ii.samples=VK_SAMPLE_COUNT_1_BIT;
    ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=usage;ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateImage(device_,&ii,nullptr,&out.handle),"vkCreateImage room");
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device_,out.handle,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(gpu_,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_,&ai,nullptr,&out.memory),"vkAllocateMemory room image");
    Check(vkBindImageMemory(device_,out.handle,out.memory,0),"vkBindImageMemory room");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=out.handle;vi.viewType=type;vi.format=format;
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount=1;vi.subresourceRange.layerCount=layers;
    Check(vkCreateImageView(device_,&vi,nullptr,&out.view),"vkCreateImageView room");
}
void VulkanRoom::DestroyImage(Image& i) {
    if(i.view)vkDestroyImageView(device_,i.view,nullptr);
    if(i.handle)vkDestroyImage(device_,i.handle,nullptr);
    if(i.memory)vkFreeMemory(device_,i.memory,nullptr);
}
VkPipeline VulkanRoom::CreatePipeline(const char* path,VkPipelineLayout layout) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error(std::string("Cannot open room SPIR-V: ")+path);
    const auto bytes=file.tellg();
    if(bytes<=0||bytes%4)throw std::runtime_error("Invalid room SPIR-V size");
    std::vector<uint32_t> code(size_t(bytes)/4);
    file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);
    if(!file)throw std::runtime_error("Cannot read room SPIR-V");
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize=size_t(bytes);mi.pCode=code.data();
    VkShaderModule module=VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device_,&mi,nullptr,&module),"vkCreateShaderModule room");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
    ci.layout=layout;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkResult result=vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
    vkDestroyShaderModule(device_,module,nullptr);
    Check(result,"vkCreateComputePipelines room");
    return pipeline;
}
void VulkanRoom::Transition(VkCommandBuffer cmd,const Image& image,VkImageLayout oldLayout,VkImageLayout newLayout,
                            VkAccessFlags srcAccess,VkAccessFlags dstAccess,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout=oldLayout;b.newLayout=newLayout;b.srcAccessMask=srcAccess;b.dstAccessMask=dstAccess;
    b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=image.handle;b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount=1;b.subresourceRange.layerCount=image.layers;
    vkCmdPipelineBarrier(cmd,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&b);
}

VulkanRoom::VulkanRoom(VkPhysicalDevice gpu,VkDevice device,VkBuffer source,VkBuffer stereo,VkFormat screenFormat,
                       uint32_t colorWidth,uint32_t colorHeight,uint32_t eyeWidth,uint32_t eyeHeight)
    :gpu_(gpu),device_(device),source_(source),stereo_(stereo),screenFormat_(screenFormat),
     colorWidth_(colorWidth),colorHeight_(colorHeight),eyeWidth_(eyeWidth),eyeHeight_(eyeHeight) {
    if(screenFormat!=VK_FORMAT_R8G8B8A8_SRGB&&screenFormat!=VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("Room needs an RGBA OpenXR swapchain format");
    CreateBuffer(decode_,256*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    CreateBuffer(emitter_,kRoomMaxEmitters*sizeof(RoomEmitter),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    CreateBuffer(glowBuffer_,GlowWidth*GlowHeight*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CreateBuffer(mirrorBuffer_,kRoomMirrorW*kRoomMirrorMaxH*4*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CreateBuffer(lightBuffer_,kRoomFaces*kRoomLightmap*kRoomLightmap*4*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CreateBuffer(curveBuffer_,sizeof(CurveConstants),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    CreateBuffer(roomBuffer_,sizeof(RoomConstants),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    CreateBuffer(readback_,VkDeviceSize(eyeWidth_)*eyeHeight_*2*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    RoomDecodeTable(static_cast<float*>(decode_.mapped));
    const auto sampled=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CreateImage(picture_,colorWidth_,colorHeight_,2,VK_FORMAT_R8G8B8A8_UNORM,sampled,VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    CreateImage(glow_,GlowWidth,GlowHeight,1,VK_FORMAT_R8G8B8A8_UNORM,sampled,VK_IMAGE_VIEW_TYPE_2D);
    CreateImage(mirror_,kRoomMirrorW,kRoomMirrorMaxH,1,VK_FORMAT_R32G32B32A32_SFLOAT,sampled,VK_IMAGE_VIEW_TYPE_2D);
    CreateImage(light_,kRoomLightmap,kRoomLightmap,kRoomFaces,VK_FORMAT_R32G32B32A32_SFLOAT,sampled,VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    CreateImage(eye_,eyeWidth_,eyeHeight_,2,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter=VK_FILTER_LINEAR;si.minFilter=VK_FILTER_LINEAR;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod=1.0f;
    Check(vkCreateSampler(device_,&si,nullptr,&sampler_),"vkCreateSampler room");
    VkDescriptorSetLayoutBinding pb[5]{};
    for(uint32_t i=0;i<5;i++){pb[i].binding=i;pb[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;pb[i].descriptorCount=1;pb[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount=5;li.pBindings=pb;
    Check(vkCreateDescriptorSetLayout(device_,&li,nullptr,&passLayout_),"vkCreateDescriptorSetLayout room pass");
    VkDescriptorSetLayoutBinding eb[8]{};
    for(uint32_t i=0;i<8;i++){eb[i].binding=i;eb[i].descriptorCount=1;eb[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        eb[i].descriptorType=i<4?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:(i==4?VK_DESCRIPTOR_TYPE_SAMPLER:(i==5?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));}
    li.bindingCount=8;li.pBindings=eb;
    Check(vkCreateDescriptorSetLayout(device_,&li,nullptr,&eyeLayout_),"vkCreateDescriptorSetLayout room eye");
    VkDescriptorPoolSize sizes[5]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,10},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets=3;pi.poolSizeCount=5;pi.pPoolSizes=sizes;
    Check(vkCreateDescriptorPool(device_,&pi,nullptr,&descriptorPool_),"vkCreateDescriptorPool room");
    VkDescriptorSetLayout layouts[3]={passLayout_,passLayout_,eyeLayout_};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=descriptorPool_;ai.descriptorSetCount=3;ai.pSetLayouts=layouts;
    VkDescriptorSet sets[3]{};
    Check(vkAllocateDescriptorSets(device_,&ai,sets),"vkAllocateDescriptorSets room");
    passSet_=sets[0];mirrorSet_=sets[1];eyeSet_=sets[2];
    VkDescriptorBufferInfo passInfos[5]={{source_,0,VK_WHOLE_SIZE},{decode_.handle,0,VK_WHOLE_SIZE},
        {emitter_.handle,0,VK_WHOLE_SIZE},{glowBuffer_.handle,0,VK_WHOLE_SIZE},{lightBuffer_.handle,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[18]{};
    for(uint32_t i=0;i<5;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=passSet_;writes[i].dstBinding=i;
        writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&passInfos[i];}
    VkDescriptorBufferInfo mirrorInfos[5];
    std::copy(std::begin(passInfos),std::end(passInfos),std::begin(mirrorInfos));
    mirrorInfos[2]={mirrorBuffer_.handle,0,VK_WHOLE_SIZE};
    for(uint32_t i=0;i<5;i++){auto& w=writes[13+i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet=mirrorSet_;w.dstBinding=i;w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&mirrorInfos[i];}
    VkDescriptorImageInfo imageInfos[6]{};
    Image* sampledImages[4]={&picture_,&glow_,&light_,&mirror_};
    for(uint32_t i=0;i<4;i++){imageInfos[i].imageView=sampledImages[i]->view;imageInfos[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
    imageInfos[4].sampler=sampler_;imageInfos[5].imageView=eye_.view;imageInfos[5].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo uniforms[2]={{curveBuffer_.handle,0,sizeof(CurveConstants)},{roomBuffer_.handle,0,sizeof(RoomConstants)}};
    for(uint32_t i=0;i<8;i++){auto& w=writes[5+i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=eyeSet_;w.dstBinding=i;
        w.descriptorCount=1;w.descriptorType=eb[i].descriptorType;
        if(i<6)w.pImageInfo=&imageInfos[i];else w.pBufferInfo=&uniforms[i-6];}
    vkUpdateDescriptorSets(device_,18,writes,0,nullptr);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(EmitParams)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1;pli.pSetLayouts=&passLayout_;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
    Check(vkCreatePipelineLayout(device_,&pli,nullptr,&passPipelineLayout_),"vkCreatePipelineLayout room pass");
    pli.pSetLayouts=&eyeLayout_;pli.pushConstantRangeCount=0;pli.pPushConstantRanges=nullptr;
    Check(vkCreatePipelineLayout(device_,&pli,nullptr,&eyePipelineLayout_),"vkCreatePipelineLayout room eye");
    emitPipeline_=CreatePipeline(VRX_ROOM_EMIT_SPV_PATH,passPipelineLayout_);
    mirrorPipeline_=CreatePipeline(VRX_ROOM_MIRROR_SPV_PATH,passPipelineLayout_);
    lightPipeline_=CreatePipeline(VRX_ROOM_LIGHT_SPV_PATH,passPipelineLayout_);
    eyePipeline_=CreatePipeline(VRX_ROOM_EYE_SPV_PATH,eyePipelineLayout_);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_,&properties);
    timestampPeriod_=properties.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=7;
    Check(vkCreateQueryPool(device_,&qi,nullptr,&timingQueries_),"vkCreateQueryPool room timing");
}
VulkanRoom::~VulkanRoom(){
    if(timingQueries_)vkDestroyQueryPool(device_,timingQueries_,nullptr);
    for(auto p:{eyePipeline_,lightPipeline_,mirrorPipeline_,emitPipeline_})if(p)vkDestroyPipeline(device_,p,nullptr);
    if(eyePipelineLayout_)vkDestroyPipelineLayout(device_,eyePipelineLayout_,nullptr);
    if(passPipelineLayout_)vkDestroyPipelineLayout(device_,passPipelineLayout_,nullptr);
    if(descriptorPool_)vkDestroyDescriptorPool(device_,descriptorPool_,nullptr);
    if(eyeLayout_)vkDestroyDescriptorSetLayout(device_,eyeLayout_,nullptr);
    if(passLayout_)vkDestroyDescriptorSetLayout(device_,passLayout_,nullptr);
    if(sampler_)vkDestroySampler(device_,sampler_,nullptr);
    for(Image* i:{&eye_,&light_,&mirror_,&glow_,&picture_})DestroyImage(*i);
    for(Buffer* b:{&readback_,&roomBuffer_,&curveBuffer_,&lightBuffer_,&mirrorBuffer_,&glowBuffer_,&emitter_,&decode_})DestroyBuffer(*b);
}

void VulkanRoom::Prepare(const uint32_t* glowSource,uint32_t glowWidth,uint32_t glowHeight,const LiveSettings& settings,
                         const XrView eyes[2],float floorLocalY) {
    if(!glowSource||!glowWidth||!glowHeight)throw std::runtime_error("Room glow source is empty");
    const float W=settings.width,H=W*float(colorHeight_)/colorWidth_;
    if (W != lastWidth_ || H != lastHeight_) glowHistoryValid_ = false;
    lastWidth_ = W; lastHeight_ = H;
    // The fixed screen is positioned relative to LOCAL origin, so its room
    // geometry uses that same origin. Eye poses vary only the traced rays.
    RoomInputs input;
    input.W=W;input.H=H;
    input.eye[0]=-settings.horizontal;
    input.eye[1]=-settings.height;
    input.eye[2]=settings.distance;
    input.floorY=std::isfinite(floorLocalY)?floorLocalY-settings.height:floorLocalY;
    const float key[6]={W,H,input.eye[0],input.eye[1],input.eye[2],
                        std::isfinite(input.floorY)?input.floorY:-999.0f};
    bool geometryChanged=!room_.valid;
    for(int i=0;i<6;i++)if(std::fabs(key[i]-geometryKey_[i])>1e-4f)geometryChanged=true;
    if(geometryChanged){
        if(!BuildRoom(input,room_))throw std::runtime_error("Room geometry invalid for anchored eye position");
        layout_=RoomLayout(W,H,GlowWidth,GlowHeight);
        std::vector<RoomEmitter> emitters;
        const float glowHalfW=0.5f*W+kAmbiMargin*W;
        const float glowHalfH=0.5f*H+kAmbiMargin*W;
        if(!BuildRoomEmitters(room_,Cylinder(),W,H,glowHalfW,glowHalfH,layout_,emitters))
            throw std::runtime_error("Room emitter geometry failed");
        std::memcpy(emitter_.mapped,emitters.data(),emitters.size()*sizeof(RoomEmitter));
        std::copy_n(key,6,geometryKey_);
    }
    RoomView view;view.flatLayer=true;view.W=W;view.H=H;view.glowOn=true;
    view.glowHalfW=0.5f*W+kAmbiMargin*W;view.glowHalfH=0.5f*H+kAmbiMargin*W;
    const auto now=std::chrono::steady_clock::now();
    const float dt=lastPrepare_==std::chrono::steady_clock::time_point{}?0.0f:
        std::clamp(std::chrono::duration<float>(now-lastPrepare_).count(),0.0f,0.25f);
    const float alpha=geometryChanged?1.0f:1.0f-std::exp(-dt/kRoomLightTau);
    lastPrepare_=now;
    RoomLook look;look.glass=settings.glass;look.reflect=settings.reflect;look.light=settings.light;look.lightRgb=settings.lightRgb;
    shading_=MakeRoomShading(room_,settings.room,0,W*H,look);
    roomConstants_=MakeRoomConstants(room_,shading_,layout_,view,int(colorWidth_),int(colorHeight_),alpha);
    mirrorW_=roomConstants_.mirrorW;mirrorH_=roomConstants_.mirrorH;
    std::memcpy(roomBuffer_.mapped,&roomConstants_,sizeof(roomConstants_));
    curveConstants_={};curveConstants_.ew=eyeWidth_;curveConstants_.eh=eyeHeight_;
    curveConstants_.glowOn=1;curveConstants_.linearBlend=1;
    curveConstants_.glowHalfW=view.glowHalfW;curveConstants_.glowHalfH=view.glowHalfH;
    curveConstants_.world[3]=1.0f;
    for(int e=0;e<2;e++){
        auto& v=curveConstants_.eye[e];
        v.origin[0]=eyes[e].pose.position.x-settings.horizontal;
        v.origin[1]=eyes[e].pose.position.y-settings.height;
        v.origin[2]=eyes[e].pose.position.z+settings.distance;
        float m[3][3];QuatRows(eyes[e].pose.orientation,m);
        for(int k=0;k<3;k++){v.row0[k]=m[0][k];v.row1[k]=m[1][k];v.row2[k]=m[2][k];}
        v.tanL=std::tan(eyes[e].fov.angleLeft);v.tanR=std::tan(eyes[e].fov.angleRight);
        v.tanU=std::tan(eyes[e].fov.angleUp);v.tanD=std::tan(eyes[e].fov.angleDown);
    }
    std::memcpy(curveBuffer_.mapped,&curveConstants_,sizeof(curveConstants_));
    AmbiConstants ambi{};
    ambi.gw=GlowWidth;ambi.gh=GlowHeight;
    ambi.srcW=glowWidth;ambi.srcH=glowHeight;
    ambi.screenW=W;ambi.screenH=H;
    ambi.marginM=kAmbiMargin*W;
    ambi.rectW=W+2.0f*ambi.marginM;ambi.rectH=H+2.0f*ambi.marginM;
    ambi.intensity=kAmbiDefaultStrength/100.0f;
    ambi.soft=kAmbiSoft*W;ambi.bezel=kAmbiBezel;ambi.ringN=kAmbiRing;
    ambi.linearBlend=1;
    std::vector<unsigned char> reference(size_t(GlowWidth)*GlowHeight*4);
    if(!AmbilightReference(ambi,reinterpret_cast<const unsigned char*>(glowSource),int(glowWidth)*4,reference.data(),GlowWidth*4))
        throw std::runtime_error("Ambilight reference failed");
    // The glow's temporal blend keeps float history in CPU memory: reading it
    // back from the (uncached) GPU buffer each frame was slow, and 8-bit
    // history stopped converging within a few levels of the target.
    glowHistory_.resize(reference.size());
    std::vector<unsigned char> glow(reference.size());
    for(size_t i=0;i<reference.size();i++){
        const float target=float(reference[i]);
        glowHistory_[i]=glowHistoryValid_?glowHistory_[i]+(target-glowHistory_[i])*kAmbiBlend:target;
        glow[i]=static_cast<unsigned char>(glowHistory_[i]+0.5f);
    }
    glowHistoryValid_=true;
    std::memcpy(glowBuffer_.mapped,glow.data(),glow.size());
}
void VulkanRoom::Record(VkCommandBuffer cmd,VkImage destination) {
    vkCmdResetQueryPool(cmd,timingQueries_,0,7);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timingQueries_,0);
    EmitParams ep{};
    ep.srcW=colorWidth_;ep.srcH=colorHeight_;ep.stride=RoomStride(int(colorWidth_));
    ep.gridX=layout_.gridX;ep.gridY=layout_.gridY;ep.glowW=GlowWidth;ep.glowH=GlowHeight;
    ep.blocksX=layout_.blocksX;ep.blocksY=layout_.blocksY;ep.glowBlock=layout_.block;
    ep.emitterCount=layout_.count();ep.glowOn=1;ep.alpha=roomConstants_.alpha;
    std::copy_n(shading_.lightL,3,ep.lightL);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,passPipelineLayout_,0,1,&passSet_,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,emitPipeline_);
    vkCmdPushConstants(cmd,passPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ep),&ep);
    vkCmdDispatch(cmd,ep.emitterCount,1,1);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,timingQueries_,1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,1,&mb,0,nullptr,0,nullptr);
    if(mirrorW_&&mirrorH_){
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,passPipelineLayout_,0,1,&mirrorSet_,0,nullptr);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mirrorPipeline_);
        uint32_t mirrorParams[5]={colorWidth_,colorHeight_,mirrorW_,mirrorH_,uint32_t(RoomStride(int(colorWidth_)))};
        vkCmdPushConstants(cmd,passPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(mirrorParams),mirrorParams);
        vkCmdDispatch(cmd,(mirrorW_+7)/8,(mirrorH_+7)/8,1);
    }
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,timingQueries_,2);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,passPipelineLayout_,0,1,&passSet_,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,lightPipeline_);
    LightParams lp{};
    lp.bounds[0]=room_.X;lp.bounds[1]=room_.yF;lp.bounds[2]=room_.yC;lp.bounds[3]=room_.zB;
    lp.geometry[0]=room_.g;lp.geometry[1]=room_.zSide;
    lp.geometry[2]=kRoomShadeFloor;lp.geometry[3]=kRoomShadeCeiling;
    lp.shading[0]=shading_.rhoWall;lp.shading[1]=shading_.rhoFloor;
    lp.shading[2]=shading_.rhoCeiling;lp.shading[3]=RoomBounceScale(shading_);
    std::copy_n(shading_.world,3,lp.worldCount);lp.worldCount[3]=float(layout_.count());
    vkCmdPushConstants(cmd,passPipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(lp),&lp);
    vkCmdDispatch(cmd,kRoomLightmap/8,kRoomLightmap/8,kRoomFaces);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,timingQueries_,3);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&mb,0,nullptr,0,nullptr);
    for(Image* i:{&picture_,&glow_,&mirror_,&light_})
        Transition(cmd,*i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy pictureCopies[2]{};
    for(int e=0;e<2;e++){
        auto& c=pictureCopies[e];c.bufferOffset=VkDeviceSize(e)*colorWidth_*colorHeight_*4;
        c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;c.imageSubresource.mipLevel=0;
        c.imageSubresource.baseArrayLayer=e;c.imageSubresource.layerCount=1;
        c.imageExtent={colorWidth_,colorHeight_,1};
    }
    vkCmdCopyBufferToImage(cmd,stereo_,picture_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,pictureCopies);
    VkBufferImageCopy copy{};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount=1;copy.imageExtent={GlowWidth,GlowHeight,1};
    vkCmdCopyBufferToImage(cmd,glowBuffer_.handle,glow_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    if(mirrorW_&&mirrorH_){
        copy.imageExtent={mirrorW_,mirrorH_,1};
        vkCmdCopyBufferToImage(cmd,mirrorBuffer_.handle,mirror_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    }
    copy.imageExtent={kRoomLightmap,kRoomLightmap,1};copy.imageSubresource.layerCount=kRoomFaces;
    vkCmdCopyBufferToImage(cmd,lightBuffer_.handle,light_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    for(Image* i:{&picture_,&glow_,&mirror_,&light_})
        Transition(cmd,*i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,timingQueries_,4);
    Transition(cmd,eye_,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,eyePipeline_);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,eyePipelineLayout_,0,1,&eyeSet_,0,nullptr);
    vkCmdDispatch(cmd,(eyeWidth_+7)/8,(eyeHeight_+7)/8,2);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,timingQueries_,5);
    Transition(cmd,eye_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    if(capture_){
        VkBufferImageCopy copies[2]{};
        for(int e=0;e<2;e++){
            copies[e].bufferOffset=VkDeviceSize(e)*eyeWidth_*eyeHeight_*4;
            copies[e].imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            copies[e].imageSubresource.baseArrayLayer=e;
            copies[e].imageSubresource.layerCount=1;
            copies[e].imageExtent={eyeWidth_,eyeHeight_,1};
        }
        vkCmdCopyImageToBuffer(cmd,eye_.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_.handle,2,copies);
        VkBufferMemoryBarrier readbackBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        readbackBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        readbackBarrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        readbackBarrier.srcQueueFamilyIndex=readbackBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        readbackBarrier.buffer=readback_.handle;readbackBarrier.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                             0,0,nullptr,1,&readbackBarrier,0,nullptr);
        capture_=false;
    }
    VkImageMemoryBarrier target{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    target.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;target.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    target.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    target.srcQueueFamilyIndex=target.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    target.image=destination;target.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    target.subresourceRange.levelCount=1;target.subresourceRange.layerCount=2;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&target);
    VkImageCopy copies[2]{};
    for(int e=0;e<2;e++){
        copies[e].srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        copies[e].srcSubresource.baseArrayLayer=e;copies[e].srcSubresource.layerCount=1;
        copies[e].dstSubresource=copies[e].srcSubresource;
        copies[e].extent={eyeWidth_,eyeHeight_,1};
    }
    vkCmdCopyImage(cmd,eye_.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,copies);
    target.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    target.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;target.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0,0,nullptr,0,nullptr,1,&target);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timingQueries_,6);
}
void VulkanRoom::PrintTiming() const {
    uint64_t stamps[7]{};
    Check(vkGetQueryPoolResults(device_,timingQueries_,0,7,sizeof(stamps),stamps,sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"vkGetQueryPoolResults room");
    const char* names[6]={"emit","mirror","light","upload","eye","copy"};
    std::printf("Room GPU pass timing:");
    for(int i=0;i<6;i++)
        std::printf(" %s %.2f ms",names[i],double(stamps[i+1]-stamps[i])*timestampPeriod_/1e6);
    std::printf("; total %.2f ms\n",double(stamps[6]-stamps[0])*timestampPeriod_/1e6);
}
bool VulkanRoom::CompareReference() const {
    RoomLightmap lm;
    const size_t lightValues=size_t(kRoomFaces)*kRoomLightmap*kRoomLightmap*4;
    const float* gpuLight=static_cast<const float*>(lightBuffer_.mapped);
    lm.texels.assign(gpuLight,gpuLight+lightValues);
    RoomMirror mirror;
    mirror.w=int(mirrorW_);mirror.h=int(mirrorH_);
    const float* gpuMirror=static_cast<const float*>(mirrorBuffer_.mapped);
    mirror.texels.assign(gpuMirror,gpuMirror+size_t(mirrorW_)*mirrorH_*4);
    const RgbaImage picture{nullptr,int(colorWidth_),int(colorHeight_),int(colorWidth_)*4};
    const RgbaImage glow{static_cast<const unsigned char*>(glowBuffer_.mapped),int(GlowWidth),int(GlowHeight),int(GlowWidth)*4};
    RoomView view;view.flatLayer=true;view.W=roomConstants_.screenW;view.H=roomConstants_.screenH;
    view.glowOn=true;view.glowHalfW=roomConstants_.glowHalfW;view.glowHalfH=roomConstants_.glowHalfH;
    RoomEyeInputs inputs;inputs.rc=&roomConstants_;inputs.picture=&picture;inputs.glow=&glow;
    inputs.light=&lm;inputs.mirror=&mirror;inputs.room=&room_;
    const auto* actual=static_cast<const unsigned char*>(readback_.mapped);
    size_t checked=0,bad=0;int worst=0;
    for(int e=0;e<2;e++)for(uint32_t y=12;y<eyeHeight_;y+=24)for(uint32_t x=12;x<eyeWidth_;x+=24){
        float ref[3];
        if(!RoomPixel(curveConstants_,Cylinder(),room_,view,e,x,y,inputs,ref))
            throw std::runtime_error("CPU room eye reference failed");
        const auto* p=actual+(size_t(e)*eyeWidth_*eyeHeight_+y*eyeWidth_+x)*4;
        int error=0;
        for(int c=0;c<3;c++)error=std::max(error,std::abs(int(p[c])-int(std::lround(std::clamp(ref[c],0.0f,1.0f)*255.0f))));
        worst=std::max(worst,error);if(error>2)bad++;checked++;
    }
    std::printf("Vulkan/CPU room eye: %zu of %zu sampled pixels over 2 levels (worst %d)\n",bad,checked,worst);
    return bad<=checked/200;
}
void VulkanRoom::SaveCapture(const char* path) const {
    std::ofstream out(path,std::ios::binary);
    if(!out)throw std::runtime_error("Cannot write room capture");
    out << "P6\n" << eyeWidth_*2 << " " << eyeHeight_ << "\n255\n";
    const auto* bytes=static_cast<const unsigned char*>(readback_.mapped);
    for(uint32_t y=0;y<eyeHeight_;y++)for(int e=0;e<2;e++)
        for(uint32_t x=0;x<eyeWidth_;x++){
            const unsigned char* p=bytes+(size_t(e)*eyeWidth_*eyeHeight_+y*eyeWidth_+x)*4;
            out.write(reinterpret_cast<const char*>(p),3);
        }
    if(!out)throw std::runtime_error("Cannot finish room capture");
}
} // namespace vrx
