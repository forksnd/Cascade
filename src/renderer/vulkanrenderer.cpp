/*
 *  Cascade Image Editor
 *
 *  Copyright (C) 2022 Till Dechent and contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "vulkanrenderer.h"

#include <iostream>

#include <QVulkanFunctions>
#include <QCoreApplication>
#include <QFile>
#include <QMouseEvent>
#include <QVulkanWindowRenderer>

#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/color.h>

#include "../vulkanwindow.h"
#include "../uientities/fileboxentity.h"
#include "../benchmark.h"
#include "../multithreading.h"
#include "../log.h"

// Use a triangle strip to get a quad.
static float vertexData[] = { // Y up, front = CW
    // x, y, z, u, v
    -1,  -1, 0, 0, 1,
    -1,   1, 0, 0, 0,
     1,  -1, 0, 1, 1,
     1,   1, 0, 1, 0
};

static const int UNIFORM_DATA_SIZE = 16 * sizeof(float);

static inline VkDeviceSize aligned(VkDeviceSize v, VkDeviceSize byteAlign)
{
    return (v + byteAlign - 1) & ~(byteAlign - 1);
}

VulkanRenderer::VulkanRenderer(VulkanWindow *w)
    : window(w)
{
    concurrentFrameCount = window->concurrentFrameCount();
}

void VulkanRenderer::initResources()
{
    /// Get device and functions
    device = window->device();
    physicalDevice = window->physicalDevice();
    devFuncs = window->vulkanInstance()->deviceFunctions(device);
    f = window->vulkanInstance()->functions();

    /// Init all the permanent parts of the renderer
    createVertexBuffer();
    createSampler();
    createDescriptorPool();
    createGraphicsDescriptors();
    createGraphicsPipelineCache();
    createGraphicsPipelineLayout();

    createGraphicsPipeline(graphicsPipelineRGB, ":/shaders/texture_frag.spv");
    createGraphicsPipeline(graphicsPipelineAlpha, ":/shaders/texture_alpha_frag.spv");

    createComputeDescriptors();
    createComputePipelineLayout();

    /// Load all the shaders we need and create their pipelines
    loadShadersFromDisk();
    // Create Noop pipeline
    computePipelineNoop = createComputePipelineNoop();
    // Create a pipeline for each shader
    createComputePipelines();

    createComputeQueue();
    createComputeCommandPool();
    createComputeCommandBuffers();

    settingsBuffer = std::unique_ptr<CsSettingsBuffer>(new CsSettingsBuffer(
                &device,
                &physicalDevice,
                devFuncs,
                f));

    // Load OCIO config
    try
    {
        const char* file = "ocio/config.ocio";
        ocioConfig = OCIO::Config::CreateFromFile(file);
    }
    catch(OCIO::Exception& exception)
    {
        CS_LOG_CRITICAL("OpenColorIO Error: " + QString(exception.what()));
    }

    emit window->rendererHasBeenCreated();
}

QString VulkanRenderer::getGpuName()
{
    VkPhysicalDeviceProperties deviceProperties = {};
    f->vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    auto deviceName = QString::fromLatin1(deviceProperties.deviceName);

    return deviceName;
}

void VulkanRenderer::createVertexBuffer()
{
    const vk::PhysicalDeviceLimits pdevLimits = physicalDevice.getProperties().limits;
    const vk::DeviceSize uniAlign = pdevLimits.minUniformBufferOffsetAlignment;

    const vk::DeviceSize vertexAllocSize = aligned(sizeof(vertexData), uniAlign);
    const vk::DeviceSize uniformAllocSize = aligned(UNIFORM_DATA_SIZE, uniAlign);

    vk::BufferCreateInfo bufferInfo({},
                         vertexAllocSize + concurrentFrameCount * uniformAllocSize,
                         vk::BufferUsageFlagBits::eVertexBuffer);
    vertexBuffer = device.createBufferUnique(bufferInfo);

    vk::MemoryRequirements memReq;
    device.getBufferMemoryRequirements(*vertexBuffer);

    vk::MemoryAllocateInfo memAllocInfo(memReq.size,
                                        window->hostVisibleMemoryIndex());

    vertexBufferMemory = device.allocateMemoryUnique(memAllocInfo);

    device.bindBufferMemory(*vertexBuffer, *vertexBufferMemory, 0);

    quint8 *p;
    device.mapMemory(*vertexBufferMemory,
                     0,
                     memReq.size,
                     {},
                     reinterpret_cast<void **>(&p));

    memcpy(p, vertexData, sizeof(vertexData));
    QMatrix4x4 ident;
    memset(uniformBufferInfo, 0, sizeof(uniformBufferInfo));
    for (int i = 0; i < concurrentFrameCount; ++i) {
        const vk::DeviceSize offset = vertexAllocSize + i * uniformAllocSize;
        memcpy(p + offset, ident.constData(), 16 * sizeof(float));
        uniformBufferInfo[i].setBuffer(*vertexBuffer);
        uniformBufferInfo[i].setOffset(offset);
        uniformBufferInfo[i].setRange(uniformAllocSize);
    }
    device.unmapMemory(*vertexBufferMemory);
}

void VulkanRenderer::createSampler()
{
    // Create sampler
    vk::SamplerCreateInfo samplerInfo({},
                                      vk::Filter::eNearest,
                                      vk::Filter::eNearest,
                                      vk::SamplerMipmapMode::eNearest,
                                      vk::SamplerAddressMode::eClampToEdge,
                                      vk::SamplerAddressMode::eClampToEdge,
                                      vk::SamplerAddressMode::eClampToEdge,
                                      0,
                                      0);

    sampler = device.createSamplerUnique(samplerInfo);
}

void VulkanRenderer::createDescriptorPool()
{
    // Create descriptor pool
    vk::DescriptorPoolSize descPoolSizes[4] = {
        { vk::DescriptorType::eUniformBuffer,         3 * uint32_t(concurrentFrameCount) },
        { vk::DescriptorType::eCombinedImageSampler,  1 * uint32_t(concurrentFrameCount) },
        { vk::DescriptorType::eCombinedImageSampler,  1 * uint32_t(concurrentFrameCount) },
        { vk::DescriptorType::eStorageImage,          6 * uint32_t(concurrentFrameCount) }
    };

    vk::DescriptorPoolCreateInfo descPoolInfo({},
                                              6,
                                              4,
                                              descPoolSizes);

    descriptorPool = device.createDescriptorPoolUnique(descPoolInfo);
}

void VulkanRenderer::createGraphicsDescriptors()
{
    // Create DescriptorSetLayout
    vk::DescriptorSetLayoutBinding layoutBinding[2] = {
        {
            0, // binding
            vk::DescriptorType::eUniformBuffer,
            1, // descriptorCount
            vk::ShaderStageFlagBits::eVertex,
            nullptr
        },
        {
            1, // binding
            vk::DescriptorType::eCombinedImageSampler,
            1, // descriptorCount
            vk::ShaderStageFlagBits::eFragment,
            nullptr
        }
    };

    vk::DescriptorSetLayoutCreateInfo descLayoutInfo({},
                                                     2, // bindingCount
                                                     layoutBinding);

    graphicsDescriptorSetLayout = device.createDescriptorSetLayoutUnique(descLayoutInfo);
}

void VulkanRenderer::createGraphicsPipelineCache()
{
    // Pipeline cache
    vk::PipelineCacheCreateInfo pipelineCacheInfo({},
                                                  {});

    pipelineCache = device.createPipelineCacheUnique(pipelineCacheInfo);
}

void VulkanRenderer::createGraphicsPipelineLayout()
{
    vk::PushConstantRange pushConstantRange;
    pushConstantRange.stageFlags                = vk::ShaderStageFlagBits::eFragment;
    pushConstantRange.offset                    = 0;
    pushConstantRange.size                      = sizeof(viewerPushConstants);

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo({},
                                                    1,
                                                    &(*graphicsDescriptorSetLayout),
                                                    1,
                                                    &pushConstantRange);

    graphicsPipelineLayout = device.createPipelineLayoutUnique(pipelineLayoutInfo);
}

void VulkanRenderer::createBuffer(
        vk::UniqueBuffer& buffer,
        vk::UniqueDeviceMemory& bufferMemory,
        vk::DeviceSize& size)
{
    vk::BufferCreateInfo bufferInfo({},
                                    size,
                                    vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eUniformBuffer,
                                    vk::SharingMode::eExclusive);

    buffer = device.createBufferUnique(bufferInfo);

    vk::MemoryRequirements memRequirements = device.getBufferMemoryRequirements(*buffer);

    uint32_t memoryType = findMemoryType(memRequirements.memoryTypeBits,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                         vk::MemoryPropertyFlagBits::eHostCoherent);

    vk::MemoryAllocateInfo allocInfo(memRequirements.size,
                                     memoryType);

    bufferMemory = device.allocateMemoryUnique(allocInfo);

    device.bindBufferMemory(*buffer, *bufferMemory, 0);
}

void VulkanRenderer::fillSettingsBuffer(NodeBase* node)
{
    auto props = node->getAllPropertyValues();

    settingsBuffer->fillBuffer(props);
}

void VulkanRenderer::createGraphicsPipeline(vk::UniquePipeline& pl,
                                            const QString& fragShaderPath)
{
    // Vertex shader never changes
    vk::UniqueShaderModule vertShaderModule = createShaderFromFile(":/shaders/texture_vert.spv");

    vk::UniqueShaderModule fragShaderModule = createShaderFromFile(fragShaderPath);

    // Graphics pipeline
    vk::GraphicsPipelineCreateInfo pipelineInfo;

    vk::PipelineShaderStageCreateInfo shaderStages[2] =
    {
        {
            {},
            vk::ShaderStageFlagBits::eVertex,
            *vertShaderModule,
            "main"
        },
        {
            {},
            vk::ShaderStageFlagBits::eFragment,
            *fragShaderModule,
            "main"
        }
    };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;

    // Vertex binding
    vk::VertexInputBindingDescription vertexBindingDesc(0,
                                                        5 * sizeof(float),
                                                        vk::VertexInputRate::eVertex);

    vk::VertexInputAttributeDescription vertexAttrDesc[] =
    {
        { // position
            0, // location
            0, // binding
            vk::Format::eR32G32B32Sfloat,
            0
        },
        { // texcoord
            1,
            0,
            vk::Format::eR32G32Sfloat,
            3 * sizeof(float)
        }
    };

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo({},
                                                           1,
                                                           &vertexBindingDesc,
                                                           2,
                                                           vertexAttrDesc);

    pipelineInfo.pVertexInputState = &vertexInputInfo;

    vk::PipelineInputAssemblyStateCreateInfo ia({},
                                                vk::PrimitiveTopology::eTriangleStrip);
    pipelineInfo.pInputAssemblyState = &ia;

    // The viewport and scissor will be set dynamically via vkCmdSetViewport/Scissor.
    // This way the pipeline does not need to be touched when resizing the window.
    vk::PipelineViewportStateCreateInfo vp({},
                                           1,
                                           {},
                                           1);
    pipelineInfo.pViewportState = &vp;

    vk::PipelineRasterizationStateCreateInfo rs({},
                                                false,
                                                false,
                                                vk::PolygonMode::eFill,
                                                vk::CullModeFlagBits::eBack,
                                                vk::FrontFace::eClockwise,
                                                {},
                                                {},
                                                {},
                                                {},
                                                1.0f);
    pipelineInfo.pRasterizationState = &rs;

    vk::PipelineMultisampleStateCreateInfo ms({},
                                              vk::SampleCountFlagBits::e1);
    pipelineInfo.pMultisampleState = &ms;

    vk::PipelineDepthStencilStateCreateInfo ds({},
                                               true,
                                               true,
                                               vk::CompareOp::eLessOrEqual);
    pipelineInfo.pDepthStencilState = &ds;


    // assume pre-multiplied alpha, blend, write out all of rgba
    vk::PipelineColorBlendAttachmentState att(true,
                                              vk::BlendFactor::eOne,
                                              vk::BlendFactor::eOne,
                                              vk::BlendOp::eAdd,
                                              vk::BlendFactor::eOne,
                                              vk::BlendFactor::eOne,
                                              vk::BlendOp::eAdd);

    vk::PipelineColorBlendStateCreateInfo cb({},
                                             {},
                                             {},
                                             1,
                                             &att);
    pipelineInfo.pColorBlendState = &cb;

    vk::DynamicState dynEnable[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };

    vk::PipelineDynamicStateCreateInfo dyn({},
                                           sizeof(dynEnable) / sizeof(vk::DynamicState),
                                           dynEnable);
    pipelineInfo.pDynamicState = &dyn;

    pipelineInfo.layout = *graphicsPipelineLayout;
    pipelineInfo.renderPass = window->defaultRenderPass();

    pl = device.createGraphicsPipelineUnique(*pipelineCache, pipelineInfo).value;
}

vk::UniqueShaderModule VulkanRenderer::createShaderFromFile(const QString &name)
{
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly))
    {
        CS_LOG_WARNING("Failed to read shader:");
        CS_LOG_WARNING(qPrintable(name));
    }
    QByteArray blob = file.readAll();
    file.close();

    vk::ShaderModuleCreateInfo shaderInfo({},
                                          blob.size(),
                                          reinterpret_cast<const uint32_t *>(blob.constData()));

    vk::UniqueShaderModule shaderModule = device.createShaderModuleUnique(shaderInfo);

    return shaderModule;
}

void VulkanRenderer::loadShadersFromDisk()
{
    for (int i = 0; i != NODE_TYPE_MAX; i++)
    {
        NodeType nodeType = static_cast<NodeType>(i);

        auto props = getPropertiesForType(nodeType);

        shaders[nodeType] = createShaderFromFile(props.shaderPath);
    }
}

bool VulkanRenderer::createComputeRenderTarget(uint32_t width, uint32_t height)
{
    // Previous image will be destroyed, so we wait here
    device.waitIdle();
    //devFuncs->vkQueueWaitIdle(compute.computeQueue);

    computeRenderTarget = std::shared_ptr<CsImage>(
                new CsImage(window, &device, &physicalDevice, width, height));

    emit window->renderTargetHasBeenCreated(width, height);

    currentRenderSize = QSize(width, height);

    return true;
}

void VulkanRenderer::createImageMemoryBarrier(
        vk::ImageMemoryBarrier& barrier,
        vk::ImageLayout targetLayout,
        vk::AccessFlags srcMask,
        vk::AccessFlags dstMask,
        CsImage& image)
{
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;

    barrier.oldLayout       = image.getLayout();
    image.setLayout(targetLayout);
    barrier.newLayout       = targetLayout;
    barrier.srcAccessMask   = srcMask;
    barrier.dstAccessMask   = dstMask;
    barrier.image           = *image.getImage();
}

bool VulkanRenderer::createTextureFromFile(const QString &path, const int colorSpace)
{
    cpuImage = std::unique_ptr<ImageBuf>(new ImageBuf(path.toStdString()));
    bool ok = cpuImage->read(0, 0, 0, 4, true, TypeDesc::FLOAT);
    if (!ok)
    {
        CS_LOG_WARNING("There was a problem reading the image from disk.");
        CS_LOG_WARNING(QString::fromStdString(cpuImage->geterror()));
    }

    // Add alpha channel if it doesn't exist
    if (cpuImage->nchannels() == 3)
    {
        int channelorder[] = { 0, 1, 2, -1 /*use a float value*/ };
        float channelvalues[] = { 0 /*ignore*/, 0 /*ignore*/, 0 /*ignore*/, 1.0 };
        std::string channelnames[] = { "R", "G", "B", "A" };

        *cpuImage = ImageBufAlgo::channels(*cpuImage, 4, channelorder, channelvalues, channelnames);
    }

    transformColorSpace(lookupColorSpace(colorSpace), "linear", *cpuImage);

    updateVertexData(cpuImage->xend(), cpuImage->yend());

    imageFromDisk = std::shared_ptr<CsImage>(new CsImage(
                                                 window,
                                                 &device,
                                                 &physicalDevice,
                                                 cpuImage->xend(),
                                                 cpuImage->yend()));

    auto imageSize = QSize(cpuImage->xend(), cpuImage->yend());

    // Now we can either map and copy the image data directly, or have to go
    // through a staging buffer to copy and convert into the internal optimal
    // tiling format.
    vk::FormatProperties props = physicalDevice.getFormatProperties(globalImageFormat, props);
    const bool canSampleLinear = (bool)(props.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage);
    const bool canSampleOptimal = (bool)(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage);
    if (!canSampleLinear && !canSampleOptimal) {
        CS_LOG_WARNING("Neither linear nor optimal image sampling is supported for image");
        return false;
    }

//    if (loadImageStaging) {
//        devFuncs->vkDestroyImage(device, loadImageStaging, nullptr);
//        loadImageStaging = VK_NULL_HANDLE;
//    }

//    if (loadImageStagingMem) {
//        devFuncs->vkFreeMemory(device, loadImageStagingMem, nullptr);
//        loadImageStagingMem = VK_NULL_HANDLE;
//    }

    if (!createTextureImage(imageSize,
                            loadImageStaging,
                            loadImageStagingMem,
                            vk::ImageTiling::eLinear,
                            vk::ImageUsageFlagBits::eTransferSrc,
                            window->hostVisibleMemoryIndex()))
        return false;

    if (!writeLinearImage(
                static_cast<float*>(cpuImage->localpixels()),
                QSize(cpuImage->xend(), cpuImage->yend()),
                loadImageStaging,
                loadImageStagingMem))
    {
        CS_LOG_WARNING("Failed to write linear image");
        CS_LOG_CONSOLE("Failed to write linear image");
        return false;
    }

    texStagingPending = true;

    vk::ImageViewCreateInfo viewInfo({},
                                     *imageFromDisk->getImage(),
                                     vk::ImageViewType::e2D,
                                     globalImageFormat,
                                     {
                                         vk::ComponentSwizzle::eR,
                                         vk::ComponentSwizzle::eG,
                                         vk::ComponentSwizzle::eB,
                                         vk::ComponentSwizzle::eA
                                     },
                                     vk::ImageSubresourceRange({}, {}, 1, {}, 1));

    imageFromDisk->getImageView() =  device.createImageViewUnique(viewInfo); // Um ok...?!

    loadImageSize = imageSize;

    return true;
}

QString VulkanRenderer::lookupColorSpace(const int i)
{
    // 0 = sRGB
    // 1 = Linear
    // 2 = rec709
    // 3 = Gamma 1.8
    // 4 = Gamma 2.2
    // 5 = Panalog
    // 6 = REDLog
    // 7 = ViperLog
    // 8 = AlexaV3LogC
    // 9 = PLogLin
    // 10 = SLog
    // 11 = Raw
    // If space is Linear, no conversion necessary

    switch(i)
    {
        case 0:
            return "sRGB";
        case 2:
            return "rec709";
        case 3:
            return "Gamma1.8";
        case 4:
            return "Gamma2.2";
        case 5:
            return "Panalog";
        case 6:
            return "REDLog";
        case 7:
            return "ViperLog";
        case 8:
            return "AlexaV3LogC";
        case 9:
            return "PLogLin";
        case 10:
            return "SLog";
        case 11:
            return "raw";
        default:
            return "linear";
    }
}

void VulkanRenderer::transformColorSpace(const QString& from, const QString& to, ImageBuf& image)
{
    parallelApplyColorSpace(
                ocioConfig,
                from,
                to,
                static_cast<float*>(image.localpixels()),
                image.xend(),
                image.yend());
}

void VulkanRenderer::createComputeDescriptors()
{
    if (computeDescriptorSetLayoutGeneric == VK_NULL_HANDLE)
    {
        // Define the layout of the input of the shader.
        // 2 images to read, 1 image to write
        VkDescriptorSetLayoutBinding bindings[4]= {};

        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[3].binding         = 3;
        bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo {};
        descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutCreateInfo.pBindings = bindings;
        descriptorSetLayoutCreateInfo.bindingCount = 4;

        //Create the layout, store it to share between shaders
        VkResult err = devFuncs->vkCreateDescriptorSetLayout(
                    device,
                    &descriptorSetLayoutCreateInfo,
                    nullptr,
                    &computeDescriptorSetLayoutGeneric);
        if (err != VK_SUCCESS)
            qFatal("Failed to create compute descriptor set layout: %d", err);
    }

    // Descriptor sets
    for (int i = 0; i < concurrentFrameCount; ++i)
    {
        {
            VkDescriptorSetAllocateInfo descSetAllocInfo = {
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                nullptr,
                descriptorPool,
                1,
                &graphicsDescriptorSetLayout
            };
            VkResult err = devFuncs->vkAllocateDescriptorSets(
                        device,
                        &descSetAllocInfo,
                        &graphicsDescriptorSet[i]);
            if (err != VK_SUCCESS)
                qFatal("Failed to allocate descriptor set: %d", err);
        }
        {
            VkDescriptorSetAllocateInfo descSetAllocInfo = {
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                nullptr,
                descriptorPool,
                1,
                &computeDescriptorSetLayoutGeneric
            };
            VkResult err = devFuncs->vkAllocateDescriptorSets(
                        device,
                        &descSetAllocInfo,
                        &computeDescriptorSetGeneric);
            if (err != VK_SUCCESS)
                qFatal("Failed to allocate descriptor set: %d", err);
        }
    }
}

void VulkanRenderer::updateComputeDescriptors(
        std::shared_ptr<CsImage> inputImageBack,
        std::shared_ptr<CsImage> inputImageFront,
        std::shared_ptr<CsImage> outputImage)
{
    for (int i = 0; i < concurrentFrameCount; ++i)
    {
        VkWriteDescriptorSet descWrite[2];
        memset(descWrite, 0, sizeof(descWrite));
        descWrite[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descWrite[0].dstSet = graphicsDescriptorSet[i];
        descWrite[0].dstBinding = 0;
        descWrite[0].descriptorCount = 1;
        descWrite[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descWrite[0].pBufferInfo = &uniformBufferInfo[i];

        VkDescriptorImageInfo descImageInfo = {
            sampler,
            outputImage->getImageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        descWrite[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descWrite[1].dstSet = graphicsDescriptorSet[i];
        descWrite[1].dstBinding = 1;
        descWrite[1].descriptorCount = 1;
        descWrite[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descWrite[1].pImageInfo = &descImageInfo;

        devFuncs->vkUpdateDescriptorSets(device, 2, descWrite, 0, nullptr);
    }

    {
        VkDescriptorImageInfo sourceInfoBack     = { };
        sourceInfoBack.imageView                 = inputImageBack->getImageView();
        sourceInfoBack.imageLayout               = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo sourceInfoFront    = { };
        sourceInfoFront.imageLayout              = VK_IMAGE_LAYOUT_GENERAL;
        if (inputImageFront)
        {
            sourceInfoFront.imageView            = inputImageFront->getImageView();
        }
        else
        {
            sourceInfoFront.imageView            = inputImageBack->getImageView();
        }

        VkDescriptorImageInfo destinationInfo     = { };
        destinationInfo.imageView                 = outputImage->getImageView();
        destinationInfo.imageLayout               = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo settingsBufferInfo = { };
        settingsBufferInfo.buffer                 = *settingsBuffer->getBuffer();
        settingsBufferInfo.offset                 = 0;
        settingsBufferInfo.range                  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet descWrite[4]= {};

        descWrite[0].sType                     = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descWrite[0].dstSet                    = computeDescriptorSetGeneric;
        descWrite[0].dstBinding                = 0;
        descWrite[0].descriptorCount           = 1;
        descWrite[0].descriptorType            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descWrite[0].pImageInfo                = &sourceInfoBack;

        descWrite[1].sType                     = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descWrite[1].dstSet                    = computeDescriptorSetGeneric;
        descWrite[1].dstBinding                = 1;
        descWrite[1].descriptorCount           = 1;
        descWrite[1].descriptorType            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descWrite[1].pImageInfo                = &sourceInfoFront;

        descWrite[2].sType                     = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descWrite[2].dstSet                    = computeDescriptorSetGeneric;
        descWrite[2].dstBinding                = 2;
        descWrite[2].descriptorCount           = 1;
        descWrite[2].descriptorType            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descWrite[2].pImageInfo                = &destinationInfo;

        descWrite[3].sType                     = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descWrite[3].dstSet                    = computeDescriptorSetGeneric;
        descWrite[3].dstBinding                = 3;
        descWrite[3].descriptorCount           = 1;
        descWrite[3].descriptorType            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descWrite[3].pBufferInfo               = &settingsBufferInfo;

        devFuncs->vkUpdateDescriptorSets(device, 4, descWrite, 0, nullptr);
    }
}

void VulkanRenderer::createComputePipelineLayout()
{
    VkPipelineLayoutCreateInfo pipelineLayoutInfo   = {};
    pipelineLayoutInfo.sType                        = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount               = 1;
    pipelineLayoutInfo.pSetLayouts                  = &computeDescriptorSetLayoutGeneric;
    pipelineLayoutInfo.pushConstantRangeCount       = 0;
    pipelineLayoutInfo.pPushConstantRanges          = nullptr;

    //Create the layout, store it to share between shaders
    VkResult err = devFuncs->vkCreatePipelineLayout(
                device,
                &pipelineLayoutInfo,
                nullptr,
                &computePipelineLayoutGeneric);
    if (err != VK_SUCCESS)
        qFatal("Failed to create compute pipeline layout: %d", err);
}


void VulkanRenderer::createComputePipelines()
{
    for (int i = 0; i != NODE_TYPE_MAX; i++)
    {
        NodeType nodeType = static_cast<NodeType>(i);

        pipelines[nodeType] = createComputePipeline(nodeType);
    }
}

VkPipeline VulkanRenderer::createComputePipeline(NodeType nodeType)
{
    auto shaderModule = shaders[nodeType];

    VkPipelineShaderStageCreateInfo computeStage = {

            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,
            VK_SHADER_STAGE_COMPUTE_BIT,
            shaderModule,
            "main",
            nullptr
        };

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = computeStage;
    pipelineInfo.layout = computePipelineLayoutGeneric;

    VkPipeline pl = VK_NULL_HANDLE;

    VkResult err = devFuncs->vkCreateComputePipelines(
                device,
                pipelineCache,
                1,
                &pipelineInfo,
                nullptr,
                &pl);
    if (err != VK_SUCCESS)
        qFatal("Failed to create compute pipeline: %d", err);

    if (shaderModule)
        devFuncs->vkDestroyShaderModule(device, shaderModule, nullptr);

    return pl;
}

VkPipeline VulkanRenderer::createComputePipelineNoop()
{
    auto shaderModule = createShaderFromFile(":/shaders/noop_comp.spv");;

    VkPipelineShaderStageCreateInfo computeStage = {

            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,
            VK_SHADER_STAGE_COMPUTE_BIT,
            shaderModule,
            "main",
            nullptr
        };

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = computeStage;
    pipelineInfo.layout = computePipelineLayoutGeneric;

    VkPipeline pl = VK_NULL_HANDLE;

    VkResult err = devFuncs->vkCreateComputePipelines(
                device,
                pipelineCache,
                1,
                &pipelineInfo,
                nullptr,
                &pl);
    if (err != VK_SUCCESS)
        qFatal("Failed to create compute pipeline: %d", err);

    if (shaderModule)
        devFuncs->vkDestroyShaderModule(device, shaderModule, nullptr);

    return pl;
}

void VulkanRenderer::createComputeQueue()
{
    VkPhysicalDevice physicalDevice = window->physicalDevice();

    uint32_t queueFamilyCount;
    f->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, NULL);

    std::vector<VkQueueFamilyProperties> queueFamilyProperties;
    queueFamilyProperties.resize(queueFamilyCount);

    f->vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties.data());

    for (auto i = 0U; i < queueFamilyProperties.size(); ++i)
    {
        if (queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            compute.queueFamilyIndex = i;
            break;
        }
    }

    // Get a compute queue from the device
    devFuncs->vkGetDeviceQueue(device, compute.queueFamilyIndex, 0, &compute.computeQueue);
}

void VulkanRenderer::createComputeCommandPool()
{
    // Separate command pool as queue family for compute may be different than graphics
    VkCommandPoolCreateInfo cmdPoolInfo = {};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = compute.queueFamilyIndex;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult err;

    err = devFuncs->vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &compute.computeCommandPool);

    if (err != VK_SUCCESS)
        qFatal("Failed to create compute command pool: %d", err);
}

void VulkanRenderer::createQueryPool()
{
    VkQueryPoolCreateInfo queryPooloolInfo = {};
    queryPooloolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPooloolInfo.pNext = nullptr;
    queryPooloolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPooloolInfo.queryCount = 2;

    VkResult err = devFuncs->vkCreateQueryPool(device, &queryPooloolInfo, nullptr, &queryPool);
    if (err != VK_SUCCESS)
        qFatal("Failed to create query pool: %d", err);
}

void VulkanRenderer::createComputeCommandBuffers()
{
    // Create the command buffer for loading an image from disk
    VkCommandBufferAllocateInfo commandBufferAllocateInfo {};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = compute.computeCommandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 3;

    VkCommandBuffer buffers[3] = {};
    VkResult err = devFuncs->vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, &buffers[0]);

    if (err != VK_SUCCESS)
        qFatal("Failed to allocate descriptor set: %d", err);

    compute.commandBufferImageLoad = buffers[0];
    compute.commandBufferGeneric = buffers[1];
    compute.commandBufferImageSave = buffers[2];

    // Fence for compute CB sync
    VkFenceCreateInfo fenceCreateInfo = {};
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    err = devFuncs->vkCreateFence(device, &fenceCreateInfo, nullptr, &compute.fence);

    if (err != VK_SUCCESS)
        qFatal("Failed to create fence: %d", err);


}

void VulkanRenderer::recordComputeCommandBufferImageLoad(
        std::shared_ptr<CsImage> outputImage)
{
    devFuncs->vkQueueWaitIdle(compute.computeQueue);

    VkCommandBufferBeginInfo cmdBufferBeginInfo {};
    cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VkResult err = devFuncs->vkBeginCommandBuffer(
                compute.commandBufferImageLoad,
                &cmdBufferBeginInfo);

    if (err != VK_SUCCESS)
        qFatal("Failed to begin command buffer: %d", err);

    VkCommandBuffer cb = compute.commandBufferImageLoad;

    VkImageMemoryBarrier barrier = {};
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;

    barrier.oldLayout       = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier.newLayout       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask   = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask   = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image           = loadImageStaging;

    devFuncs->vkCmdPipelineBarrier(cb,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr,
                            1, &barrier);

    barrier.oldLayout       = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask   = 0;
    barrier.dstAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image           = imageFromDisk->getImage();

    devFuncs->vkCmdPipelineBarrier(cb,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            0,
                            nullptr,
                            0,
                            nullptr,
                            1, &barrier);

    VkImageCopy copyInfo;
    memset(&copyInfo, 0, sizeof(copyInfo));
    copyInfo.srcSubresource.aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT;
    copyInfo.srcSubresource.layerCount  = 1;
    copyInfo.dstSubresource.aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT;
    copyInfo.dstSubresource.layerCount  = 1;
    copyInfo.extent.width               = loadImageSize.width();
    copyInfo.extent.height              = loadImageSize.height();
    copyInfo.extent.depth               = 1;

    devFuncs->vkCmdCopyImage(
                cb,
                loadImageStaging,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                imageFromDisk->getImage(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &copyInfo);

    {
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.image         = imageFromDisk->getImage();

        devFuncs->vkCmdPipelineBarrier(cb,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 0, nullptr, 0, nullptr,
                                1, &barrier);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.image         = outputImage->getImage();

        devFuncs->vkCmdPipelineBarrier(cb,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0,
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &barrier);
    }

    devFuncs->vkCmdBindPipeline(
                compute.commandBufferImageLoad,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                pipelines[NODE_TYPE_READ]);
    devFuncs->vkCmdBindDescriptorSets(
                compute.commandBufferImageLoad,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                computePipelineLayoutGeneric,
                0,
                1,
                &computeDescriptorSetGeneric,
                0,
                0);
    devFuncs->vkCmdDispatch(
                compute.commandBufferImageLoad,
                cpuImage->xend() / 16 + 1,
                cpuImage->yend() / 16 + 1, 1);

    {
        VkImageMemoryBarrier barrier[2] = {};

        barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier[0].subresourceRange.levelCount = barrier[0].subresourceRange.layerCount = 1;

        barrier[0].oldLayout       = VK_IMAGE_LAYOUT_GENERAL;
        barrier[0].newLayout       = VK_IMAGE_LAYOUT_GENERAL;
        barrier[0].srcAccessMask   = 0;
        barrier[0].dstAccessMask   = 0;
        barrier[0].image           = imageFromDisk->getImage();

        barrier[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier[1].subresourceRange.levelCount = barrier[1].subresourceRange.layerCount = 1;

        barrier[1].oldLayout       = VK_IMAGE_LAYOUT_GENERAL;
        barrier[1].newLayout       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier[1].srcAccessMask   = 0;
        barrier[1].dstAccessMask   = 0;
        barrier[1].image           = outputImage->getImage();

        devFuncs->vkCmdPipelineBarrier(cb,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr,
                            2, &barrier[0]);
    }

    devFuncs->vkEndCommandBuffer(compute.commandBufferImageLoad);
}

bool VulkanRenderer::createTextureImage(const QSize &size,
                vk::UniqueImage &image,
                vk::UniqueDeviceMemory &mem,
                vk::ImageTiling tiling,
                vk::ImageUsageFlags usage,
                uint32_t memIndex)
{
    vk::ImageCreateInfo imageInfo({},
                                  vk::ImageType::e2D,
                                  globalImageFormat,
                                  { (uint32_t)size.width(), (uint32_t)size.height(), 1 },
                                  1,
                                  1,
                                  vk::SampleCountFlagBits::e1,
                                  tiling,
                                  usage);
    image = device.createImageUnique(imageInfo);


    vk::MemoryRequirements memReq = device.getImageMemoryRequirements(*image, memReq);

    if (!(memReq.memoryTypeBits & (1 << memIndex)))
    {
        vk::PhysicalDeviceMemoryProperties physDevMemProps = physicalDevice.getMemoryProperties();
        for (uint32_t i = 0; i < physDevMemProps.memoryTypeCount; ++i)
        {
            if (!(memReq.memoryTypeBits & (1 << i)))
                continue;
            memIndex = i;
        }
    }

    vk::MemoryAllocateInfo allocInfo(memReq.size, memIndex);

    CS_LOG_INFO("Allocating texture image:");
    CS_LOG_INFO(QString::number(uint32_t(memReq.size)) + " bytes");
    mem = device.allocateMemoryUnique(allocInfo);

    device.bindImageMemory(*image, *mem, 0);

    return true;
}

bool VulkanRenderer::writeLinearImage(float* imgStart,
        QSize imgSize,
        vk::UniqueImage &image,
        vk::UniqueDeviceMemory &memory)
{
    vk::ImageSubresource subres(
                vk::ImageAspectFlagBits::eColor,
                0, // mip level
                0);

    vk::SubresourceLayout layout = device.getImageSubresourceLayout(*image, subres);

    float *p;
    vk::Result err = device.mapMemory(*memory,
                                      layout.offset,
                                      layout.size,
                                      {},
                                      reinterpret_cast<void **>(&p));
    if (err != vk::Result::eSuccess) {
        CS_LOG_WARNING("Failed to map memory for linear image.");
        return false;
    }

    int pad = (layout.rowPitch - imgSize.width() * 16) / 4;

    // TODO: Parallelize this
    float* pixels = imgStart;
    int lineWidth = imgSize.width() * 16; // 4 channels * 4 bytes
    for (int y = 0; y < imgSize.height(); ++y)
    {
        memcpy(p, pixels, lineWidth);
        pixels += imgSize.width() * 4;
        p += imgSize.width() * 4 + pad;
    }

    device.unmapMemory(*memory);

    return true;
}

void VulkanRenderer::updateVertexData(const int w, const int h)
{
    vertexData[0]  = -0.002 * w;
    vertexData[5]  = -0.002 * w;
    vertexData[10] = 0.002 * w;
    vertexData[15] = 0.002 * w;
    vertexData[1]  = -0.002 * h;
    vertexData[6]  = 0.002 * h;
    vertexData[11] = -0.002 * h;
    vertexData[16] = 0.002 * h;
}

void VulkanRenderer::initSwapChainResources()
{
    CS_LOG_INFO("Initializing swapchain resources.");

    // Projection matrix
    projection = window->clipCorrectionMatrix(); // adjust for Vulkan-OpenGL clip space differences
    const QSize sz = window->swapChainImageSize();
    projection.ortho( -sz.width() / scaleXY, sz.width() / scaleXY, -sz.height() / scaleXY, sz.height() / scaleXY, -1.0f, 100.0f);
    projection.scale(500);
}

void VulkanRenderer::recordComputeCommandBufferGeneric(
        std::shared_ptr<CsImage> inputImageBack,
        std::shared_ptr<CsImage> inputImageFront,
        std::shared_ptr<CsImage> outputImage,
        VkPipeline& pl,
        int numShaderPasses,
        int currentShaderPass)
{
    devFuncs->vkQueueWaitIdle(compute.computeQueue);

    VkCommandBufferBeginInfo cmdBufferBeginInfo {};
    cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VkResult err = devFuncs->vkBeginCommandBuffer(
                compute.commandBufferGeneric,
                &cmdBufferBeginInfo);
    if (err != VK_SUCCESS)
        qFatal("Failed to begin command buffer: %d", err);

    if (inputImageFront && inputImageFront != inputImageBack)
    {
       VkImageMemoryBarrier barrier[3] = {};

       createImageMemoryBarrier(
               barrier[0],
               VK_IMAGE_LAYOUT_GENERAL,
               0,
               0,
               *inputImageBack);

       createImageMemoryBarrier(
               barrier[1],
               VK_IMAGE_LAYOUT_GENERAL,
               0,
               0,
               *inputImageFront);

       createImageMemoryBarrier(
               barrier[2],
               VK_IMAGE_LAYOUT_GENERAL,
               0,
               0,
               *outputImage);

       devFuncs->vkCmdPipelineBarrier(compute.commandBufferGeneric,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0,
                               0,
                               nullptr,
                               0,
                               nullptr,
                               3,
                               &barrier[0]);
    }
    else
    {
        VkImageMemoryBarrier barrier[2] = {};

        createImageMemoryBarrier(
                barrier[0],
                VK_IMAGE_LAYOUT_GENERAL,
                0,
                0,
                *inputImageBack);

        createImageMemoryBarrier(
                barrier[1],
                VK_IMAGE_LAYOUT_GENERAL,
                0,
                0,
                *outputImage);

        devFuncs->vkCmdPipelineBarrier(compute.commandBufferGeneric,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0,
                                0,
                                nullptr,
                                0,
                                nullptr,
                                2,
                                &barrier[0]);
    }

    devFuncs->vkCmdBindPipeline(
                compute.commandBufferGeneric,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                pl);
    devFuncs->vkCmdBindDescriptorSets(
                compute.commandBufferGeneric,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                computePipelineLayoutGeneric,
                0,
                1,
                &computeDescriptorSetGeneric,
                0,
                0);
    devFuncs->vkCmdDispatch(
                compute.commandBufferGeneric,
                outputImage->getWidth() / 16 + 1,
                outputImage->getHeight() / 16 + 1, 1);

    if (inputImageFront && inputImageFront != inputImageBack)
    {
        VkImageMemoryBarrier barrier[3] = {};

        createImageMemoryBarrier(
                barrier[0],
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                0,
                0,
                *inputImageBack);

        createImageMemoryBarrier(
                barrier[1],
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                0,
                0,
                *inputImageFront);

        auto layout = VK_IMAGE_LAYOUT_GENERAL;
        if (currentShaderPass == numShaderPasses)
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        createImageMemoryBarrier(
                barrier[2],
                layout,
                0,
                0,
                *outputImage);

        devFuncs->vkCmdPipelineBarrier(compute.commandBufferGeneric,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0,
                            0,
                            nullptr,
                            0,
                            nullptr,
                            3,
                            &barrier[0]);
    }
    else
    {
        VkImageMemoryBarrier barrier[2] = {};

        createImageMemoryBarrier(
                barrier[0],
                VK_IMAGE_LAYOUT_GENERAL,
                0,
                0,
                *inputImageBack);

        auto layout = VK_IMAGE_LAYOUT_GENERAL;
        if (currentShaderPass == numShaderPasses)
            layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        createImageMemoryBarrier(
                barrier[1],
                layout,
                0,
                0,
                *outputImage);

        devFuncs->vkCmdPipelineBarrier(compute.commandBufferGeneric,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0,
                            0,
                            nullptr,
                            0,
                            nullptr,
                            2,
                            &barrier[0]);
    }

    devFuncs->vkEndCommandBuffer(compute.commandBufferGeneric);
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties)
{
    vk::PhysicalDeviceMemoryProperties memProperties = physicalDevice.getMemoryProperties(memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type!");
}

void VulkanRenderer::recordComputeCommandBufferCPUCopy(
        CsImage& inputImage)
{
    CS_LOG_INFO("Copying image GPU-->CPU.");

    // This is for outputting an image to the CPU
    devFuncs->vkQueueWaitIdle(compute.computeQueue);

    VkCommandBufferBeginInfo cmdBufferBeginInfo {};
    cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VkResult err = devFuncs->vkBeginCommandBuffer(
                compute.commandBufferImageSave,
                &cmdBufferBeginInfo);
    if (err != VK_SUCCESS)
        qFatal("Failed to begin command buffer: %d", err);

    VkCommandBuffer cb = compute.commandBufferImageSave;

    outputImageSize = QSize(inputImage.getWidth(), inputImage.getHeight());

    VkDeviceSize bufferSize = outputImageSize.width() * outputImageSize.height() * 16; // 4 channels * 4 bytes

    createBuffer(outputStagingBuffer, outputStagingBufferMemory, bufferSize);

    {
        VkImageMemoryBarrier barrier = {};

        createImageMemoryBarrier(
                barrier,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0,
                0,
                inputImage);

        devFuncs->vkCmdPipelineBarrier(cb,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0,
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &barrier);
    }

    VkImageSubresourceLayers imageLayers =
    {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        0,
        1
    };

    VkBufferImageCopy copyInfo;
    copyInfo.bufferOffset       = 0;
    copyInfo.bufferRowLength    = outputImageSize.width();
    copyInfo.bufferImageHeight  = outputImageSize.height();
    copyInfo.imageSubresource   = imageLayers;
    copyInfo.imageOffset        = { 0, 0, 0 };
    copyInfo.imageExtent.width  = outputImageSize.width();
    copyInfo.imageExtent.height = outputImageSize.height();
    copyInfo.imageExtent.depth  = 1;

    devFuncs->vkCmdCopyImageToBuffer(
                cb,
                inputImage.getImage(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                outputStagingBuffer,
                1,
                &copyInfo);

    {
        VkImageMemoryBarrier barrier = {};

        createImageMemoryBarrier(
                barrier,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT,
                0,
                inputImage);

        devFuncs->vkCmdPipelineBarrier(cb,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            0,
                            nullptr,
                            0,
                            nullptr,
                            1,
                            &barrier);
    }

    devFuncs->vkEndCommandBuffer(compute.commandBufferImageSave);
}

void VulkanRenderer::setDisplayMode(DisplayMode mode)
{
    displayMode = mode;
}

bool VulkanRenderer::saveImageToDisk(CsImage& inputImage, const QString &path, const int colorSpace)
{
    bool success = true;

    recordComputeCommandBufferCPUCopy(inputImage);
    submitImageSaveCommand();

    devFuncs->vkQueueWaitIdle(compute.computeQueue);

    float *pInput;
    VkResult err = devFuncs->vkMapMemory(
                device,
                outputStagingBufferMemory,
                0,
                VK_WHOLE_SIZE,
                0,
                reinterpret_cast<void **>(&pInput));
    if (err != VK_SUCCESS)
    {
        CS_LOG_WARNING("Failed to map memory for staging buffer:");
        CS_LOG_WARNING(QString::number(err));
    }

    int width = inputImage.getWidth();
    int height = inputImage.getHeight();
    int numValues = width * height * 4;

    float* output = new float[numValues];
    float* pOutput = &output[0];

    parallelArrayCopy(pInput, pOutput, width, height);

    ImageSpec spec(width, height, 4, TypeDesc::FLOAT);
    std::unique_ptr<ImageBuf> saveImage =
            std::unique_ptr<ImageBuf>(new ImageBuf(spec, output));

    transformColorSpace("linear", lookupColorSpace(colorSpace), *saveImage);

    success = saveImage->write(path.toStdString());

    if (!success)
    {
        CS_LOG_INFO("Problem saving image." + QString::fromStdString(saveImage->geterror()));
    }

    delete[] output;

    devFuncs->vkUnmapMemory(device, outputStagingBufferMemory);

    return success;
}

void VulkanRenderer::createRenderPass()
{
    CS_LOG_INFO("Creating Render Pass.");

    VkCommandBuffer cb = window->currentCommandBuffer();

    const QSize sz = window->swapChainImageSize();

    std::cout << "Swapchain image width: " << sz.width() << std::endl;
    std::cout << "Swapchain image height: " << sz.height() << std::endl;

    // Clear background
    VkClearDepthStencilValue clearDS = { 1, 0 };
    VkClearValue clearValues[2];
    memset(clearValues, 0, sizeof(clearValues));
    clearValues[0].color = clearColor;
    clearValues[1].depthStencil = clearDS;

    VkRenderPassBeginInfo rpBeginInfo;
    memset(&rpBeginInfo, 0, sizeof(rpBeginInfo));
    rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBeginInfo.renderPass = window->defaultRenderPass();
    rpBeginInfo.framebuffer = window->currentFramebuffer();
    rpBeginInfo.renderArea.extent.width = sz.width();
    rpBeginInfo.renderArea.extent.height = sz.height();
    rpBeginInfo.clearValueCount = 2;
    rpBeginInfo.pClearValues = clearValues;
    VkCommandBuffer cmdBuf = window->currentCommandBuffer();
    devFuncs->vkCmdBeginRenderPass(cmdBuf, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    // TODO: Can we do this once?
    quint8 *p;
    VkResult err = devFuncs->vkMapMemory(
                device,
                vertexBufferMemory,
                uniformBufferInfo[window->currentFrame()].offset,
                UNIFORM_DATA_SIZE,
                0,
                reinterpret_cast<void **>(&p));
    if (err != VK_SUCCESS)
        qFatal("Failed to map memory: %d", err);

    QMatrix4x4 m = projection;

    QMatrix4x4 rotation;
    rotation.setToIdentity();

    QMatrix4x4 translation;
    translation.setToIdentity();
    translation.translate(position_x, position_y, position_z);

    QMatrix4x4 scale;
    scale.setToIdentity();
    scale.scale(scaleXY, scaleXY, scaleXY);

    m = m * translation * scale;

    memcpy(p, m.constData(), 16 * sizeof(float));
    devFuncs->vkUnmapMemory(device, vertexBufferMemory);

    // Choose to either display RGB or Alpha
    VkPipeline pl;
    if (displayMode == DISPLAY_MODE_ALPHA)
        pl = graphicsPipelineAlpha;
    else
        pl = graphicsPipelineRGB;

    devFuncs->vkCmdPushConstants(
                cb,
                graphicsPipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(viewerPushConstants),
                viewerPushConstants.data());
    devFuncs->vkCmdBindPipeline(
                cb,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pl);
    devFuncs->vkCmdBindDescriptorSets(
                cb,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                graphicsPipelineLayout,
                0,
                1,
                &graphicsDescriptorSet[window->currentFrame()],
                0,
                nullptr);
    VkDeviceSize vbOffset = 0;
    devFuncs->vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &vbOffset);

    //negative viewport
    VkViewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = sz.width();
    viewport.height = sz.height();
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    devFuncs->vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor;
    scissor.offset.x = scissor.offset.y = 0;
    scissor.extent.width = viewport.width;
    scissor.extent.height = viewport.height;
    devFuncs->vkCmdSetScissor(cb, 0, 1, &scissor);

    devFuncs->vkCmdDraw(cb, 4, 1, 0, 0);

    devFuncs->vkCmdEndRenderPass(cmdBuf);
}

void VulkanRenderer::submitComputeCommands()
{
    // Submit compute commands
    // Use a fence to ensure that compute command buffer has finished executing before using it again
    devFuncs->vkWaitForFences(device, 1, &compute.fence, VK_TRUE, UINT64_MAX);
    devFuncs->vkResetFences(device, 1, &compute.fence);

    // Do the copy on the compute queue
    if (texStagingPending)
    {
        texStagingPending = false;
        VkSubmitInfo computeSubmitInfo {};
        computeSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        computeSubmitInfo.commandBufferCount = 1;
        computeSubmitInfo.pCommandBuffers = &compute.commandBufferImageLoad;
        devFuncs->vkQueueSubmit(compute.computeQueue, 1, &computeSubmitInfo, compute.fence);
    }
    else
    {
        VkSubmitInfo computeSubmitInfo {};
        computeSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        computeSubmitInfo.commandBufferCount = 1;
        computeSubmitInfo.pCommandBuffers = &compute.commandBufferGeneric;
        devFuncs->vkQueueSubmit(compute.computeQueue, 1, &computeSubmitInfo, compute.fence);
    }
}

void VulkanRenderer::submitImageSaveCommand()
{
    // Use a fence to ensure that compute command buffer has finished executing before using it again
    devFuncs->vkWaitForFences(device, 1, &compute.fence, VK_TRUE, UINT64_MAX);
    devFuncs->vkResetFences(device, 1, &compute.fence);

    VkSubmitInfo computeSubmitInfo {};
    computeSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    computeSubmitInfo.commandBufferCount = 1;

    computeSubmitInfo.pCommandBuffers = &compute.commandBufferImageSave;

    devFuncs->vkQueueSubmit(compute.computeQueue, 1, &computeSubmitInfo, compute.fence);
}

std::vector<float> VulkanRenderer::unpackPushConstants(const QString s)
{
    std::vector<float> values;
    auto parts = s.split(",");
    foreach(const QString& part, parts)
    {
        values.push_back(part.toFloat());
    }
    return values;
}

void VulkanRenderer::setViewerPushConstants(const QString &s)
{
    viewerPushConstants = unpackPushConstants(s);
}

void VulkanRenderer::processReadNode(NodeBase *node)
{
    auto parts = node->getAllPropertyValues().split(",");
    int index = parts[parts.size() - 2].toInt();
    if ( index < 0 )
        index = 0;
    QString path = parts[index];
    int colorSpace = parts.last().toInt();

    if(path != "")
    {
        imagePath = path;

        // Create texture
        if (!createTextureFromFile(imagePath, colorSpace))
            CS_LOG_WARNING("Failed to create texture");

        // Update the projection size
        createVertexBuffer();

        // Create render target
        if (!createComputeRenderTarget(cpuImage->xend(), cpuImage->yend()))
            CS_LOG_WARNING("Failed to create compute render target.");

        updateComputeDescriptors(imageFromDisk, nullptr, computeRenderTarget);

        recordComputeCommandBufferImageLoad(computeRenderTarget);

        submitComputeCommands();

        CS_LOG_INFO("Moving render target.");

        node->cachedImage = std::move(computeRenderTarget);
    }
}

void VulkanRenderer::processNode(
        NodeBase* node,
        std::shared_ptr<CsImage> inputImageBack,
        std::shared_ptr<CsImage> inputImageFront,
        const QSize targetSize)
{

    fillSettingsBuffer(node);

    if (currentRenderSize != targetSize)
    {
        updateVertexData(targetSize.width(), targetSize.height());
        createVertexBuffer();
    }

    if (!createComputeRenderTarget(targetSize.width(), targetSize.height()))
        qFatal("Failed to create compute render target.");

    // Tells the shader if we have a mask on the front input
    settingsBuffer->appendValue(0.0);
    if (inputImageFront)
    {
        settingsBuffer->incrementLastValue();
    }

    // TODO: This is a workaround for generative nodes without input
    // but should not be necessary
    if (!inputImageBack)
    {
        inputImageBack = std::shared_ptr<CsImage>(new CsImage(
                                                      window,
                                                      &device,
                                                      devFuncs,
                                                      targetSize.width(),
                                                      targetSize.height()));
    }

    int numShaderPasses = getPropertiesForType(node->nodeType).numShaderPasses;
    int currentShaderPass = 1;

    if (numShaderPasses == 1)
    {
        updateComputeDescriptors(inputImageBack, inputImageFront, computeRenderTarget);

        recordComputeCommandBufferGeneric(
                    inputImageBack,
                    inputImageFront,
                    computeRenderTarget,
                    pipelines[node->nodeType],
                    numShaderPasses,
                    currentShaderPass);

        submitComputeCommands();

        window->requestUpdate();

        devFuncs->vkQueueWaitIdle(compute.computeQueue);

        node->cachedImage = std::move(computeRenderTarget);
    }
    else
    {
        for (int i = 1; i <= numShaderPasses; ++i)
        {
            // TODO: Shorten this
            if (currentShaderPass == 1)
            {
                // First pass of multipass shader
                settingsBuffer->appendValue(0.0);

                updateComputeDescriptors(inputImageBack, inputImageFront, computeRenderTarget);

                recordComputeCommandBufferGeneric(
                            inputImageBack,
                            inputImageFront,
                            computeRenderTarget,
                            pipelines[node->nodeType],
                            numShaderPasses,
                            currentShaderPass);

                submitComputeCommands();
            }
            else if (currentShaderPass <= numShaderPasses)
            {
                // Subsequent passes
                settingsBuffer->incrementLastValue();

                if (!createComputeRenderTarget(targetSize.width(), targetSize.height()))
                    qFatal("Failed to create compute render target.");

                updateComputeDescriptors(node->cachedImage, inputImageFront, computeRenderTarget);

                recordComputeCommandBufferGeneric(
                            node->cachedImage,
                            inputImageFront,
                            computeRenderTarget,
                            pipelines[node->nodeType],
                            numShaderPasses,
                            currentShaderPass);

                submitComputeCommands();
            }
            currentShaderPass++;

            devFuncs->vkQueueWaitIdle(compute.computeQueue);

            node->cachedImage = std::move(computeRenderTarget);
        }
        window->requestUpdate();
    }
}

void VulkanRenderer::displayNode(NodeBase *node)
{
    // TODO: Should probably use something like cmdBlitImage
    // instead of the hacky noop shader workaround
    // for displaying a node that has already been rendered
    if(auto image = node->cachedImage)
    {
        clearScreen = false;

        updateVertexData(image->getWidth(), image->getHeight());
        createVertexBuffer();

        if (!createComputeRenderTarget(image->getWidth(), image->getHeight()))
            qFatal("Failed to create compute render target.");

        updateComputeDescriptors(image, nullptr, computeRenderTarget);

        recordComputeCommandBufferGeneric(image, nullptr, computeRenderTarget, computePipelineNoop, 1, 1);

        submitComputeCommands();

        window->requestUpdate();
    }
    else
    {
        doClearScreen();
    }
}

void VulkanRenderer::doClearScreen()
{
    clearScreen = true;

    window->requestUpdate();
}

std::vector<char> uintVecToCharVec(const std::vector<unsigned int>& in)
{
    std::vector<char> out;

    for (size_t i = 0; i < in.size(); i++)
    {
        out.push_back(in[i] >> 0);
        out.push_back(in[i] >> 8);
        out.push_back(in[i] >> 16);
        out.push_back(in[i] >> 24);
    }

    return out;
}

void VulkanRenderer::startNextFrame()
{
    CS_LOG_INFO("Starting next frame.");

    if (clearScreen)
    {
        const QSize sz = window->swapChainImageSize();

        // Clear background
        VkClearDepthStencilValue clearDS = { 1, 0 };
        VkClearValue clearValues[2];
        memset(clearValues, 0, sizeof(clearValues));
        clearValues[0].color = clearColor;
        clearValues[1].depthStencil = clearDS;

        VkRenderPassBeginInfo rpBeginInfo;
        memset(&rpBeginInfo, 0, sizeof(rpBeginInfo));
        rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBeginInfo.renderPass = window->defaultRenderPass();
        rpBeginInfo.framebuffer = window->currentFramebuffer();
        rpBeginInfo.renderArea.extent.width = sz.width();
        rpBeginInfo.renderArea.extent.height = sz.height();
        rpBeginInfo.clearValueCount = 2;
        rpBeginInfo.pClearValues = clearValues;
        VkCommandBuffer cmdBuf = window->currentCommandBuffer();
        devFuncs->vkCmdBeginRenderPass(cmdBuf, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        devFuncs->vkCmdEndRenderPass(cmdBuf);
    }
    else
    {
        createRenderPass();
    }

    window->frameReady();
}

void VulkanRenderer::logicalDeviceLost()
{
    emit window->deviceLost();
}

void VulkanRenderer::translate(float dx, float dy)
{
    const QSize sz = window->size();

    position_x += 6.0 * dx / sz.width();
    position_y += 2.0 * -dy / sz.height();

    window->requestUpdate();
}

void VulkanRenderer::scale(float s)
{
    scaleXY = s;
    window->requestUpdate();
    emit window->requestZoomTextUpdate(s);
}

void VulkanRenderer::releaseSwapChainResources()
{
    CS_LOG_INFO("Releasing swapchain resources.");
}

void VulkanRenderer::cleanup()
{
    CS_LOG_INFO("Cleaning up renderer.");

    devFuncs->vkQueueWaitIdle(compute.computeQueue);

    if (settingsBuffer)
    {
        settingsBuffer = nullptr;
    }

    if (computeRenderTarget)
    {
        computeRenderTarget->destroy();
        computeRenderTarget = nullptr;
    }
    if (imageFromDisk)
    {
        imageFromDisk->destroy();
        imageFromDisk = nullptr;
    }

    if (queryPool) {
        CS_LOG_INFO("Destroying queryPool");
        devFuncs->vkDestroyQueryPool(device, queryPool, nullptr);
        queryPool = VK_NULL_HANDLE;
    }

    if (sampler) {
        CS_LOG_INFO("Destroying sampler");
        devFuncs->vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }

    if (loadImageStaging) {
        CS_LOG_INFO("Destroying loadImageStaging");
        devFuncs->vkDestroyImage(device, loadImageStaging, nullptr);
        loadImageStaging = VK_NULL_HANDLE;
    }

    if (loadImageStagingMem) {
        CS_LOG_INFO("Destroying loadImageStagingMem");
        devFuncs->vkFreeMemory(device, loadImageStagingMem, nullptr);
        loadImageStagingMem = VK_NULL_HANDLE;
    }

    if (graphicsPipelineAlpha) {
        CS_LOG_INFO("Destroying graphicsPipelineAlpha");
        devFuncs->vkDestroyPipeline(device, graphicsPipelineAlpha, nullptr);
        graphicsPipelineAlpha = VK_NULL_HANDLE;
    }

    if (graphicsPipelineRGB) {
        CS_LOG_INFO("Destroying graphicsPipelineRGB");
        devFuncs->vkDestroyPipeline(device, graphicsPipelineRGB, nullptr);
        graphicsPipelineRGB = VK_NULL_HANDLE;
    }

    if (graphicsPipelineLayout) {
        CS_LOG_INFO("Destroying graphicsPipelineLayout");
        devFuncs->vkDestroyPipelineLayout(device, graphicsPipelineLayout, nullptr);
        graphicsPipelineLayout = VK_NULL_HANDLE;
    }

    if (pipelineCache) {
        CS_LOG_INFO("Destroying pipelineCache");
        devFuncs->vkDestroyPipelineCache(device, pipelineCache, nullptr);
        pipelineCache = VK_NULL_HANDLE;
    }

    if (graphicsDescriptorSetLayout) {
        CS_LOG_INFO("Destroying graphicsDescriptorSetLayout");
        devFuncs->vkDestroyDescriptorSetLayout(device, graphicsDescriptorSetLayout, nullptr);
        graphicsDescriptorSetLayout = VK_NULL_HANDLE;
    }

    if (descriptorPool) {
        CS_LOG_INFO("Destroying descriptorPool");
        devFuncs->vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
    }

    if (outputStagingBuffer) {
        CS_LOG_INFO("Destroying outputStagingBuffer");
        devFuncs->vkDestroyBuffer(device, outputStagingBuffer, nullptr);
        outputStagingBuffer = VK_NULL_HANDLE;
    }

    if (outputStagingBufferMemory) {
        CS_LOG_INFO("Destroying outputStagingBufferMemory");
        devFuncs->vkFreeMemory(device, outputStagingBufferMemory, nullptr);
        outputStagingBufferMemory = VK_NULL_HANDLE;
    }

    if (vertexBuffer) {
        CS_LOG_INFO("Destroying vertexBuffer");
        devFuncs->vkDestroyBuffer(device, vertexBuffer, nullptr);
        vertexBuffer = VK_NULL_HANDLE;
    }

    if (vertexBufferMemory) {
        CS_LOG_INFO("Destroying vertexBufferMemory");
        devFuncs->vkFreeMemory(device, vertexBufferMemory, nullptr);
        vertexBufferMemory = VK_NULL_HANDLE;
    }

    if (computeDescriptorSetLayoutGeneric) {
        CS_LOG_INFO("Destroying computeDescriptorSetLayoutGeneric");
        devFuncs->vkDestroyDescriptorSetLayout(device, computeDescriptorSetLayoutGeneric, nullptr);
        computeDescriptorSetLayoutGeneric = VK_NULL_HANDLE;
    }

    // Destroy compute pipelines
    if (computePipelineNoop) {
        CS_LOG_INFO("Destroying computePipelineNoop");
        devFuncs->vkDestroyPipeline(device, computePipelineNoop, nullptr);
        computePipelineNoop = VK_NULL_HANDLE;
    }

    if (computePipeline) {
        CS_LOG_INFO("Destroying computePipeline");
        devFuncs->vkDestroyPipeline(device, computePipeline, nullptr);
        computePipeline = VK_NULL_HANDLE;
    }

    foreach (auto pipeline, pipelines.keys())
    {
        if (pipelines.value(pipeline)) {
            CS_LOG_INFO("Destroying some pipeline");
            devFuncs->vkDestroyPipeline(device, pipelines.value(pipeline), nullptr);
        }
    }

    if (computePipelineLayoutGeneric) {
        CS_LOG_INFO("Destroying computePipelineLayoutGeneric");
        devFuncs->vkDestroyPipelineLayout(device, computePipelineLayoutGeneric, nullptr);
        computePipelineLayoutGeneric = VK_NULL_HANDLE;
    }

    if (compute.fence) {
        CS_LOG_INFO("Destroying fence");
        devFuncs->vkDestroyFence(device, compute.fence, nullptr);
        compute.fence = VK_NULL_HANDLE;
    }

    if (compute.computeCommandPool)
    {
        CS_LOG_INFO("Destroying commandBuffers");
        VkCommandBuffer buffers[3]=
        {
            compute.commandBufferImageLoad,
            compute.commandBufferImageSave,
            compute.commandBufferGeneric
        };
        devFuncs->vkFreeCommandBuffers(device, compute.computeCommandPool, 3, &buffers[0]);
        devFuncs->vkDestroyCommandPool(device, compute.computeCommandPool, nullptr);
    }

}

void VulkanRenderer::releaseResources()
{

}

VulkanRenderer::~VulkanRenderer()
{
    CS_LOG_INFO("Destroying Renderer.");
}
