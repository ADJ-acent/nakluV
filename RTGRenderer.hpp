#pragma once

#include "PosColVertex.hpp"
#include "PosNorTanTexVertex.hpp"
#include "mat4.hpp"

#include "RTG.hpp"
#include "Scene.hpp"
#include "frustum_culling.hpp"

#include "GLM.hpp"

struct RTGRenderer : RTG::Application {

	RTGRenderer(RTG &, Scene &);
	RTGRenderer(RTGRenderer const &) = delete; //you shouldn't be copying this object
	~RTGRenderer();

	//kept for use in destructor:
	RTG &rtg;

	//scene that contains nodes, camera, light, material and texture information
	Scene &scene;
	//--------------------------------------------------------------------
	//Resources that last the lifetime of the application:

	//chosen format for depth buffer:
	VkFormat depth_format{};
	//Render passes describe how pipelines write to images:
	VkRenderPass render_pass = VK_NULL_HANDLE;

	//Pipelines:

	struct BackgroundPipeline {
		// no descriptor set layouts

		struct Push {
			float time;
		};

		VkPipelineLayout layout = VK_NULL_HANDLE;

		// no vertex bindings

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} background_pipeline;

	struct LinesPipeline {
		//descriptor set layouts:
		VkDescriptorSetLayout set0_Camera = VK_NULL_HANDLE;

		//types for descriptors:
		struct Camera {
			glm::mat4x4 CLIP_FROM_WORLD;
		};

		static_assert(sizeof(Camera) == 16*4, "camera buffer structure is packed");

		// no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;

		using Vertex = PosColVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} lines_pipeline;

	struct LambertianPipeline {
		//descriptor set layouts:
		VkDescriptorSetLayout set0_World = VK_NULL_HANDLE;
        VkDescriptorSetLayout set1_Transforms = VK_NULL_HANDLE;
        VkDescriptorSetLayout set2_TEXTURE = VK_NULL_HANDLE;

		//types for descriptors:

        struct World {
            struct { float x, y, z, padding_; } SKY_DIRECTION;
            struct { float r, g, b, padding_; } SKY_ENERGY;
            struct { float x, y, z, padding_; } SUN_DIRECTION;
            struct { float r, g, b, padding_; } SUN_ENERGY;
			glm::vec4 CAMERA_POSITION_ENVIRONMENT_MIPS; //xyz: camera position, z: environment mips
        };
        static_assert(sizeof(World) == 4*4 + 4*4 + 4*4 + 4*4 + 4*4, "World is the expected size.");
		
        struct Transform {
            glm::mat4x4 CLIP_FROM_LOCAL;
            glm::mat4x4 WORLD_FROM_LOCAL;
            glm::mat4x4 WORLD_FROM_LOCAL_NORMAL;
        };
        static_assert(sizeof(Transform) == 16*4 + 16*4 + 16*4, "Transform is the expected size.");

		//no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;

		using Vertex = PosNorTanTexVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} lambertian_pipeline;

	struct EnvironmentPipeline {
		//descriptor set layouts:
		VkDescriptorSetLayout set0_World = VK_NULL_HANDLE;
        VkDescriptorSetLayout set1_Transforms = VK_NULL_HANDLE;
        VkDescriptorSetLayout set2_TEXTURE = VK_NULL_HANDLE;

		//types for descriptors same as objects pipeline

		//no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;

		using Vertex = PosNorTanTexVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} environment_pipeline;

	struct MirrorPipeline {
		//descriptor set layouts:
		VkDescriptorSetLayout set0_World = VK_NULL_HANDLE;
        VkDescriptorSetLayout set1_Transforms = VK_NULL_HANDLE;
        VkDescriptorSetLayout set2_TEXTURE = VK_NULL_HANDLE;

		//types for descriptors same as objects pipeline
		
		//no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;

		using Vertex = PosNorTanTexVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} mirror_pipeline;

	struct PBRPipeline {
		//descriptor set layouts:
		VkDescriptorSetLayout set0_World = VK_NULL_HANDLE;
        VkDescriptorSetLayout set1_Transforms = VK_NULL_HANDLE;
        VkDescriptorSetLayout set2_TEXTURE = VK_NULL_HANDLE;

		//types for descriptors same as objects pipeline
		
		//no push constants

		VkPipelineLayout layout = VK_NULL_HANDLE;

		using Vertex = PosNorTanTexVertex;

		VkPipeline handle = VK_NULL_HANDLE;

		void create(RTG &, VkRenderPass render_pass, uint32_t subpass);
		void destroy(RTG &);
	} pbr_pipeline;

	//pools from which per-workspace things are allocated:
	VkCommandPool command_pool = VK_NULL_HANDLE;
	
	//descriptor pool
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

	//workspaces hold per-render resources:
	struct Workspace {
		VkCommandBuffer command_buffer = VK_NULL_HANDLE; //from the command pool above; reset at the start of every render.
		
		//location for lines data: (streamed to GPU per-frame)
		Helpers::AllocatedBuffer lines_vertices_src; //host coherent; mapped
		Helpers::AllocatedBuffer lines_vertices; //device-local

		//location for LinesPipeline::Camera data: (streamed to GPU per-frame)
		Helpers::AllocatedBuffer Camera_src; //host coherent; mapped
		Helpers::AllocatedBuffer Camera; //device-local
		VkDescriptorSet Camera_descriptors; //references Camera

        //location for ObjectsPipeline::World data: (streamed to GPU per-frame)
        Helpers::AllocatedBuffer World_src; //host coherent; mapped
        Helpers::AllocatedBuffer World; //device-local
        VkDescriptorSet World_descriptors; //references World

        // locations for ObjectsPipeline::Transforms data: (streamed to GPU per-frame):
        Helpers::AllocatedBuffer Transforms_src; //host coherent; mapped
        Helpers::AllocatedBuffer Transforms; //device-local
        VkDescriptorSet Transforms_descriptors; //references Transforms
	};
	std::vector< Workspace > workspaces;

	//-------------------------------------------------------------------
	//static scene resources:

    Helpers::AllocatedBuffer object_vertices;
    struct ObjectVertices {
		uint32_t first = 0;
		uint32_t count = 0;
	};
	std::vector<ObjectVertices> mesh_vertices; // indexed the same as scene.meshes
	std::vector<AABB> mesh_AABBs; // also indexed the same as scene.meshes

	Helpers::AllocatedImage World_environment;
	VkImageView World_environment_view = VK_NULL_HANDLE;
	VkSampler World_environment_sampler = VK_NULL_HANDLE;

	Helpers::AllocatedImage World_environment_brdf_lut;
	VkImageView World_environment_brdf_lut_view = VK_NULL_HANDLE;

    std::vector< Helpers::AllocatedImage > textures;
	std::vector< VkImageView > texture_views;
	VkSampler texture_sampler = VK_NULL_HANDLE;
	VkDescriptorPool material_descriptor_pool = VK_NULL_HANDLE;
	std::vector< VkDescriptorSet > material_descriptors; //allocated from texture_descriptor_pool

	//--------------------------------------------------------------------
	//Resources that change when the swapchain is resized:

	virtual void on_swapchain(RTG &, RTG::SwapchainEvent const &) override;

	Helpers::AllocatedImage swapchain_depth_image;
	VkImageView swapchain_depth_image_view = VK_NULL_HANDLE;
	std::vector< VkFramebuffer > swapchain_framebuffers;
	//used from on_swapchain and the destructor: (framebuffers are created in on_swapchain)
	void destroy_framebuffers();

	//--------------------------------------------------------------------
	//Resources that change when time passes or the user interacts:
	struct FreeCamera;

	virtual void update(float dt) override;
	virtual void on_input(InputEvent const &) override;
	virtual void set_animation_time(float t) override;
	void update_free_camera(FreeCamera& camera);

	float time = 0.0f;

	glm::mat4x4 CLIP_FROM_WORLD;

	std::vector<LinesPipeline::Vertex> lines_vertices;

    LambertianPipeline::World world;

	using Transform = LambertianPipeline::Transform;
    
    struct ObjectInstance {
		ObjectVertices vertices;
		Transform transform;
		uint32_t material_index;
	};
	std::vector< ObjectInstance > lambertian_instances, environment_instances, mirror_instances, pbr_instances;

	enum InSceneCamera{
		SceneCamera = 0,
		UserCamera = 1,
		DebugCamera = 2
	};
	InSceneCamera view_camera = InSceneCamera::SceneCamera;

	struct FreeCamera
	{
		InSceneCamera type;
		glm::vec3 target = {0.0f, 0.0f, 0.5f};
		float radius = 10.0f;
		float azimuth = 0.0f;
		float elevation = 0.785398163f; //Pi/4
		glm::vec3 eye = {
			radius * std::cos(elevation) * std::cos(azimuth),
			radius * std::cos(elevation) * std::sin(azimuth),
			radius * std::sin(elevation)
		};
	} user_camera, debug_camera;

	float previous_mouse_x = -1.0f, previous_mouse_y = -1.0f;
	bool shift_down = false;
	bool upside_down = false;

	CullingFrustum scene_cam_frustum, user_cam_frustum;
	// perspective and view matrices for scene, user, and debug cameras
	std::array<glm::mat4x4, 3> clip_from_view;
	std::array<glm::mat4x4, 3> view_from_world;

	InSceneCamera culling_camera = InSceneCamera::SceneCamera;
	//--------------------------------------------------------------------
	//Rendering function, uses all the resources above to queue work to draw a frame:

	virtual void render(RTG &, RTG::RenderParams const &) override;
};
