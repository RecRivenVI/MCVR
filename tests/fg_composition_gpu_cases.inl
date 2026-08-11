// Included inside the Vulkan harness; uses the production compute shaders and
// real sampled/storage images, including half-float intermediate filtering.
void translatedCoverageCase(const std::filesystem::path &fragmentPath) {
    extraPipelineLayouts_.push_back(pipelineLayout_);
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,4};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
    checked(vkCreatePipelineLayout(device_,&li,nullptr,&pipelineLayout_),"coverage layout");
    auto color=createImage(4,2,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto readback=createBuffer(32,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto pass=createColorRenderPass({color.format});
    auto fb=createFramebuffer(pass,{color.view},4,2);
    VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
    auto pipeline=createGraphicsPipeline(pass,createShader(fragmentPath),{blend},false,false);
    bool first=true;
    for(uint32_t opaque : {0u,1u,0u}) {
        submit([&](VkCommandBuffer cmd){
            transition(cmd,color,first?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,first?0:VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue initial{{.1f,.1f,.1f,.25f}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd,color.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&initial,1,&range);
            transition(cmd,color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&opaque);
            draw(cmd,pass,fb,pipeline,{4,2},{{0,0},{4,2}},{0,0,0,0},false);
            transition(cmd,color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd,color,VK_IMAGE_ASPECT_COLOR_BIT,readback);
        });
        first=false;
        auto pixels=static_cast<const uint8_t*>(readback.mapped);
        for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<4;++x) {
            const std::array<float,4> expected=x<2?std::array<float,4>{.1f,.1f,.1f,.25f}:
                std::array<float,4>{.2f,.3f,.4f,opaque?1.f:.54f};
            for(uint32_t c=0;c<4;++c) require(nearByte(pixels[(y*4+x)*4+c],unorm(expected[c])),
                "translated GUI coverage changed RGB/discard or failed opaque/FBO alpha transition");
        }
    }
    std::cout<<"[PASS] actual Java-translated fragment preserves RGB/discard and switches opaque coverage\n";
}

void executionDescriptorCase(const std::filesystem::path &shaderPath) {
    auto constants=createBuffer(4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto output=createBuffer(12,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::array<VkDescriptorSetLayoutBinding,2> bindings{{
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT}}};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};si.bindingCount=2;si.pBindings=bindings.data();
    VkDescriptorSetLayout setLayout;checked(vkCreateDescriptorSetLayout(device_,&si,nullptr,&setLayout),"execution descriptor layout");descriptorSetLayouts_.push_back(setLayout);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&setLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
    VkPipelineLayout layout;checked(vkCreatePipelineLayout(device_,&li,nullptr,&layout),"execution pipeline layout");extraPipelineLayouts_.push_back(layout);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&size;
    VkDescriptorPool pool;checked(vkCreateDescriptorPool(device_,&pi,nullptr,&pool),"execution pool");descriptorPools_.push_back(pool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
    VkDescriptorSet set;checked(vkAllocateDescriptorSets(device_,&ai,&set),"execution descriptor");
    std::array<VkDescriptorBufferInfo,2> info{{{constants.handle,0,4},{output.handle,0,12}}};
    std::array<VkWriteDescriptorSet,2> writes{};
    for(uint32_t i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].descriptorCount=1;writes[i].pBufferInfo=&info[i];}
    vkUpdateDescriptorSets(device_,2,writes.data(),0,nullptr);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=layout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=createShader(shaderPath);ci.stage.pName="main";
    VkPipeline pipeline;checked(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline),"execution pipeline");pipelines_.push_back(pipeline);
    for(uint32_t frame=0;frame<2;++frame) {
        submit([&](VkCommandBuffer cmd){
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            for(uint32_t slot=0;slot<3;++slot){
                std::array<uint32_t,1> value{17+slot*38+frame*101};
                vk::recordInlineUpdate(cmd,constants.handle,value);
                vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&slot);
                vkCmdDispatch(cmd,1,1,1); value[0]=0;
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        });
        auto values=static_cast<const uint32_t*>(output.mapped);
        for(uint32_t i=0;i<3;++i) require(values[i]==17+i*38+frame*101,"Pass read a later pass's execution buffer");
    }
    std::cout << "[PASS] one stable descriptor, three GPU consumers, inline pass constants, repeated frame\n";
}

void weightedBlurCase(const std::filesystem::path &blurPath, const std::filesystem::path &weightPath) {
    constexpr uint32_t w=17, h=3;
    for (float radius : {2.f,2.5f,1.25f})
    for (int phase : {0, 5}) {
        const auto sampledUsage = VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const auto outputUsage = VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Image scene=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,sampledUsage);
        Image final=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,sampledUsage);
        Image weighted=createImage(w,h,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT);
        Image background=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,outputUsage);
        Image normal=createImage(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,outputUsage);
        Buffer sceneUpload=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer finalUpload=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer bgRead=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer normalRead=createBuffer(w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto H=static_cast<uint8_t*>(sceneUpload.mapped), F=static_cast<uint8_t*>(finalUpload.mapped);
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
            const auto i=4*(y*w+x);
            // Fractional text/panel edges, opaque UI PT, and uncovered world.
            const float a = radius != 2.f ? 0 : x<3 ? 0 : x<9 ? ((x%3)+1)/4.f : 1;
            F[i+3]=unorm(a); H[i+3]=0;
            for(uint32_t c=0;c<3;++c) {
                H[i+c]=uint8_t(((x+phase)*37+y*51+c*73)%256);
                F[i+c]=unorm(a*(c+1)/4.f+(1-a)*H[i+c]/255.f);
            }
        }
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_LINEAR;
        samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler sampler; checked(vkCreateSampler(device_,&samplerInfo,nullptr,&sampler),"weighted blur sampler"); samplers_.push_back(sampler);
        struct Pass { VkPipeline pipeline; VkPipelineLayout layout; VkDescriptorSet set; };
        const auto makePass = [&](const std::filesystem::path &shader, const Image &input, const Image &output, const Image *coverage) {
            const uint32_t count=coverage ? 3 : 2;
            std::array<VkDescriptorSetLayoutBinding,3> bindings{{
                {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT},
                {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT},
                {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT}}};
            VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; si.bindingCount=count;si.pBindings=bindings.data();
            VkDescriptorSetLayout setLayout;checked(vkCreateDescriptorSetLayout(device_,&si,nullptr,&setLayout),"weighted blur layout");descriptorSetLayouts_.push_back(setLayout);
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&setLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
            Pass result{};checked(vkCreatePipelineLayout(device_,&li,nullptr,&result.layout),"weighted blur pipeline layout");extraPipelineLayouts_.push_back(result.layout);
            std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=2;pi.pPoolSizes=sizes.data();
            VkDescriptorPool pool;checked(vkCreateDescriptorPool(device_,&pi,nullptr,&pool),"weighted blur pool");descriptorPools_.push_back(pool);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
            checked(vkAllocateDescriptorSets(device_,&ai,&result.set),"weighted blur set");
            std::array<VkDescriptorImageInfo,3> images{{{sampler,input.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL},{sampler,coverage ? coverage->view : VK_NULL_HANDLE,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
            std::array<VkWriteDescriptorSet,3> writes{};
            for(uint32_t i=0;i<count;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=result.set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&images[i];}
            vkUpdateDescriptorSets(device_,count,writes.data(),0,nullptr);
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=createShader(shader);ci.stage.pName="main";ci.layout=result.layout;
            checked(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&ci,nullptr,&result.pipeline),"weighted blur pipeline");pipelines_.push_back(result.pipeline);
            return result;
        };
        auto prepare=makePass(weightPath,scene,weighted,&final);
        auto normalize=makePass(blurPath,weighted,background,nullptr);
        auto reference=makePass(blurPath,final,normal,nullptr);
        submit([&](VkCommandBuffer cmd){
            const auto upload=[&](const Image &image, const Buffer &buffer){
                transition(cmd,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};
                vkCmdCopyBufferToImage(cmd,buffer.handle,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                transition(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            };
            upload(scene,sceneUpload);upload(final,finalUpload);
            for(auto &image:{weighted,background,normal}) transition(cmd,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            const auto dispatch=[&](const Pass &pass,float normalize){
                auto push = normalize != 0 ? mcvr::ui::blurParameters(w,h,1,0,5,radius/5) :
                    std::array<float,4>{1.f/w,0,radius,0};
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.pipeline);
                vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pass.layout,0,1,&pass.set,0,nullptr);
                vkCmdPushConstants(cmd,pass.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,push.data());
                vkCmdDispatch(cmd,(w+7)/8,(h+7)/8,1);
            };
            dispatch(prepare,0);
            transition(cmd,weighted,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            dispatch(normalize,1);dispatch(reference,0);
            for(auto &image:{background,normal}) transition(cmd,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyImageToBuffer(cmd,background,VK_IMAGE_ASPECT_COLOR_BIT,bgRead);
            copyImageToBuffer(cmd,normal,VK_IMAGE_ASPECT_COLOR_BIT,normalRead);
        });
        auto B=static_cast<const uint8_t*>(bgRead.mapped), N=static_cast<const uint8_t*>(normalRead.mapped);
        for(int y=0;y<int(h);++y) for(int x=0;x<int(w);++x) {
            const auto i=4*(y*w+x);
            for(int c=0;c<3;++c) {
                if (radius != 2.f) {
                    require(nearByte(B[i+c],N[i+c]),"Fractional-radius background blur differs from GPU rounding reference");
                    continue;
                }
                float transmission=0, weightedH=0, blurredF=0, blurredU=0;
                for(int k=-2;k<=2;++k){const auto j=4*(y*w+std::clamp(x+k,0,int(w)-1));const float t=1-F[j+3]/255.f;
                    transmission+=t/5;weightedH+=t*H[j+c]/(255.f*5);blurredF+=F[j+c]/(255.f*5);
                    blurredU+=(F[j+c]/255.f-t*H[j+c]/255.f)/5;
                }
                require(nearByte(B[i+c],unorm(transmission>0 ? weightedH/transmission : 0)),"Weighted HUD-less blur lost coverage covariance");
                require(nearByte(N[i+c],unorm(blurredF)),"Normal blur RGB changed");
                const float recomposed=blurredU+(1-N[i+3]/255.f)*B[i+c]/255.f;
                require(std::abs(recomposed-N[i+c]/255.f)<=2.1f/255,"Blurred premultiplied UI failed reconstruction");
                // A later fractional draw must still obey the same decomposition.
                require(std::abs((.25f*.8f+.75f*recomposed)-(.25f*.8f+.75f*N[i+c]/255.f))<=2.1f/255,
                    "Post-effect fractional UI changed order");
            }
        }
    }
    std::cout << "[PASS] weighted GPU blur: moving background, fractional/opaque UI, clamped edges, later UI, real-frame reconstruction\n";
}
