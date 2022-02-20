
#include "VulkanImpl.h"
#include <assert.h>
#include <sstream>
#include <vulkan/vulkan_win32.h>
#include <fstream>
#include <wrl.h>
#include <dxcapi.h>

//#include <dxcapi.use.h>
#include <Windows.h>


VulkanImpl* g_vulkanImpl = nullptr;

Microsoft::WRL::ComPtr<IDxcBlob> compileShaderLibrary(std::wstring dir, std::wstring  filename, std::wstring entryPoint, std::wstring target, std::optional<std::vector< DxcDefine>>  Defines)
{
    Microsoft::WRL::ComPtr<IDxcUtils> pUtils;
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(pUtils.GetAddressOf()));

    Microsoft::WRL::ComPtr<IDxcCompiler3> pCompiler;
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(pCompiler.GetAddressOf()));

    Microsoft::WRL::ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
    pUtils->CreateDefaultIncludeHandler(&dxcIncludeHandler);

    std::wstring Path = dir + filename;
    // Open and read the file
    std::ifstream shaderFile(Path);
    if (shaderFile.good() == false)
    {
        //msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
        return nullptr;
    }
    std::stringstream strStream;
    strStream << shaderFile.rdbuf();
    std::string shader = strStream.str();

    // Create blob from the string
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> pTextBlob;
    pUtils->CreateBlob((LPBYTE)shader.c_str(), (uint32_t)shader.size(), CP_UTF8, pTextBlob.GetAddressOf());
    // Compile

    std::vector<LPCWSTR> arguments;
    arguments.push_back(L"-spirv");

    arguments.push_back(L"-E");
    arguments.push_back(entryPoint.c_str());

    //-T for the target profile (eg. ps_6_2)
    arguments.push_back(L"-T");
    arguments.push_back(target.c_str());

    arguments.push_back(L"-I");
    arguments.push_back(dir.c_str());

    DxcDefine* defines = nullptr;
    UINT32 numDefines = 0;
    if (Defines.has_value())
    {
        for (auto& d : Defines.value())
        {
            arguments.push_back(L"-D");
            arguments.push_back(LPWSTR(d.Name));
        }
    }

    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = pTextBlob->GetBufferPointer();
    sourceBuffer.Size = pTextBlob->GetBufferSize();
    sourceBuffer.Encoding = 0;

    Microsoft::WRL::ComPtr<IDxcResult> pCompileResult;
    HRESULT hr = pCompiler->Compile(&sourceBuffer, arguments.data(), arguments.size(), dxcIncludeHandler.Get(), IID_PPV_ARGS(pCompileResult.GetAddressOf()));

    //Error Handling
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> pErrors;
    pCompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(pErrors.GetAddressOf()), nullptr);
    if (pErrors && pErrors->GetStringLength() > 0)
    {
        std::string log = (char*)pErrors->GetBufferPointer();

        std::string fileNameA;
        fileNameA.assign(filename.begin(), filename.end());
        std::stringstream ss;
        ss << fileNameA << " \n" << log;
        //msgBox("Compiler error:\n" + log);
        OutputDebugStringA(ss.str().c_str());
    }

    Microsoft::WRL::ComPtr<IDxcBlob> pShader;
    pCompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(pShader.GetAddressOf()), nullptr);
    if (pShader)
    {

        return pShader;

    }
    else
        return nullptr;
}

VKAPI_ATTR VkBool32 VKAPI_CALL dbgFunc(VkDebugReportFlagsEXT msgFlags, VkDebugReportObjectTypeEXT objType, uint64_t srcObject,
    size_t location, int32_t msgCode, const char* pLayerPrefix, const char* pMsg,
    void* pUserData) {
    //std::ostringstream message;
    std::stringstream message;

    if (msgFlags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
        message << "ERROR: ";
    }
    else if (msgFlags & VK_DEBUG_REPORT_WARNING_BIT_EXT) {
        message << "WARNING: ";
    }
    else if (msgFlags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) {
        message << "PERFORMANCE WARNING: ";
    }
    else if (msgFlags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) {
        message << "INFO: ";
    }
    else if (msgFlags & VK_DEBUG_REPORT_DEBUG_BIT_EXT) {
        message << "DEBUG: ";
    }
    message << "[" << pLayerPrefix << "] Code " << msgCode << " : " << pMsg << "\n";

    OutputDebugStringA(message.str().c_str());
    //std::cout << message.str() << std::endl;

    /*
     * false indicates that layer should not bail-out of an
     * API call that had validation failures. This may mean that the
     * app dies inside the driver due to invalid parameter(s).
     * That's what would happen without validation layers, so we'll
     * keep that behavior here.
     */
    return false;
}

bool memory_type_from_properties(VkPhysicalDeviceMemoryProperties memory_properties, uint32_t typeBits, VkFlags requirements_mask, uint32_t* typeIndex) {
    // Search memtypes to find first index with those properties
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((typeBits & 1) == 1) {
            // Type is available, does it match user properties?
            if ((memory_properties.memoryTypes[i].propertyFlags & requirements_mask) == requirements_mask) {
                *typeIndex = i;
                return true;
            }
        }
        typeBits >>= 1;
    }
    // No memory types matched, return failure
    return false;
}

VKTexture::~VKTexture()
{
    vkDestroyImageView(g_vulkanImpl->Device, view, NULL);
    vkDestroyImage(g_vulkanImpl->Device, image, NULL);
    vkFreeMemory(g_vulkanImpl->Device, mem, NULL);

    if(framebuffer)
        vkDestroyFramebuffer(g_vulkanImpl->Device, framebuffer, NULL);
}

bool VKTexture::Create(TextureCreateInfo* info)
{
    textureInfo = *info;
    VkResult res;

    VkImageCreateInfo image_info = {};
    
   /* VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(g_vulkanImpl->vecGPU[0], textureInfo.format, &props);
    if (props.linearTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        image_info.tiling = VK_IMAGE_TILING_LINEAR;
    }
    else if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    }
    else
    {
        assert(false);
    }*/

    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;

    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = NULL;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = textureInfo.format;
    image_info.extent.width = info->width;
    image_info.extent.height = info->height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.queueFamilyIndexCount = 0;
    image_info.pQueueFamilyIndices = NULL;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.flags = 0;

    /* Create image */
    res = vkCreateImage(g_vulkanImpl->Device, &image_info, NULL, &image);
    assert(res == VK_SUCCESS);

    VkMemoryAllocateInfo mem_alloc = {};
    mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem_alloc.pNext = NULL;
    mem_alloc.allocationSize = 0;
    mem_alloc.memoryTypeIndex = 0;

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(g_vulkanImpl->Device, image, &mem_reqs);

    mem_alloc.allocationSize = mem_reqs.size;

    vkGetPhysicalDeviceMemoryProperties(g_vulkanImpl->vecGPU[0], &g_vulkanImpl->memory_properties);

    bool pass = memory_type_from_properties(g_vulkanImpl->memory_properties, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_alloc.memoryTypeIndex);
    assert(pass);

    /* Allocate memory */
    res = vkAllocateMemory(g_vulkanImpl->Device, &mem_alloc, NULL, &mem);
    assert(res == VK_SUCCESS);

    /* Bind memory */
    res = vkBindImageMemory(g_vulkanImpl->Device, image, mem, 0);
    assert(res == VK_SUCCESS);

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = NULL;
    view_info.image = VK_NULL_HANDLE;
    view_info.format = textureInfo.format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_R;
    view_info.components.g = VK_COMPONENT_SWIZZLE_G;
    view_info.components.b = VK_COMPONENT_SWIZZLE_B;
    view_info.components.a = VK_COMPONENT_SWIZZLE_A;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.flags = 0;
    view_info.image = image;
    res = vkCreateImageView(g_vulkanImpl->Device, &view_info, NULL, &view);
    assert(res == VK_SUCCESS);

    if (res == VK_SUCCESS)
        return true;
    else
        return false;
}

bool VKTexture::CreateFromImage(VkImage img, TextureCreateInfo* info)
{
    textureInfo = *info;
    image = img;
    

    VkImageViewCreateInfo color_image_view = {};
    color_image_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    color_image_view.pNext = NULL;
    color_image_view.flags = 0;
    color_image_view.image = image;
    color_image_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    color_image_view.format = textureInfo.format;
    color_image_view.components.r = VK_COMPONENT_SWIZZLE_R;
    color_image_view.components.g = VK_COMPONENT_SWIZZLE_G;
    color_image_view.components.b = VK_COMPONENT_SWIZZLE_B;
    color_image_view.components.a = VK_COMPONENT_SWIZZLE_A;
    color_image_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_image_view.subresourceRange.baseMipLevel = 0;
    color_image_view.subresourceRange.levelCount = 1;
    color_image_view.subresourceRange.baseArrayLayer = 0;
    color_image_view.subresourceRange.layerCount = 1;

    VkResult res;
    res = vkCreateImageView(g_vulkanImpl->Device, &color_image_view, NULL, &view);
    assert(res == VK_SUCCESS);
    if (res == VK_SUCCESS)
        return true;
    else
        return false;
}

bool VKUniformBuffer::Create(UniformBufferCreateInfo* info)
{
    VkResult res;
    VkBufferCreateInfo buf_info = {};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.pNext = NULL;
    buf_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buf_info.size = info->size;
    buf_info.queueFamilyIndexCount = 0;
    buf_info.pQueueFamilyIndices = NULL;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buf_info.flags = 0;
    res = vkCreateBuffer(g_vulkanImpl->Device, &buf_info, NULL, &buffer);
    assert(res == VK_SUCCESS);

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(g_vulkanImpl->Device, buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.memoryTypeIndex = 0;

    alloc_info.allocationSize = mem_reqs.size;

    bool pass = memory_type_from_properties(g_vulkanImpl->memory_properties, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &alloc_info.memoryTypeIndex);
    assert(pass && "No mappable, coherent memory");

    res = vkAllocateMemory(g_vulkanImpl->Device, &alloc_info, NULL, &memory);
    assert(res == VK_SUCCESS);

    res = vkMapMemory(g_vulkanImpl->Device, memory, 0, mem_reqs.size, 0, (void**)&pData);
    assert(res == VK_SUCCESS);

    res = vkBindBufferMemory(g_vulkanImpl->Device, buffer, memory, 0);
    assert(res == VK_SUCCESS);

    buffer_info.buffer = buffer;
    buffer_info.offset = 0;
    buffer_info.range = info->size;

    if (res == VK_SUCCESS)
        return true;
    else
        return false;
}

VKUniformBuffer::~VKUniformBuffer()
{
    vkUnmapMemory(g_vulkanImpl->Device, memory);
    vkDestroyBuffer(g_vulkanImpl->Device, buffer, NULL);
    vkFreeMemory(g_vulkanImpl->Device, memory, NULL);
}


bool VKVertexBuffer::Create(VertexBufferInfo* info)
{
    VkResult res;
    VkBufferCreateInfo buf_info = {};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.pNext = NULL;
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buf_info.size = info->size;
    buf_info.queueFamilyIndexCount = 0;
    buf_info.pQueueFamilyIndices = NULL;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buf_info.flags = 0;
    res = vkCreateBuffer(g_vulkanImpl->Device, &buf_info, NULL, &buffer);
    assert(res == VK_SUCCESS);

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(g_vulkanImpl->Device, buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.memoryTypeIndex = 0;

    alloc_info.allocationSize = mem_reqs.size;

    bool pass = memory_type_from_properties(g_vulkanImpl->memory_properties, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &alloc_info.memoryTypeIndex);
    assert(pass && "No mappable, coherent memory");

    res = vkAllocateMemory(g_vulkanImpl->Device, &alloc_info, NULL, &memory);
    assert(res == VK_SUCCESS);

    res = vkMapMemory(g_vulkanImpl->Device, memory, 0, mem_reqs.size, 0, (void**)&info->pData);
    assert(res == VK_SUCCESS);

    res = vkBindBufferMemory(g_vulkanImpl->Device, buffer, memory, 0);
    assert(res == VK_SUCCESS);

    buffer_info.buffer = buffer;
    buffer_info.offset = 0;
    buffer_info.range = info->size;

    if (res == VK_SUCCESS)
        return true;
    else
        return false;
}

VKVertexBuffer::~VKVertexBuffer()
{
    vkUnmapMemory(g_vulkanImpl->Device, memory);
    vkDestroyBuffer(g_vulkanImpl->Device, buffer, NULL);
    vkFreeMemory(g_vulkanImpl->Device, memory, NULL);
}

VKPipelineStateObject::~VKPipelineStateObject()
{
    std::map<VKTexture*, VkFramebuffer>::iterator it;
    for (it = frameBuffers.begin(); it != frameBuffers.end(); it++)
    {
    }

    vkDestroyRenderPass(g_vulkanImpl->Device, render_pass, NULL);

    for (int i = 0; i < desc_layout.size(); i++)
        vkDestroyDescriptorSetLayout(g_vulkanImpl->Device, desc_layout[i], NULL);
    vkDestroyPipelineLayout(g_vulkanImpl->Device, pipeline_layout, NULL);

    vkDestroyPipeline(g_vulkanImpl->Device, pipeline, NULL);
}

void VKPipelineStateObject::BindUniform(std::string name, int index)
{
    BindingData data = { name, index};
    uniformBinding.insert(std::pair < std::string , BindingData > (data.name, data));
}

void VKPipelineStateObject::BindSampler(std::string name, int index)
{
    BindingData data = { name, index};
    samplerBinding.insert(std::pair<std::string , BindingData>(data.name, data));
}

void VKPipelineStateObject::BindTexture(std::string name, int index)
{
    BindingData data = { name, index };
    textureBinding.insert(std::pair<std::string, BindingData>(data.name, data));
}


void VKPipelineStateObject::SetUniformBuffer(std::string name, VKUniformBuffer* buffer)
{
    uniformBinding[name].uniformBuffer = buffer;
}

void VKPipelineStateObject::SetRendertargets(std::vector<VKTexture*> colorTargets, VKTexture* depthTarget)
{
    std::map<VKTexture*, VkFramebuffer>::iterator it;
    if ((it = frameBuffers.find(colorTargets[0])) == frameBuffers.end())
    {
        std::vector< VkImageView> attachments;
        for (int i = 0; i < colorTargets.size(); i++)
        {
            attachments.push_back(colorTargets[i]->view);
        }
        attachments.push_back(depthTarget->view);


        VkFramebufferCreateInfo fb_info = {};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.pNext = NULL;
        fb_info.renderPass = render_pass;
        fb_info.attachmentCount = attachments.size();
        fb_info.pAttachments = attachments.data();
        fb_info.width = colorTargets[0]->textureInfo.width;
        fb_info.height = colorTargets[0]->textureInfo.height;
        fb_info.layers = 1;

        vkCreateFramebuffer(g_vulkanImpl->Device, &fb_info, NULL, &currentFBO);
        
        frameBuffers.insert(std::pair< VKTexture*, VkFramebuffer>(colorTargets[0], currentFBO));
    }
    else
    {
        currentFBO = it->second;
    }

}

void VKPipelineStateObject::Apply()
{
    // allocate required descriptorsets for each resource type and update.
    VkResult res;

    std::vector< VkDescriptorBufferInfo> vecUniformBufferInfo;
    vecUniformBufferInfo.resize(uniformBinding.size());
    std::map<std::string, BindingData>::iterator it;
    for (it = uniformBinding.begin(); it != uniformBinding.end(); it++)
    {
        int BindingIndex = it->second.BindingIndex;
        VKUniformBuffer* buffer = it->second.uniformBuffer;
        vecUniformBufferInfo[BindingIndex] = buffer->buffer_info;
    }

    // descriptor set
    VkDescriptorSetAllocateInfo alloc_info;
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.descriptorPool = g_vulkanImpl->desc_pool;
    alloc_info.descriptorSetCount = desc_layout.size();
    alloc_info.pSetLayouts = desc_layout.data();

    desc_set.resize(desc_layout.size());
    res = vkAllocateDescriptorSets(g_vulkanImpl->Device, &alloc_info, desc_set.data());
    assert(res == VK_SUCCESS);


    std::vector<VkWriteDescriptorSet> writes;
    writes.resize(1);
    
    const int uniformBinding = 0;
    writes[uniformBinding] = {};
    writes[uniformBinding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[uniformBinding].pNext = NULL;
    writes[uniformBinding].dstSet = desc_set[0];
    writes[uniformBinding].descriptorCount = vecUniformBufferInfo.size();
    writes[uniformBinding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[uniformBinding].pBufferInfo = vecUniformBufferInfo.data();
    writes[uniformBinding].dstArrayElement = 0;
    writes[uniformBinding].dstBinding = 0;

    /*const int samplerBinding = 1;
    writes[samplerBinding] = {};
    writes[samplerBinding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[samplerBinding].pNext = NULL;
    writes[samplerBinding].dstSet = desc_set[0];
    writes[samplerBinding].descriptorCount = vecUniformBufferInfo.size();
    writes[samplerBinding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[samplerBinding].pBufferInfo = vecUniformBufferInfo.data();
    writes[samplerBinding].dstArrayElement = 0;
    writes[samplerBinding].dstBinding = 0;*/

    vkUpdateDescriptorSets(g_vulkanImpl->Device, writes.size(), writes.data(), 0, NULL);

}


bool VKPipelineStateObject::Create(PSOCreateInfo* info)
{
    VkResult res;


    std::vector< VkDescriptorSetLayoutBinding> vecBinding;
   
    VkDescriptorSetLayoutBinding uniform_binding_layout = {};
    uniform_binding_layout.binding = 0;
    uniform_binding_layout.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform_binding_layout.descriptorCount = uniformBinding.size();
    uniform_binding_layout.stageFlags = VK_SHADER_STAGE_ALL;
    uniform_binding_layout.pImmutableSamplers = NULL;

    vecBinding.push_back(uniform_binding_layout);


   /* VkDescriptorSetLayoutBinding sampler_binding_layout = {};
    sampler_binding_layout.binding = 0;
    sampler_binding_layout.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_binding_layout.descriptorCount = samplerBinding.size();
    sampler_binding_layout.stageFlags = VK_SHADER_STAGE_ALL;
    sampler_binding_layout.pImmutableSamplers = NULL;

    vecBinding.push_back(sampler_binding_layout);*/


    desc_layout.resize(vecBinding.size());
    int i = 0;
    for (auto& binding : vecBinding)
    {
        VkDescriptorSetLayoutCreateInfo uniform_descriptor_layout_info = {};
        uniform_descriptor_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        uniform_descriptor_layout_info.pNext = NULL;
        uniform_descriptor_layout_info.bindingCount = 1;// vecBinding.size();
        uniform_descriptor_layout_info.pBindings = &binding;

        res = vkCreateDescriptorSetLayout(g_vulkanImpl->Device, &uniform_descriptor_layout_info, NULL, &desc_layout[i++]);
        assert(res == VK_SUCCESS);
    }
   

    /* Now use the descriptor layout to create a pipeline layout */
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.pNext = NULL;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pipelineLayoutCreateInfo.pPushConstantRanges = NULL;
    pipelineLayoutCreateInfo.setLayoutCount = vecBinding.size();
    pipelineLayoutCreateInfo.pSetLayouts = desc_layout.data();

    res = vkCreatePipelineLayout(g_vulkanImpl->Device, &pipelineLayoutCreateInfo, NULL, &pipeline_layout);
    assert(res == VK_SUCCESS);

   
    // render pass
    std::vector< VkAttachmentDescription> attachments;
    attachments.resize(info->colorFormatVec.size() + 1);

    for (int i=0;i < info->colorFormatVec.size();i++)
    {
        //VkAttachmentDescription attachments[2];
        attachments[i].format = info->colorFormatVec[i];
        attachments[i].samples = info->samples;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[i].flags = 0;
    }
    
    
    attachments[info->colorFormatVec.size()].format = info->depthFormat;
    attachments[info->colorFormatVec.size()].samples = info->samples;
    attachments[info->colorFormatVec.size()].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[info->colorFormatVec.size()].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[info->colorFormatVec.size()].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[info->colorFormatVec.size()].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[info->colorFormatVec.size()].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[info->colorFormatVec.size()].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[info->colorFormatVec.size()].flags = 0;

    std::vector<VkAttachmentReference> color_reference;
    color_reference.resize(info->colorFormatVec.size());
    for (int i = 0; i < info->colorFormatVec.size(); i++)
    {
        color_reference[i].attachment = 0;
        color_reference[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }


    VkAttachmentReference depth_reference = {};
    depth_reference.attachment = 1;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.flags = 0;
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments = NULL;
    subpass.colorAttachmentCount = color_reference.size();
    subpass.pColorAttachments = color_reference.data();
    subpass.pResolveAttachments = NULL;
    subpass.pDepthStencilAttachment = &depth_reference;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = NULL;

    // Subpass dependency to wait for wsi image acquired semaphore before starting layout transition
    VkSubpassDependency subpass_dependency = {};
    subpass_dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    subpass_dependency.dstSubpass = 0;
    subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    subpass_dependency.srcAccessMask = 0;
    subpass_dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    subpass_dependency.dependencyFlags = 0;

    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.pNext = NULL;
    rp_info.attachmentCount = attachments.size();
    rp_info.pAttachments = attachments.data();
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &subpass_dependency;

    res = vkCreateRenderPass(g_vulkanImpl->Device, &rp_info, NULL, &render_pass);
    assert(res == VK_SUCCESS);


    // shader
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
    VkPipelineShaderStageCreateInfo shaderStagesVS;
    Microsoft::WRL::ComPtr<IDxcBlob> vs_blob = compileShaderLibrary(info->vsInfo->dir, info->vsInfo->file, info->vsInfo->entryPoint, info->vsInfo->target, std::nullopt);

    std::string vs_entryPointA;
    vs_entryPointA.assign(info->vsInfo->entryPoint.begin(), info->vsInfo->entryPoint.end());

    std::vector<unsigned int> vtx_spv;
    shaderStagesVS.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStagesVS.pNext = NULL;
    shaderStagesVS.pSpecializationInfo = NULL;
    shaderStagesVS.flags = 0;
    shaderStagesVS.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStagesVS.pName = vs_entryPointA.c_str();// "VSMain";

    VkShaderModuleCreateInfo moduleCreateInfo;
    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.pNext = NULL;
    moduleCreateInfo.flags = 0;
    moduleCreateInfo.codeSize = vs_blob->GetBufferSize();
    moduleCreateInfo.pCode = (uint32_t*)vs_blob->GetBufferPointer();
    res = vkCreateShaderModule(g_vulkanImpl->Device, &moduleCreateInfo, NULL, &shaderStagesVS.module);
    assert(res == VK_SUCCESS);
    shaderStages.push_back(shaderStagesVS);


    VkPipelineShaderStageCreateInfo shaderStagesPS;
    Microsoft::WRL::ComPtr<IDxcBlob> ps_blob = compileShaderLibrary(info->psInfo->dir, info->psInfo->file, info->psInfo->entryPoint, info->psInfo->target, std::nullopt);

    std::string ps_entryPointA;
    ps_entryPointA.assign(info->psInfo->entryPoint.begin(), info->psInfo->entryPoint.end());

    std::vector<unsigned int> frag_spv;
    shaderStagesPS.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStagesPS.pNext = NULL;
    shaderStagesPS.pSpecializationInfo = NULL;
    shaderStagesPS.flags = 0;
    shaderStagesPS.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStagesPS.pName = ps_entryPointA.c_str();// "PSMain";

    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.pNext = NULL;
    moduleCreateInfo.flags = 0;
    moduleCreateInfo.codeSize = ps_blob->GetBufferSize();
    moduleCreateInfo.pCode = (uint32_t*)ps_blob->GetBufferPointer();
    res = vkCreateShaderModule(g_vulkanImpl->Device, &moduleCreateInfo, NULL, &shaderStagesPS.module);
    assert(res == VK_SUCCESS);
    shaderStages.push_back(shaderStagesPS);


    // vertex input assembly
    vi_binding.binding = 0;
    vi_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vi_binding.stride = info->vertexInputLayoutInfo->vertexStride;

    for (int i = 0; i < info->vertexInputLayoutInfo->vertexElementVec.size(); i++)
    {
        auto& vi_att = info->vertexInputLayoutInfo->vertexElementVec[i];
        VkVertexInputAttributeDescription att = {
            /*location*/ i,
            /*binding*/ 0,
            /*format*/ vi_att.format,
            /*offset*/ vi_att.offset
        };

        vi_attribs.push_back(att);
    }

    // pipeline states
    // dynamic states
    VkDynamicState dynamicStateEnables[2];  // Viewport + Scissor
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    memset(dynamicStateEnables, 0, sizeof dynamicStateEnables);
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.pNext = NULL;
    dynamicState.pDynamicStates = dynamicStateEnables;
    dynamicState.dynamicStateCount = 0;


    VkPipelineVertexInputStateCreateInfo vi;
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vi_binding;
    vi.vertexAttributeDescriptionCount = vi_attribs.size();
    vi.pVertexAttributeDescriptions = vi_attribs.data();


    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = info->topology;// VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1.0f;

    VkPipelineColorBlendAttachmentState att_state[1];
    att_state[0].colorWriteMask = 0xf;
    att_state[0].blendEnable = VK_FALSE;
    att_state[0].alphaBlendOp = VK_BLEND_OP_ADD;
    att_state[0].colorBlendOp = VK_BLEND_OP_ADD;
    att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.pNext = NULL;
    cb.flags = 0;
    cb.attachmentCount = 1;
    cb.pAttachments = att_state;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
    vp.scissorCount = 1;
    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
    vp.pScissors = NULL;
    vp.pViewports = NULL;

    VkPipelineDepthStencilStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 0;
    ds.stencilTestEnable = VK_FALSE;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_KEEP;
    ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back.compareMask = 0;
    ds.back.reference = 0;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.writeMask = 0;
    ds.front = ds.back;

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    VkGraphicsPipelineCreateInfo pipeline_info;

    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = NULL;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.basePipelineHandle = VK_NULL_HANDLE;
    pipeline_info.basePipelineIndex = 0;
    pipeline_info.flags = 0;
    pipeline_info.pVertexInputState = &vi;
    pipeline_info.pInputAssemblyState = &ia;
    pipeline_info.pRasterizationState = &rs;
    pipeline_info.pColorBlendState = &cb;
    pipeline_info.pTessellationState = NULL;
    pipeline_info.pMultisampleState = &ms;
    pipeline_info.pDynamicState = &dynamicState;
    pipeline_info.pViewportState = &vp;
    pipeline_info.pDepthStencilState = &ds;
    pipeline_info.pStages = shaderStages.data();
    pipeline_info.stageCount = shaderStages.size();
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    res = vkCreateGraphicsPipelines(g_vulkanImpl->Device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
    assert(res == VK_SUCCESS);




    vkDestroyShaderModule(g_vulkanImpl->Device, shaderStagesVS.module, NULL);
    vkDestroyShaderModule(g_vulkanImpl->Device, shaderStagesPS.module, NULL);
    return true;
}


VulkanImpl::VulkanImpl()
{
    g_vulkanImpl = this;
}

VulkanImpl::~VulkanImpl()
{
    for (uint32_t i = 0; i < frameBuffers.size(); i++) {
        vkDestroyFramebuffer(Device, frameBuffers[i], NULL);
    }
    vkDestroyDescriptorPool(Device, desc_pool, NULL);
    vkDestroySwapchainKHR(Device, swap_chain, NULL);
    vkDestroyDevice(Device, NULL);
    vkDestroyInstance(Instance, NULL);
    dbgDestroyDebugReportCallback(Instance, debug_report_callback, NULL);
}

bool VulkanImpl::Init(HINSTANCE hInstance, HWND hWnd, UINT DisplayWidth, UINT DisplayHeight)
{
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pNext = NULL;
    app_info.pApplicationName = "Coronoa";
    app_info.applicationVersion = 1;
    app_info.pEngineName = "Coronoa";
    app_info.engineVersion = 1;
    app_info.apiVersion = VK_API_VERSION_1_0;


    std::vector<const char*> instance_extension_names;
    instance_extension_names.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    instance_extension_names.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    instance_extension_names.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    //instance_extension_names.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);


    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    const std::vector<const char*> validationLayers = {"VK_LAYER_KHRONOS_validation" };

    for (const char* layerName : validationLayers)
    {
        bool layerFound = false;
        for (const auto& layerProperties : availableLayers) 
        {
            if (strcmp(layerName, layerProperties.layerName) == 0) 
            {
                layerFound = true;
                break;
            }
        }
        if (!layerFound)
        {
          return false;
        }
    }

    std::vector<const char*> instance_layer_names;
    instance_layer_names.push_back("VK_LAYER_KHRONOS_validation");
    //instance_extension_names.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);


    // initialize the VkInstanceCreateInfo structure
    VkInstanceCreateInfo inst_info = {};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pNext = NULL;
    inst_info.flags = 0;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount = instance_extension_names.size();
    inst_info.ppEnabledExtensionNames = instance_extension_names.data();
    inst_info.enabledLayerCount = instance_layer_names.size();
    inst_info.ppEnabledLayerNames = instance_layer_names.data();

    VkResult res;

    res = vkCreateInstance(&inst_info, NULL, &Instance);
    if (res == VK_ERROR_INCOMPATIBLE_DRIVER)
    {
        assert(false);
        return false;
    }
    else if (res) 
    {
        assert(false);
        return false;
    }


    uint32_t gpu_count = 1;
    res = vkEnumeratePhysicalDevices(Instance, &gpu_count, NULL);
    assert(gpu_count);
    vecGPU.resize(gpu_count);
    res = vkEnumeratePhysicalDevices(Instance, &gpu_count, vecGPU.data());
    assert(!res && gpu_count >= 1);

    vkGetPhysicalDeviceProperties(vecGPU[0], &physical_device_props);

    uint32_t queue_family_count;

    vkGetPhysicalDeviceQueueFamilyProperties(vecGPU[0], &queue_family_count, NULL);
    assert(queue_family_count >= 1);

    std::vector<VkQueueFamilyProperties> queue_props;
    queue_props.resize(queue_family_count);

    vkGetPhysicalDeviceQueueFamilyProperties(vecGPU[0], &queue_family_count, queue_props.data());
    assert(queue_family_count >= 1);

    
    
    // find queue family index
    uint32_t graphics_queue_family_index;
    uint32_t present_queue_family_index;



    VkWin32SurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = NULL;
    createInfo.hinstance = hInstance;
    createInfo.hwnd = hWnd;
    res = vkCreateWin32SurfaceKHR(Instance, &createInfo, NULL, &Surface);
    assert(res == VK_SUCCESS);

    VkBool32* pSupportsPresent = (VkBool32*)malloc(queue_family_count * sizeof(VkBool32));
    for (uint32_t i = 0; i < queue_family_count; i++) {
        vkGetPhysicalDeviceSurfaceSupportKHR(vecGPU[0], i, Surface, &pSupportsPresent[i]);
    }

        

    graphics_queue_family_index = UINT32_MAX;
    present_queue_family_index = UINT32_MAX;
    for (uint32_t i = 0; i < queue_family_count; ++i) 
    {
        if ((queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) 
        {
            if (graphics_queue_family_index == UINT32_MAX) graphics_queue_family_index = i;

            if (pSupportsPresent[i] == VK_TRUE) 
            {
                graphics_queue_family_index = i;
                present_queue_family_index = i;
                break;
            }
        }
    }

    if (present_queue_family_index == UINT32_MAX) 
    {
        // If didn't find a queue that supports both graphics and present, then
        // find a separate present queue.
        for (size_t i = 0; i < queue_family_count; ++i)
        {
            if (pSupportsPresent[i] == VK_TRUE)
            {
                present_queue_family_index = i;
                break;
            }
        }
    }
    free(pSupportsPresent);

    // create logical device
    VkDeviceQueueCreateInfo queue_info = {};

    float queue_priorities[1] = { 0.0 };
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.pNext = NULL;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = queue_priorities;
    queue_info.queueFamilyIndex = graphics_queue_family_index;

    std::vector<const char*> device_extension_names;
    device_extension_names.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = NULL;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = device_extension_names.size();
    device_info.ppEnabledExtensionNames = device_extension_names.data();
    device_info.enabledLayerCount = 0;
    device_info.ppEnabledLayerNames = NULL;
    device_info.pEnabledFeatures = NULL;

    res = vkCreateDevice(vecGPU[0], &device_info, NULL, &Device);
    assert(res == VK_SUCCESS);
    


    dbgCreateDebugReportCallback = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(Instance, "vkCreateDebugReportCallbackEXT");
    if (!dbgCreateDebugReportCallback) 
    {
        exit(1);
    }
    
    dbgDestroyDebugReportCallback = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(Instance, "vkDestroyDebugReportCallbackEXT");
    if (!dbgDestroyDebugReportCallback) {
        exit(1);
    }

    VkDebugReportCallbackCreateInfoEXT create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT;
    create_info.pNext = NULL;
    create_info.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
    create_info.pfnCallback = dbgFunc;
    create_info.pUserData = NULL;
    res = dbgCreateDebugReportCallback(Instance, &create_info, NULL, &debug_report_callback);
    switch (res) 
    {
    case VK_SUCCESS:
        break;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        break;
    default:
        break;
    }

    vkGetDeviceQueue(Device, graphics_queue_family_index, 0, &GraphicsQueue);

    // get supported frame buffer formats
    uint32_t formatCount;
    res = vkGetPhysicalDeviceSurfaceFormatsKHR(vecGPU[0], Surface, &formatCount, NULL);
    assert(res == VK_SUCCESS);
    std::vector<VkSurfaceFormatKHR> surfFormats;
    surfFormats.resize(formatCount);
    res = vkGetPhysicalDeviceSurfaceFormatsKHR(vecGPU[0], Surface, &formatCount, surfFormats.data());
    assert(res == VK_SUCCESS);

    VkFormat format;
    if (formatCount == 1 && surfFormats[0].format == VK_FORMAT_UNDEFINED) 
    {
        format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    else 
    {
        assert(formatCount >= 1);
        format = surfFormats[0].format;
    }

    // present capabilities
    VkSurfaceCapabilitiesKHR surfCapabilities;

    res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vecGPU[0], Surface, &surfCapabilities);
    assert(res == VK_SUCCESS);

    uint32_t presentModeCount;
    res = vkGetPhysicalDeviceSurfacePresentModesKHR(vecGPU[0], Surface, &presentModeCount, NULL);
    assert(res == VK_SUCCESS);
    
    std::vector<VkPresentModeKHR> presentModes;
    presentModes.resize(presentModeCount);
    res = vkGetPhysicalDeviceSurfacePresentModesKHR(vecGPU[0], Surface, &presentModeCount, presentModes.data());
    assert(res == VK_SUCCESS);

    VkExtent2D swapchainExtent;
    // width and height are either both 0xFFFFFFFF, or both not 0xFFFFFFFF.
    if (surfCapabilities.currentExtent.width == 0xFFFFFFFF) {
        // If the surface size is undefined, the size is set to
        // the size of the images requested.
        swapchainExtent.width = DisplayWidth;
        swapchainExtent.height = DisplayHeight;
        if (swapchainExtent.width < surfCapabilities.minImageExtent.width) {
            swapchainExtent.width = surfCapabilities.minImageExtent.width;
        }
        else if (swapchainExtent.width > surfCapabilities.maxImageExtent.width) {
            swapchainExtent.width = surfCapabilities.maxImageExtent.width;
        }

        if (swapchainExtent.height < surfCapabilities.minImageExtent.height) {
            swapchainExtent.height = surfCapabilities.minImageExtent.height;
        }
        else if (swapchainExtent.height > surfCapabilities.maxImageExtent.height) {
            swapchainExtent.height = surfCapabilities.maxImageExtent.height;
        }
    }
    else 
    {
        // If the surface size is defined, the swap chain size must match
        swapchainExtent = surfCapabilities.currentExtent;
    }

    // The FIFO present mode is guaranteed by the spec to be supported
    VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;

    // Determine the number of VkImage's to use in the swap chain.
    // We need to acquire only 1 presentable image at at time.
    // Asking for minImageCount images ensures that we can acquire
    // 1 presentable image as long as we present it before attempting
    // to acquire another.
    uint32_t desiredNumberOfSwapChainImages = surfCapabilities.minImageCount;

    VkSurfaceTransformFlagBitsKHR preTransform;
    if (surfCapabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) 
    {
        preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    else 
    {
        preTransform = surfCapabilities.currentTransform;
    }

    // Find a supported composite alpha mode - one of these is guaranteed to be set
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR compositeAlphaFlags[4] = 
    {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };

    for (uint32_t i = 0; i < sizeof(compositeAlphaFlags) / sizeof(compositeAlphaFlags[0]); i++) 
    {
        if (surfCapabilities.supportedCompositeAlpha & compositeAlphaFlags[i]) 
        {
            compositeAlpha = compositeAlphaFlags[i];
            break;
        }
    }

    VkSwapchainCreateInfoKHR swapchain_ci = {};
    swapchain_ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_ci.pNext = NULL;
    swapchain_ci.surface = Surface;
    swapchain_ci.minImageCount = desiredNumberOfSwapChainImages;
    swapchain_ci.imageFormat = format;
    swapchain_ci.imageExtent.width = swapchainExtent.width;
    swapchain_ci.imageExtent.height = swapchainExtent.height;
    swapchain_ci.preTransform = preTransform;
    swapchain_ci.compositeAlpha = compositeAlpha;
    swapchain_ci.imageArrayLayers = 1;
    swapchain_ci.presentMode = swapchainPresentMode;
    swapchain_ci.oldSwapchain = VK_NULL_HANDLE;
    swapchain_ci.clipped = true;
    swapchain_ci.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    swapchain_ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_ci.queueFamilyIndexCount = 0;
    swapchain_ci.pQueueFamilyIndices = NULL;
    uint32_t queueFamilyIndices[2] = { (uint32_t)graphics_queue_family_index, (uint32_t)present_queue_family_index };
    if (graphics_queue_family_index != present_queue_family_index) 
    {
        // If the graphics and present queues are from different queue families,
        // we either have to explicitly transfer ownership of images between
        // the queues, or we have to create the swapchain with imageSharingMode
        // as VK_SHARING_MODE_CONCURRENT
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = 2;
        swapchain_ci.pQueueFamilyIndices = queueFamilyIndices;
    }

    res = vkCreateSwapchainKHR(Device, &swapchain_ci, NULL, &swap_chain);
    assert(res == VK_SUCCESS);

    res = vkGetSwapchainImagesKHR(Device, swap_chain, &swapchainImageCount, NULL);
    assert(res == VK_SUCCESS);

    swapchainImages.resize(swapchainImageCount);
    res = vkGetSwapchainImagesKHR(Device, swap_chain, &swapchainImageCount, swapchainImages.data());
    assert(res == VK_SUCCESS);

    frameBuffers.resize(swapchainImageCount);

    for (uint32_t i = 0; i < swapchainImageCount; i++)
    {
        VKTexture* tex = new VKTexture;


        TextureCreateInfo textureInfo;
        textureInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        textureInfo.width = DisplayWidth;
        textureInfo.height = DisplayHeight;
        textureInfo.depth = 1;
        textureInfo.mipLevels = 1;
        textureInfo.arrayLayers = 1;

        tex->CreateFromImage(swapchainImages[i], &textureInfo);

        frameBufferTextures.push_back(std::shared_ptr<VKTexture>(tex));

      /*  VkFramebuffer& fb = frameBuffers[i];
        res = vkCreateFramebuffer(Device, &fb_info, NULL, &info.framebuffers[i]);*/

    }

    // depth texture
    TextureCreateInfo textureInfo;
    textureInfo.format = VK_FORMAT_D32_SFLOAT;
    textureInfo.width = DisplayWidth;
    textureInfo.height = DisplayHeight;
    textureInfo.depth = 1;
    textureInfo.mipLevels = 1;
    textureInfo.arrayLayers = 1;

    depthTexture = std::shared_ptr<VKTexture>(new VKTexture);
    depthTexture->Create(&textureInfo);


    // command pool
    /* Create a command pool to allocate our command buffer from */
    VkCommandPoolCreateInfo cmd_pool_info = {};
    cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_info.pNext = NULL;
    cmd_pool_info.queueFamilyIndex = graphics_queue_family_index;
    cmd_pool_info.flags = 0;

    res = vkCreateCommandPool(Device, &cmd_pool_info, NULL, &cmd_pool);
    assert(res == VK_SUCCESS);

    /* Create the command buffer from the command pool */
    VkCommandBufferAllocateInfo cmd_info = {};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.pNext = NULL;
    cmd_info.commandPool = cmd_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;

    res = vkAllocateCommandBuffers(Device, &cmd_info, &cmd);
    assert(res == VK_SUCCESS);

    UniformBufferCreateInfo uniformBufferInfo;
    uniformBufferInfo.size = sizeof(MVP);

    uniformBuffer = std::shared_ptr<VKUniformBuffer>(new VKUniformBuffer);
    uniformBuffer->Create(&uniformBufferInfo);

    // descriptor pool
    std::vector<VkDescriptorPoolSize> type_count =
    {
        {
            /*type*/VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            /*descriptorCount*/10000
        },
       
        {
            /*type*/VK_DESCRIPTOR_TYPE_SAMPLER,
            /*descriptorCount*/100
        },
    };

    VkDescriptorPoolCreateInfo descriptor_pool = {};
    descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool.pNext = NULL;
    descriptor_pool.maxSets = 100;
    descriptor_pool.poolSizeCount = type_count.size();
    descriptor_pool.pPoolSizes = type_count.data();

    res = vkCreateDescriptorPool(Device, &descriptor_pool, NULL, &desc_pool);
    assert(res == VK_SUCCESS);
}

void VulkanImpl::GetFrameBuffers(std::vector<std::shared_ptr<VKTexture>>& vkFrameFuffers)
{
    for (auto& fbTex : frameBufferTextures)
    {
        vkFrameFuffers.push_back(fbTex);
    }
}
