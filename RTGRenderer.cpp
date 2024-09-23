#ifdef _WIN32
//ensure we have M_PI
#define _USE_MATH_DEFINES
#endif

#ifndef GLFW_KEY_LEFT_SHIFT
	#define GLFW_KEY_LEFT_SHIFT 340
#endif
#include "RTGRenderer.hpp"

#include "VK.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <fstream>

RTGRenderer::RTGRenderer(RTG &rtg_, Scene &scene_) : rtg(rtg_), scene(scene_) {
	{ //create command pool
		VkCommandPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = rtg.graphics_queue_family.value(),
		};
		VK(vkCreateCommandPool(rtg.device, &create_info, nullptr, &command_pool));
	}

	//select a depth format:
	//	at least one of these two must be supported, according to the spec; but neither are required
	depth_format = rtg.helpers.find_image_format(
		{VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32},
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);

	{ //create render pass
		std::array<VkAttachmentDescription, 2> attachments{
			VkAttachmentDescription{//0 - color attachment:
				.format = rtg.surface_format.format,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			},
			VkAttachmentDescription{//1 - depth attachment:
				.format = depth_format,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			},
		};

		VkAttachmentReference color_attachment_ref{
			.attachment = 0,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};

		VkAttachmentReference depth_attachment_ref{
			.attachment = 1,
			.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};

		VkSubpassDescription subpass{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.inputAttachmentCount = 0,
			.pInputAttachments = nullptr,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment_ref,
			.pDepthStencilAttachment = &depth_attachment_ref,
		};

		//this defers the image load actions for the attachments:
		std::array<VkSubpassDependency, 2> dependencies{
			VkSubpassDependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL,
				.dstSubpass = 0,
				.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask = 0,
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			},
			VkSubpassDependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL,
				.dstSubpass = 0,
				.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			}
		};

		VkRenderPassCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = uint32_t(attachments.size()),
			.pAttachments = attachments.data(),
			.subpassCount = 1,
			.pSubpasses = &subpass,
			.dependencyCount = uint32_t(dependencies.size()),
			.pDependencies = dependencies.data(),
		};

		VK( vkCreateRenderPass(rtg.device, &create_info, nullptr, &render_pass) );
	}

	background_pipeline.create(rtg, render_pass, 0);
	lines_pipeline.create(rtg, render_pass, 0);
	objects_pipeline.create(rtg, render_pass, 0);

	{//create descriptor pool:
		uint32_t per_workspace = uint32_t(rtg.workspaces.size()); //for easier-to-read counting

		std::array< VkDescriptorPoolSize, 2> pool_sizes{
			VkDescriptorPoolSize{
				.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 2 * per_workspace, //one descriptor per set, two sets per workspace
			},
			VkDescriptorPoolSize{
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1 * per_workspace, //one descriptor per set, one set per workspace
			},
		};
		
		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, //because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, *can't* free individual descriptors allocated from this pool
			.maxSets = 3 * per_workspace, //three sets per workspace
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VK(vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &descriptor_pool));
	}

	workspaces.resize(rtg.workspaces.size());
	for (Workspace &workspace : workspaces) {
		{//allocate command buffer
			VkCommandBufferAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = command_pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			VK(vkAllocateCommandBuffers(rtg.device, &alloc_info, &workspace.command_buffer));
		}
	
		workspace.Camera_src = rtg.helpers.create_buffer(
			sizeof(LinesPipeline::Camera),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, //going to have GPU copy from this memory
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, //host-visible memory, coherent (no special sync needed)
			Helpers::Mapped //get a pointer to the memory
		);
		workspace.Camera = rtg.helpers.create_buffer(
			sizeof(LinesPipeline::Camera),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, //going to use as a uniform buffer, also going to have GPU copy into this memory
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //GPU-local memory
			Helpers::Unmapped //don't get a pointer to the memory
		);

		{//allocate descriptor set for Camera descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &lines_pipeline.set0_Camera,
			};

			VK(vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.Camera_descriptors));
		}

		workspace.World_src = rtg.helpers.create_buffer(
			sizeof(ObjectsPipeline::World),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			Helpers::Mapped
		);
		workspace.World = rtg.helpers.create_buffer(
			sizeof(ObjectsPipeline::World),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			Helpers::Unmapped
		);

		{ //allocate descriptor set for World descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set0_World,
			};

			VK(vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.World_descriptors));
			//NOTE: will actually fill in this descriptor set just a bit lower
		}

		{//allocate descriptor set for Transforms descriptor
			VkDescriptorSetAllocateInfo alloc_info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &objects_pipeline.set1_Transforms,
			};

			VK( vkAllocateDescriptorSets(rtg.device, &alloc_info, &workspace.Transforms_descriptors) );
			//NOTE: will fill in this descriptor set in render when buffers are [re-]allocated
		}

		{//point descriptor to Camera buffer:
			VkDescriptorBufferInfo Camera_info{
				.buffer = workspace.Camera.handle,
				.offset = 0,
				.range = workspace.Camera.size,
			};

			VkDescriptorBufferInfo World_info{
				.buffer = workspace.World.handle,
				.offset = 0,
				.range = workspace.World.size,
			};

			std::array< VkWriteDescriptorSet, 2 > writes{
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = workspace.Camera_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo = &Camera_info,
				},
				
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = workspace.World_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.pBufferInfo = &World_info,
				},
			};

			vkUpdateDescriptorSets(
				rtg.device, //device
				uint32_t(writes.size()), //descriptorWriteCount
				writes.data(), //pDescriptorWrites
				0, //descriptorCopyCount
				nullptr //pDescriptorCopies
			);
		}
	}

	{//create object vertices
		std::vector<PosNorTanTexVertex> vertices;
		vertices.resize(scene.vertices_count);
		uint32_t new_vertices_start = 0;
		mesh_vertices.clear();
		mesh_vertices.reserve(scene.meshes.size());
		for (uint32_t i = 0; i < uint32_t(scene.meshes.size()); ++i) {
			Scene::Mesh& cur_mesh = scene.meshes[i];
			mesh_vertices[i].count = cur_mesh.count;
			mesh_vertices[i].first = new_vertices_start;
			std::ifstream file(scene.scene_path + "/" + cur_mesh.attributes[0].source, std::ios::binary); // assuming the attribute layout holds
			if (!file.is_open()) throw std::runtime_error("Error opening file for mesh data: " + scene.scene_path + "/" + cur_mesh.attributes[0].source);
			if (!file.read(reinterpret_cast< char * >(&vertices[new_vertices_start]), cur_mesh.count * sizeof(PosNorTanTexVertex))) {
				throw std::runtime_error("Failed to read mesh data: " + scene.scene_path + "/" + cur_mesh.attributes[0].source);
			}
			new_vertices_start += cur_mesh.count;
		}
		assert(new_vertices_start == scene.vertices_count);

		size_t bytes = vertices.size() * sizeof(vertices[0]);

		object_vertices = rtg.helpers.create_buffer(
			bytes,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			Helpers::Unmapped
		);

		//copy data to buffer:
		rtg.helpers.transfer_to_buffer(vertices.data(), bytes, object_vertices);
	}

	{//make some textures
		textures.reserve(scene.textures.size() + 1); // index 0 is the default texture
		{//default material
			uint8_t data[4] = {255,255,255,255};
			//make a place for the texture to live on the GPU:
			textures.emplace_back(rtg.helpers.create_image(
				VkExtent2D{ .width = 1 , .height = 1 }, //size of image
				VK_FORMAT_R8G8B8A8_UNORM, //how to interpret image data (in this case, SRGB-encoded 8-bit RGBA)
				VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, //will sample and upload
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //should be device-local
				Helpers::Unmapped
			));

			//transfer data:
			rtg.helpers.transfer_to_image(&data, sizeof(uint8_t) * 4, textures.back());
		}

		for (uint32_t i = 0; i < scene.textures.size(); ++i) {
			Scene::Texture& cur_texture = scene.textures[i];
			if (cur_texture.has_src) {
				int width,height,n;
				unsigned char *image = stbi_load(cur_texture.source.c_str(), &width, &height, &n, 4);
				if (image == NULL) throw std::runtime_error("Error loading texture " + cur_texture.source);
				assert(n == 3); // should only be 3 channel per .s72 spec
				//make a place for the texture to live on the GPU:
				textures.emplace_back(rtg.helpers.create_image(
					VkExtent2D{ .width = uint32_t(width) , .height = uint32_t(height) }, //size of image
					VK_FORMAT_R8G8B8A8_UNORM, //how to interpret image data (in this case, linearly-encoded 8-bit RGBA) TODO: double check format
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, //will sample and upload
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //should be device-local
					Helpers::Unmapped
				));
				rtg.helpers.transfer_to_image(image, sizeof(image[0]) * width*height*n, textures.back());
				//free image:
				stbi_image_free(image);
			}
			else {
				uint8_t data[4] = {uint8_t(cur_texture.value.x*255.0f), uint8_t(cur_texture.value.y*255.0f), uint8_t(cur_texture.value.z*255.0f),255};
				//make a place for the texture to live on the GPU:
				textures.emplace_back(rtg.helpers.create_image(
					VkExtent2D{ .width = 1 , .height = 1 }, //size of image
					VK_FORMAT_R8G8B8A8_UNORM, //how to interpret image data (in this case, SRGB-encoded 8-bit RGBA)
					VK_IMAGE_TILING_OPTIMAL,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, //will sample and upload
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //should be device-local
					Helpers::Unmapped
				));

				//transfer data:
				rtg.helpers.transfer_to_image(&data, sizeof(uint8_t) * 4, textures.back());
			}
		}
	}

	{//make image views for the textures
		texture_views.reserve(textures.size());
		for (Helpers::AllocatedImage const &image : textures) {
			VkImageViewCreateInfo create_info{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.flags = 0,
				.image = image.handle,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = image.format,
				// .components sets swizzling and is fine when zero-initialized
				.subresourceRange{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			};

			VkImageView image_view = VK_NULL_HANDLE;
			VK(vkCreateImageView(rtg.device, &create_info, nullptr, &image_view));

			texture_views.emplace_back(image_view);
		}
		assert(texture_views.size() == textures.size());
	}

	{//make a sampler for the textures
		VkSamplerCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.flags = 0,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.mipLodBias = 0.0f,
			.anisotropyEnable = VK_FALSE,
			.maxAnisotropy = 0.0f, //doesn't matter if anisotropy isn't enabled
			.compareEnable = VK_FALSE,
			.compareOp = VK_COMPARE_OP_ALWAYS, //doesn't matter if compare isn't enabled
			.minLod = 0.0f,
			.maxLod = 0.0f,
			.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
			.unnormalizedCoordinates = VK_FALSE,
		};
		VK(vkCreateSampler(rtg.device, &create_info, nullptr, &texture_sampler));
	}
		
	{//create the texture descriptor pool
		uint32_t per_texture = uint32_t(textures.size()); //for easier-to-read counting

		std::array< VkDescriptorPoolSize, 1> pool_sizes{
			VkDescriptorPoolSize{
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1 * 1 * per_texture, //one descriptor per set, one set per texture
			},
		};
		
		VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0, //because CREATE_FREE_DESCRIPTOR_SET_BIT isn't included, *can't* free individual descriptors allocated from this pool
			.maxSets = 1 * per_texture, //one set per texture
			.poolSizeCount = uint32_t(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VK(vkCreateDescriptorPool(rtg.device, &create_info, nullptr, &texture_descriptor_pool));
	}

	{//allocate and write the texture descriptor sets
		//allocate the descriptors (using the same alloc_info):
		VkDescriptorSetAllocateInfo alloc_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = texture_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &objects_pipeline.set2_TEXTURE,
		};
		texture_descriptors.assign(textures.size(), VK_NULL_HANDLE);

		for (VkDescriptorSet &descriptor_set : texture_descriptors) {
			VK(vkAllocateDescriptorSets(rtg.device, &alloc_info, &descriptor_set));
		}

		//write descriptors for textures:
		std::vector< VkDescriptorImageInfo > infos(textures.size());
		std::vector< VkWriteDescriptorSet > writes(textures.size());

		for (Helpers::AllocatedImage const &image : textures) {
			size_t i = &image - &textures[0];
			
			infos[i] = VkDescriptorImageInfo{
				.sampler = texture_sampler,
				.imageView = texture_views[i],
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			writes[i] = VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = texture_descriptors[i],
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &infos[i],
			};
		}

		vkUpdateDescriptorSets(rtg.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
	}

	
	{ //setup camera
		float x = user_camera.radius * std::sin(user_camera.elevation) * std::cos(user_camera.azimuth);
		float y = user_camera.radius * std::sin(user_camera.elevation) * std::sin(user_camera.azimuth);
		float z = user_camera.radius * std::cos(user_camera.elevation);
		CLIP_FROM_WORLD = glm::make_mat4((perspective(
			60.0f * float(M_PI) / 180.0f, //vfov
			rtg.swapchain_extent.width / float(rtg.swapchain_extent.height), //aspect
			0.1f, //near
			1000.0f //far
		) * look_at(
			x,y,z, //eye
			0.0f, 0.0f, 0.5f, //target
			0.0f, 0.0f, 1.0f //up
		)).data());
	}
}

RTGRenderer::~RTGRenderer() {
	//just in case rendering is still in flight, don't destroy resources:
	//(not using VK macro to avoid throw-ing in destructor)
	if (VkResult result = vkDeviceWaitIdle(rtg.device); result != VK_SUCCESS) {
		std::cerr << "Failed to vkDeviceWaitIdle in RTGRenderer::~RTGRenderer [" << string_VkResult(result) << "]; continuing anyway." << std::endl;
	}

	if (texture_descriptor_pool) {
		vkDestroyDescriptorPool(rtg.device, texture_descriptor_pool, nullptr);
		texture_descriptor_pool = nullptr;

		//this also frees the descriptor sets allocated from the pool:
		texture_descriptors.clear();
	}

	if (texture_sampler) {
		vkDestroySampler(rtg.device, texture_sampler, nullptr);
		texture_sampler = VK_NULL_HANDLE;
	}

	for (VkImageView &view : texture_views) {
		vkDestroyImageView(rtg.device, view, nullptr);
		view = VK_NULL_HANDLE;
	}
	texture_views.clear();

	for (auto &texture : textures) {
		rtg.helpers.destroy_image(std::move(texture));
	}
	textures.clear();

	background_pipeline.destroy(rtg);
	lines_pipeline.destroy(rtg);
	objects_pipeline.destroy(rtg);

	rtg.helpers.destroy_buffer(std::move(object_vertices));

	if (swapchain_depth_image.handle != VK_NULL_HANDLE) {
		destroy_framebuffers();
	}

	for (Workspace &workspace : workspaces) {
		if (workspace.command_buffer != VK_NULL_HANDLE) {
			vkFreeCommandBuffers(rtg.device, command_pool, 1, &workspace.command_buffer);
			workspace.command_buffer = VK_NULL_HANDLE;
		}

		if (workspace.lines_vertices_src.handle != VK_NULL_HANDLE) {
		rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices_src));
		}
		if (workspace.lines_vertices.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices));
		}
		if (workspace.Camera_src.handle != VK_NULL_HANDLE) {
				rtg.helpers.destroy_buffer(std::move(workspace.Camera_src));
			}
		if (workspace.Camera.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.Camera));
		}
		// Camera descriptors are freed when the pool is destroyed

		if (workspace.World_src.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.World_src));
		}
		if (workspace.World.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.World));
		}
		//World descriptors freed when pool is destroyed.

		if (workspace.Transforms_src.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.Transforms_src));
		}
		if (workspace.Transforms.handle != VK_NULL_HANDLE) {
			rtg.helpers.destroy_buffer(std::move(workspace.Transforms));
		}
		//Transforms_descriptors freed when pool is destroyed.
	}
	workspaces.clear();

	if (descriptor_pool) {
		vkDestroyDescriptorPool(rtg.device, descriptor_pool, nullptr);
		descriptor_pool = nullptr;
		//(this also frees the descriptor sets allocated from the pool)
	}

	if (command_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(rtg.device, command_pool, nullptr);
		command_pool = VK_NULL_HANDLE;
	}

	if (render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(rtg.device, render_pass, nullptr);
		render_pass = VK_NULL_HANDLE;
	}
}

void RTGRenderer::on_swapchain(RTG &rtg_, RTG::SwapchainEvent const &swapchain) {
	// clean up existing framebuffers (and depth image):
	if (swapchain_depth_image.handle != VK_NULL_HANDLE) {
		destroy_framebuffers();
	}
	//Allocate depth image for framebuffers to share:
	swapchain_depth_image = rtg.helpers.create_image(
		swapchain.extent,
		depth_format,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		Helpers::Unmapped
	);
	{//create depth image view:
		VkImageViewCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = swapchain_depth_image.handle,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = depth_format,
			.subresourceRange{
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
		};

		VK(vkCreateImageView(rtg.device, &create_info, nullptr, &swapchain_depth_image_view));
	}
	//Make framebuffers for each swapchain image:
	swapchain_framebuffers.assign(swapchain.image_views.size(), VK_NULL_HANDLE);
	for (size_t i = 0; i < swapchain.image_views.size(); ++i) {
		std::array<VkImageView, 2> attachments{
			swapchain.image_views[i],
			swapchain_depth_image_view,
		};
		VkFramebufferCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = render_pass,
			.attachmentCount = uint32_t(attachments.size()),
			.pAttachments = attachments.data(),
			.width = swapchain.extent.width,
			.height = swapchain.extent.height,
			.layers = 1,
		};

		VK(vkCreateFramebuffer(rtg.device, &create_info, nullptr, &swapchain_framebuffers[i]));
	}
	std::cout<< "There are "<< swapchain.image_views.size() << " images in the swapchain" <<std::endl;
}

void RTGRenderer::destroy_framebuffers() {
	for (VkFramebuffer &framebuffer : swapchain_framebuffers) {
		assert(framebuffer != VK_NULL_HANDLE);
		vkDestroyFramebuffer(rtg.device, framebuffer, nullptr);
		framebuffer = VK_NULL_HANDLE;
	}
	swapchain_framebuffers.clear();

	assert(swapchain_depth_image_view != VK_NULL_HANDLE);
	vkDestroyImageView(rtg.device, swapchain_depth_image_view, nullptr);
	swapchain_depth_image_view = VK_NULL_HANDLE;

	rtg.helpers.destroy_image(std::move(swapchain_depth_image));
}


void RTGRenderer::render(RTG &rtg_, RTG::RenderParams const &render_params) {
	//assert that parameters are valid:
	assert(&rtg == &rtg_);
	assert(render_params.workspace_index < workspaces.size());
	assert(render_params.image_index < swapchain_framebuffers.size());

	//get more convenient names for the current workspace and target framebuffer:
	Workspace &workspace = workspaces[render_params.workspace_index];
	VkFramebuffer framebuffer = swapchain_framebuffers[render_params.image_index];

	//reset the command buffer (clear old commands):
	VK(vkResetCommandBuffer(workspace.command_buffer, 0));

	{//begin recording:
		VkCommandBufferBeginInfo begine_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // records again every submit
		};
		VK(vkBeginCommandBuffer(workspace.command_buffer, &begine_info));
	}

	if (!lines_vertices.empty()) { //upload lines vertices;
		//[re-]allocate lines buffer if needed:
		size_t needed_bytes = lines_vertices.size() * sizeof(lines_vertices[0]);
		if (workspace.lines_vertices_src.handle == VK_NULL_HANDLE || workspace.lines_vertices_src.size < needed_bytes) {
			//round to next multiple of 4k to avoid re-allocating continuously if vertex count grows slowly:
			size_t new_bytes = ((needed_bytes + 4096) / 4096) * 4096;

			if (workspace.lines_vertices_src.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices_src));
			}
			if (workspace.lines_vertices.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.lines_vertices));
			}
			
			workspace.lines_vertices_src = rtg.helpers.create_buffer(
				new_bytes,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT, //going to have GPU copy from this memory
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, //host-visible memory, coherent (no special sync needed)
				Helpers::Mapped //get a pointer to the memory
			);
			workspace.lines_vertices = rtg.helpers.create_buffer(
				new_bytes,
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, //going to use as vertex buffer, also going to have GPU into this memory
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //GPU-local memory
				Helpers::Unmapped //don't get a pointer to the memory
			);

			std::cout << "Re-allocated lines buffers to " << new_bytes << " bytes." << std::endl;

			assert(workspace.lines_vertices_src.size == workspace.lines_vertices.size);
			assert(workspace.lines_vertices_src.size >= needed_bytes);

			// host-side copy into lines_vertices_src:
			assert(workspace.lines_vertices_src.allocation.mapped);
			std::memcpy(workspace.lines_vertices_src.allocation.data(), lines_vertices.data(), needed_bytes);

			//device-side copy from lines_vertices_src -> lines_vertices:
			VkBufferCopy copy_region{
				.srcOffset = 0,
				.dstOffset = 0,
				.size = needed_bytes,
			};

			vkCmdCopyBuffer(workspace.command_buffer, workspace.lines_vertices_src.handle, workspace.lines_vertices.handle, 1, &copy_region);
		}
	}

	{//upload camera info:
		LinesPipeline::Camera camera{
			.CLIP_FROM_WORLD = CLIP_FROM_WORLD
		};
		assert(workspace.Camera_src.size == sizeof(camera));

		//host-side copy into Camera_src:
		memcpy(workspace.Camera_src.allocation.data(), &camera, sizeof(camera));

		//add device-side copy from Camera_src -> Camera:
		assert(workspace.Camera_src.size == workspace.Camera.size);
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = workspace.Camera_src.size,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.Camera_src.handle, workspace.Camera.handle, 1, &copy_region);
	}

	{ //upload world info:
		assert(workspace.Camera_src.size == sizeof(world));

		//host-side copy into World_src:
		memcpy(workspace.World_src.allocation.data(), &world, sizeof(world));

		//add device-side copy from World_src -> World:
		assert(workspace.World_src.size == workspace.World.size);
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = workspace.World_src.size,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.World_src.handle, workspace.World.handle, 1, &copy_region);
	}

	if (!object_instances.empty()) { //upload object transforms:
		size_t needed_bytes = object_instances.size() * sizeof(ObjectsPipeline::Transform);
		if (workspace.Transforms_src.handle == VK_NULL_HANDLE || workspace.Transforms_src.size < needed_bytes) {
			//round to next multiple of 4k to avoid re-allocating continuously if vertex count grows slowly:
			size_t new_bytes = ((needed_bytes + 4096) / 4096) * 4096;
			if (workspace.Transforms_src.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.Transforms_src));
			}
			if (workspace.Transforms.handle) {
				rtg.helpers.destroy_buffer(std::move(workspace.Transforms));
			}
			workspace.Transforms_src = rtg.helpers.create_buffer(
				new_bytes,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT, //going to have GPU copy from this memory
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, //host-visible memory, coherent (no special sync needed)
				Helpers::Mapped //get a pointer to the memory
			);
			workspace.Transforms = rtg.helpers.create_buffer(
				new_bytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, //going to use as storage buffer, also going to have GPU into this memory
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, //GPU-local memory
				Helpers::Unmapped //don't get a pointer to the memory
			);

			//update the descriptor set:
			VkDescriptorBufferInfo Transforms_info{
				.buffer = workspace.Transforms.handle,
				.offset = 0,
				.range = workspace.Transforms.size,
			};

			std::array< VkWriteDescriptorSet, 1 > writes{
				VkWriteDescriptorSet{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = workspace.Transforms_descriptors,
					.dstBinding = 0,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &Transforms_info,
				},
			};

			vkUpdateDescriptorSets(
				rtg.device,
				uint32_t(writes.size()), writes.data(), //descriptorWrites count, data
				0, nullptr //descriptorCopies count, data
			);

			std::cout << "Re-allocated object transforms buffers to " << new_bytes << " bytes." << std::endl;
		}

		assert(workspace.Transforms_src.size == workspace.Transforms.size);
		assert(workspace.Transforms_src.size >= needed_bytes);

		{ //copy transforms into Transforms_src:
			assert(workspace.Transforms_src.allocation.mapped);
			ObjectsPipeline::Transform *out = reinterpret_cast< ObjectsPipeline::Transform * >(workspace.Transforms_src.allocation.data()); // Strict aliasing violation, but it doesn't matter
			for (ObjectInstance const &inst : object_instances) {
				*out = inst.transform;
				++out;
			}
		}

		//device-side copy from Transforms_src -> Transforms:
		VkBufferCopy copy_region{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = needed_bytes,
		};
		vkCmdCopyBuffer(workspace.command_buffer, workspace.Transforms_src.handle, workspace.Transforms.handle, 1, &copy_region);
	}

	{//memory barrier to make sure copies complete before rendering happens:
		VkMemoryBarrier memory_barrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
		};

		vkCmdPipelineBarrier( workspace.command_buffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, //srcStageMask
			VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, //dstStageMask
			0, //dependencyFlags
			1, &memory_barrier, //memoryBarriers (count, data)
			0, nullptr, //bufferMemoryBarriers (count, data)
			0, nullptr //imageMemoryBarriers (count, data)
		);

	}

	{//render pass:
		std::array<VkClearValue, 2> clear_values{
			VkClearValue{.color{.float32{0.0f, 1.0f, .7f, 1.0f}}},
			VkClearValue{.depthStencil{.depth = 1.0f, .stencil = 0}},
		};
		VkRenderPassBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = render_pass,
			.framebuffer = framebuffer,
			.renderArea{
				.offset = {.x = 0, .y = 0},
				.extent = rtg.swapchain_extent,
			},
			.clearValueCount = uint32_t(clear_values.size()),
			.pClearValues = clear_values.data(),
		};
		vkCmdBeginRenderPass(workspace.command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

		{//set scissor rectangle:
			VkRect2D scissor{
				.offset = {.x = 0, .y = 0},
				.extent = rtg.swapchain_extent,
			};
			vkCmdSetScissor(workspace.command_buffer, 0, 1, &scissor);
		}
		{//configure viewport transform:
			VkViewport viewport{
				.x = 0.0f,
				.y = 0.0f,
				.width = float(rtg.swapchain_extent.width),
				.height = float(rtg.swapchain_extent.height),
				.minDepth = 0.0f,
				.maxDepth = 1.0f,
			};
			vkCmdSetViewport(workspace.command_buffer, 0, 1, &viewport);
		}

		{//draw with the background pipeline:
			vkCmdBindPipeline(workspace.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, background_pipeline.handle);
			
			{//push time:
				BackgroundPipeline::Push push{
					.time = float(time),
				};
				vkCmdPushConstants(workspace.command_buffer, background_pipeline.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			}
			
			vkCmdDraw(workspace.command_buffer, 3, 1, 0, 0);
		}

		// {//draw with the lines pipeline:
		// 	vkCmdBindPipeline(workspace.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, lines_pipeline.handle);

		// 	{//use lines_vertices (offset 0) as vertex buffer binding 0:
		// 		std::array< VkBuffer, 1 > vertex_buffers{ workspace.lines_vertices.handle };
		// 		std::array< VkDeviceSize, 1 > offsets{ 0 };
		// 		vkCmdBindVertexBuffers(workspace.command_buffer, 0, uint32_t(vertex_buffers.size()), vertex_buffers.data(), offsets.data());
		// 	}

		// 	{ //bind Camera descriptor set:
		// 		std::array< VkDescriptorSet, 1 > descriptor_sets{
		// 			workspace.Camera_descriptors, //0: Camera
		// 		};
		
		// 		vkCmdBindDescriptorSets(
		// 			workspace.command_buffer, //command buffer
		// 			VK_PIPELINE_BIND_POINT_GRAPHICS, //pipeline bind point
		// 			lines_pipeline.layout, //pipeline layout
		// 			0, //first set
		// 			uint32_t(descriptor_sets.size()), descriptor_sets.data(), //descriptor sets count, ptr
		// 			0, nullptr //dynamic offsets count, ptr
		// 		);
		// 	}

		// 	//draw lines vertices:
		// 	vkCmdDraw(workspace.command_buffer, uint32_t(lines_vertices.size()), 1, 0, 0);
		// }

		{//draw with the objects pipeline:
			vkCmdBindPipeline(workspace.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, objects_pipeline.handle);

			{//use object_vertices (offset 0) as vertex buffer binding 0:
				std::array<VkBuffer, 1>vertex_buffers{object_vertices.handle};
				std::array< VkDeviceSize, 1 > offsets{ 0 };
				vkCmdBindVertexBuffers(workspace.command_buffer, 0, uint32_t(vertex_buffers.size()), vertex_buffers.data(), offsets.data());

			}

			{ //bind Transforms descriptor set:
				std::array< VkDescriptorSet, 2 > descriptor_sets{
					workspace.World_descriptors, //0: World
					workspace.Transforms_descriptors, //1: Transforms
				};
				vkCmdBindDescriptorSets(
					workspace.command_buffer, //command buffer
					VK_PIPELINE_BIND_POINT_GRAPHICS, //pipeline bind point
					objects_pipeline.layout, //pipeline layout
					0, //first set
					uint32_t(descriptor_sets.size()), descriptor_sets.data(), //descriptor sets count, ptr
					0, nullptr //dynamic offsets count, ptr
				);
			}

			//Camera descriptor set is still bound, but unused

			//draw all instances:
			for (ObjectInstance const &inst : object_instances) {
				uint32_t index = uint32_t(&inst - &object_instances[0]);
				//bind texture descriptor set:
				vkCmdBindDescriptorSets(
					workspace.command_buffer, //command buffer
					VK_PIPELINE_BIND_POINT_GRAPHICS, //pipeline bind point
					objects_pipeline.layout, //pipeline layout
					2, //second set
					1, &texture_descriptors[inst.texture], //descriptor sets count, ptr
					0, nullptr //dynamic offsets count, ptr
				);

				vkCmdDraw(workspace.command_buffer, inst.vertices.count, 1, inst.vertices.first, index);
			}
			vkCmdEndRenderPass(workspace.command_buffer);
		}
	}

	//end recording:
	VK(vkEndCommandBuffer(workspace.command_buffer));

	{//submit `workspace.command buffer` for the GPU to run:
		std::array<VkSemaphore, 1> wait_semaphores{
			render_params.image_available
		};
		std::array<VkPipelineStageFlags,1> wait_stages{
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		};
		static_assert(wait_semaphores.size() == wait_stages.size(), "every semaphore needs a stage");

		std::array<VkSemaphore, 1>signal_semaphores{
			render_params.image_done
		};
		VkSubmitInfo submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = uint32_t(wait_semaphores.size()),
			.pWaitSemaphores = wait_semaphores.data(),
			.pWaitDstStageMask = wait_stages.data(),
			.commandBufferCount = 1,
			.pCommandBuffers = &workspace.command_buffer,
			.signalSemaphoreCount = uint32_t(signal_semaphores.size()),
			.pSignalSemaphores = signal_semaphores.data(),
		};
		VK(vkQueueSubmit(rtg.graphics_queue, 1, &submit_info, render_params.workspace_available))
	}
}


void RTGRenderer::update(float dt) {
	time = std::fmod(time + dt, 60.0f);

	{ //non static sun and static sky:
		world.SKY_DIRECTION.x = 0.0f;
		world.SKY_DIRECTION.y = 0.0f;
		world.SKY_DIRECTION.z = 1.0f;

		world.SKY_ENERGY.r = 0.1f;
		world.SKY_ENERGY.g = 0.1f;
		world.SKY_ENERGY.b = 0.2f;

		world.SUN_DIRECTION.x = 6.0f / 23.0f;
		world.SUN_DIRECTION.y = 13.0f / 23.0f;
		world.SUN_DIRECTION.z = 18.0f / 23.0f;

		float length = sqrt(world.SUN_DIRECTION.x * world.SUN_DIRECTION.x + world.SUN_DIRECTION.y * world.SUN_DIRECTION.y + world.SUN_DIRECTION.z * world.SUN_DIRECTION.z);
		world.SKY_DIRECTION.x /= length;
		world.SKY_DIRECTION.y /= length;
		world.SKY_DIRECTION.z /= length;

		world.SUN_ENERGY.r = 1.0f;
		world.SUN_ENERGY.g = 1.0f;
		world.SUN_ENERGY.b = 0.9f;
	}

	// { //make a grid that is circular:
	// 	lines_vertices.clear();
	// 	constexpr size_t count = 2 * 101;
	// 	lines_vertices.reserve(count);
	// 	//horizontal lines at z = 0.5f:
	// 	for (uint32_t i = 0; i < 101; ++i) {
	// 		float x = 1.0f, y = 1.0f;
	// 		if (i < 50) {
	// 			y -= float(i) / 25.0f;
	// 		}
	// 		else {
	// 			x -= float(i-50) / 25.0f;
	// 		}
	// 		lines_vertices.emplace_back(PosColVertex{
	// 			.Position{.x = x, .y = y, .z = .1f + sin(time/20.0f)},
	// 			.Color{ .r = uint8_t((x+1) * 255.0f/2.0f), .g = uint8_t((y+1) * 255.0f/2.0f), .b = 0xff, .a = 0xff},
	// 		});
	// 		lines_vertices.emplace_back(PosColVertex{
	// 			.Position{.x = -x, .y = -y, .z = .1f + cos(time/10.0f)},
	// 			.Color{ .r = uint8_t((-x+1) * 255.0f/2.0f), .g = uint8_t((-y+1) * 255.0f/2.0f), .b = 0x00, .a = 0xff},
	// 		});
	// 	}

	// 	assert(lines_vertices.size() == count);
	// }

	{ //make some objects:
		object_instances.clear();

		std::deque<glm::mat4x4> transform_stack;
		std::function<void(uint32_t)> draw_node = [&](uint32_t i) {
			Scene::Node& cur_node = scene.nodes[i];
			glm::mat4x4 cur_node_transform_in_parent = cur_node.transform.parent_from_local();
			if (transform_stack.empty()) {
				transform_stack.push_back(cur_node_transform_in_parent);
			}
			else {
				glm::mat4x4 parent_node_transform_in_world = transform_stack.back();
				transform_stack.push_back(parent_node_transform_in_world * cur_node_transform_in_parent);
			}
			// draw own mesh
			if (int32_t cur_mesh_index = cur_node.mesh_index; cur_mesh_index != -1) {
				glm::mat4x4 WORLD_FROM_LOCAL = transform_stack.back();
				uint32_t texture_index = 0;
				if (scene.meshes[cur_mesh_index].material_index != -1) {
					texture_index = scene.materials[scene.meshes[cur_mesh_index].material_index].texture_index + 1;
				}

				object_instances.emplace_back(ObjectInstance{
					.vertices = mesh_vertices[cur_mesh_index],
					.transform{
						.CLIP_FROM_LOCAL = CLIP_FROM_WORLD * WORLD_FROM_LOCAL,
						.WORLD_FROM_LOCAL = WORLD_FROM_LOCAL,
						.WORLD_FROM_LOCAL_NORMAL = WORLD_FROM_LOCAL,
					},
					.texture = texture_index,
				});
			}
			// draw children mesh
			for (uint32_t child_index : cur_node.children) {
				draw_node(child_index);
			}
			transform_stack.pop_back();
		};

		//traverse the scene hiearchy:
		for (uint32_t i = 0; i < scene.root_nodes.size(); ++i) {
			transform_stack.clear();
			draw_node(scene.root_nodes[i]);
		}
	}
}


void RTGRenderer::on_input(InputEvent const &event) {
	static glm::mat4x4 perspective_mat = glm::make_mat4(perspective(
		60.0f * float(M_PI) / 180.0f, //vfov
		rtg.swapchain_extent.width / float(rtg.swapchain_extent.height), //aspect
		0.1f, //near
		1000.0f //far
	).data());
	static glm::vec3 eye = {user_camera.radius * std::cos(user_camera.elevation) * std::cos(user_camera.azimuth),
			      user_camera.radius * std::cos(user_camera.elevation) * std::sin(user_camera.azimuth),
				  user_camera.radius * std::sin(user_camera.elevation)};
	bool update_camera = false;
	switch (event.type) {
		case InputEvent::Type::MouseMotion:
			if (event.motion.state && !shift_down) {
				if (previous_mouse_x != -1.0f) {
					update_camera = true;
					if (upside_down) {
						user_camera.azimuth += (event.motion.x - previous_mouse_x) * 3.0f;
					}
					else {
						user_camera.azimuth -= (event.motion.x - previous_mouse_x) * 3.0f;
					}
					user_camera.elevation += (event.motion.y - previous_mouse_y) * 3.0f;
					user_camera.azimuth = fmod(user_camera.azimuth, 2.0f * float(M_PI));
					user_camera.elevation = fmod(user_camera.elevation, 2.0f * float(M_PI));
				}

				previous_mouse_x = event.motion.x;
				previous_mouse_y = event.motion.y;
			} 
			else if (event.motion.state && shift_down) {
				//pan
				if (previous_mouse_x != -1.0f) {
					update_camera = true;
					glm::vec3 forward = glm::normalize(user_camera.target - eye);  // Forward direction
					glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f))); // Right vector
					glm::vec3 up = glm::cross(right, forward);  // Up vector
		
					float pan_sensitivity = 2.0f * std::max(user_camera.radius,0.5f);
					user_camera.target -= right * (event.motion.x - previous_mouse_x) * pan_sensitivity;
					user_camera.target += up * (event.motion.y - previous_mouse_y) * pan_sensitivity;
				}
				previous_mouse_x = event.motion.x;
				previous_mouse_y = event.motion.y;
			}
			break;
		case InputEvent::Type::KeyDown:
			if (event.key.key == GLFW_KEY_LEFT_SHIFT) {
				shift_down = true;
			}
			break;
		case InputEvent::Type::KeyUp:
			if (event.key.key == GLFW_KEY_LEFT_SHIFT) {
				shift_down = false;
			}
			break;
		case InputEvent::Type::MouseButtonUp:
			previous_mouse_x = -1.0f;
			break;
		case InputEvent::Type::MouseWheel:
			user_camera.radius = std::max(user_camera.radius - event.wheel.y*0.5f, 0.0f);
			update_camera = true;
			break;
		default:
			break;
	}
	if (update_camera) {
		float x = user_camera.radius * std::cos(user_camera.elevation) * std::cos(user_camera.azimuth);
		float y = user_camera.radius * std::cos(user_camera.elevation) * std::sin(user_camera.azimuth);
		float z = user_camera.radius * std::sin(user_camera.elevation);
		float up = 1.0f;
		upside_down = false;
		// flip up axis when upside down
		if (int((abs(user_camera.elevation) + float(M_PI) / 2) / float(M_PI)) % 2 == 1 ) {
			up =-1.0f;
			upside_down = true;
		}
		eye = glm::vec3{x,y,z} + user_camera.target;
		CLIP_FROM_WORLD = perspective_mat * glm::make_mat4(look_at(
			eye.x,eye.y,eye.z, //eye
			user_camera.target.x,user_camera.target.y,user_camera.target.z, //target
			0.0f, 0.0f, up //up
		).data());
	}

}
