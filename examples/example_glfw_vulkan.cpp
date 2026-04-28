/* See LICENSE for license details. */
/*
	example_glfw_vulkan.cpp — alternative shell using Vulkan for
	rendering, with GLFW for windowing/input.

	Functionally equivalent to main_example_glfw_gl.cpp and
	example_mac_metal.mm — same widget (imgui_win.cpp), same core (st.c),
	different GPU API.
	Demonstrates the renderer-agnostic principle: anywhere ImGui has a
	backend, this terminal runs.

	Vulkan setup is verbose by nature (instance, physical device, queue
	family, descriptor pool, swapchain, framebuffers, fences,
	semaphores). Most of the boilerplate below is lifted from ImGui's
	canonical glfw+vulkan example with the demo windows and validation
	layers stripped. ImGui_ImplVulkanH_* helpers wrap the swapchain
	lifecycle.

	  Build:  `make vulkan`   →   build/imgui_terminal_vulkan

	Requires the Vulkan SDK + GLFW with Vulkan support:
	  - macOS:  brew install vulkan-headers vulkan-loader molten-vk
	            (or install LunarG VulkanSDK)
	  - Linux:  apt install libvulkan-dev   (Debian/Ubuntu)
*/

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

/* Public API of imgui_win.cpp (the widget). */
extern void term_init(int cols, int rows, char **argv);
extern void term_draw_widget(void);
extern void term_shutdown(void);

/*
	Vulkan globals — file-static; the example uses globals because the
	setup/cleanup helpers reference them across functions.
*/
static VkAllocationCallbacks   *g_alloc          = nullptr;
static VkInstance               g_instance       = VK_NULL_HANDLE;
static VkPhysicalDevice         g_physical       = VK_NULL_HANDLE;
static VkDevice                 g_device         = VK_NULL_HANDLE;
static uint32_t                 g_queue_family   = (uint32_t)-1;
static VkQueue                  g_queue          = VK_NULL_HANDLE;
static VkDescriptorPool         g_descriptor_pool = VK_NULL_HANDLE;
static ImGui_ImplVulkanH_Window g_window_data;
static uint32_t                 g_min_image_count = 2;
static bool                     g_swapchain_rebuild = false;

static void
check_vk(VkResult err)
{
	if (err == VK_SUCCESS)
		return;
	fprintf(stderr, "[vulkan] VkResult = %d\n", err);
	if (err < 0)
		abort();
}

static bool
ext_available(const ImVector<VkExtensionProperties> &props, const char *name)
{
	for (const VkExtensionProperties &p : props)
		if (strcmp(p.extensionName, name) == 0)
			return true;
	return false;
}

static void
setup_vulkan(ImVector<const char*> instance_exts)
{
	VkResult err;

	/* Instance + portability extensions for MoltenVK on macOS. */
	{
		VkInstanceCreateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

		uint32_t pcount;
		ImVector<VkExtensionProperties> props;
		vkEnumerateInstanceExtensionProperties(nullptr, &pcount, nullptr);
		props.resize(pcount);
		err = vkEnumerateInstanceExtensionProperties(nullptr, &pcount,
		                                             props.Data);
		check_vk(err);

		if (ext_available(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
			instance_exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
		if (ext_available(props, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
			instance_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}
#endif
		ci.enabledExtensionCount   = (uint32_t)instance_exts.Size;
		ci.ppEnabledExtensionNames = instance_exts.Data;
		err = vkCreateInstance(&ci, g_alloc, &g_instance);
		check_vk(err);
	}

	g_physical     = ImGui_ImplVulkanH_SelectPhysicalDevice(g_instance);
	g_queue_family = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_physical);

	/* Logical device + queue. */
	{
		ImVector<const char*> dev_exts;
		dev_exts.push_back("VK_KHR_swapchain");

		uint32_t pcount;
		ImVector<VkExtensionProperties> props;
		vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &pcount, nullptr);
		props.resize(pcount);
		vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &pcount, props.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
		if (ext_available(props, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
			dev_exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
		const float prio[] = { 1.0f };
		VkDeviceQueueCreateInfo qi = {};
		qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qi.queueFamilyIndex = g_queue_family;
		qi.queueCount       = 1;
		qi.pQueuePriorities = prio;

		VkDeviceCreateInfo ci = {};
		ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		ci.queueCreateInfoCount    = 1;
		ci.pQueueCreateInfos       = &qi;
		ci.enabledExtensionCount   = (uint32_t)dev_exts.Size;
		ci.ppEnabledExtensionNames = dev_exts.Data;
		err = vkCreateDevice(g_physical, &ci, g_alloc, &g_device);
		check_vk(err);
		vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);
	}

	/* Descriptor pool — sized via ImGui's recommended minimums. */
	{
		VkDescriptorPoolSize sizes[] = {
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE },
			{ VK_DESCRIPTOR_TYPE_SAMPLER,       IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE       },
		};
		VkDescriptorPoolCreateInfo ci = {};
		ci.sType    = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		ci.flags    = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		ci.maxSets  = 0;
		for (auto &s : sizes) ci.maxSets += s.descriptorCount;
		ci.poolSizeCount = (uint32_t)IM_ARRAYSIZE(sizes);
		ci.pPoolSizes    = sizes;
		err = vkCreateDescriptorPool(g_device, &ci, g_alloc, &g_descriptor_pool);
		check_vk(err);
	}
}

static void
setup_window(ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface, int w, int h)
{
	VkBool32 wsi_ok;
	vkGetPhysicalDeviceSurfaceSupportKHR(g_physical, g_queue_family, surface, &wsi_ok);
	if (!wsi_ok) {
		fprintf(stderr, "[vulkan] no WSI support on selected device\n");
		exit(1);
	}

	const VkFormat fmts[] = {
		VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8_UNORM,   VK_FORMAT_R8G8B8_UNORM,
	};
	wd->Surface       = surface;
	wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
	    g_physical, surface, fmts, IM_ARRAYSIZE(fmts),
	    VK_COLORSPACE_SRGB_NONLINEAR_KHR);

	VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
	wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
	    g_physical, surface, present_modes, IM_ARRAYSIZE(present_modes));

	IM_ASSERT(g_min_image_count >= 2);
	ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_physical, g_device,
	                                       wd, g_queue_family, g_alloc,
	                                       w, h, g_min_image_count, 0);
}

static void
cleanup_vulkan()
{
	vkDestroyDescriptorPool(g_device, g_descriptor_pool, g_alloc);
	vkDestroyDevice(g_device, g_alloc);
	vkDestroyInstance(g_instance, g_alloc);
}

static void
cleanup_window(ImGui_ImplVulkanH_Window *wd)
{
	ImGui_ImplVulkanH_DestroyWindow(g_instance, g_device, wd, g_alloc);
	vkDestroySurfaceKHR(g_instance, wd->Surface, g_alloc);
}

static void
frame_render(ImGui_ImplVulkanH_Window *wd, ImDrawData *draw_data)
{
	VkSemaphore acq = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
	VkSemaphore done = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

	VkResult err = vkAcquireNextImageKHR(g_device, wd->Swapchain, UINT64_MAX,
	                                     acq, VK_NULL_HANDLE, &wd->FrameIndex);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
		g_swapchain_rebuild = true;
	if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
	if (err != VK_SUBOPTIMAL_KHR) check_vk(err);

	ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];
	check_vk(vkWaitForFences(g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
	check_vk(vkResetFences(g_device, 1, &fd->Fence));
	check_vk(vkResetCommandPool(g_device, fd->CommandPool, 0));

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vk(vkBeginCommandBuffer(fd->CommandBuffer, &bi));

	VkRenderPassBeginInfo rp = {};
	rp.sType                 = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp.renderPass            = wd->RenderPass;
	rp.framebuffer           = fd->Framebuffer;
	rp.renderArea.extent     = { (uint32_t)wd->Width, (uint32_t)wd->Height };
	rp.clearValueCount       = 1;
	rp.pClearValues          = &wd->ClearValue;
	vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

	ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

	vkCmdEndRenderPass(fd->CommandBuffer);

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {};
	si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount   = 1;
	si.pWaitSemaphores      = &acq;
	si.pWaitDstStageMask    = &wait_stage;
	si.commandBufferCount   = 1;
	si.pCommandBuffers      = &fd->CommandBuffer;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores    = &done;
	check_vk(vkEndCommandBuffer(fd->CommandBuffer));
	check_vk(vkQueueSubmit(g_queue, 1, &si, fd->Fence));
}

static void
frame_present(ImGui_ImplVulkanH_Window *wd)
{
	if (g_swapchain_rebuild) return;
	VkSemaphore done = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
	VkPresentInfoKHR pi = {};
	pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores    = &done;
	pi.swapchainCount     = 1;
	pi.pSwapchains        = &wd->Swapchain;
	pi.pImageIndices      = &wd->FrameIndex;
	VkResult err = vkQueuePresentKHR(g_queue, &pi);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
		g_swapchain_rebuild = true;
	if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
	if (err != VK_SUBOPTIMAL_KHR) check_vk(err);
	wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

int
main(int, char **)
{
	if (!glfwInit())
		return 1;

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow *window = glfwCreateWindow(800, 600,
	                                      "st-imgui (vulkan)",
	                                      nullptr, nullptr);
	if (!window || !glfwVulkanSupported()) {
		fprintf(stderr, "GLFW: Vulkan not supported on this system\n");
		glfwTerminate();
		return 1;
	}

	/* Required-by-GLFW instance extensions (varies by platform). */
	ImVector<const char*> exts;
	uint32_t ext_count = 0;
	const char **glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
	for (uint32_t i = 0; i < ext_count; i++)
		exts.push_back(glfw_exts[i]);
	setup_vulkan(exts);

	VkSurfaceKHR surface;
	check_vk(glfwCreateWindowSurface(g_instance, window, g_alloc, &surface));

	int fb_w, fb_h;
	glfwGetFramebufferSize(window, &fb_w, &fb_h);
	ImGui_ImplVulkanH_Window *wd = &g_window_data;
	setup_window(wd, surface, fb_w, fb_h);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui::GetIO().ConfigDpiScaleFonts = true;

	ImGui_ImplGlfw_InitForVulkan(window, true);
	ImGui_ImplVulkan_InitInfo init = {};
	init.Instance       = g_instance;
	init.PhysicalDevice = g_physical;
	init.Device         = g_device;
	init.QueueFamily    = g_queue_family;
	init.Queue          = g_queue;
	init.DescriptorPool = g_descriptor_pool;
	init.MinImageCount  = g_min_image_count;
	init.ImageCount     = wd->ImageCount;
	init.Allocator      = g_alloc;
	init.PipelineInfoMain.RenderPass  = wd->RenderPass;
	init.PipelineInfoMain.Subpass     = 0;
	init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init.CheckVkResultFn = check_vk;
	ImGui_ImplVulkan_Init(&init);

	term_init(80, 24, NULL);

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		int cur_w, cur_h;
		glfwGetFramebufferSize(window, &cur_w, &cur_h);
		if (cur_w > 0 && cur_h > 0
		    && (g_swapchain_rebuild
		        || g_window_data.Width != cur_w
		        || g_window_data.Height != cur_h)) {
			ImGui_ImplVulkan_SetMinImageCount(g_min_image_count);
			ImGui_ImplVulkanH_CreateOrResizeWindow(
			    g_instance, g_physical, g_device, wd,
			    g_queue_family, g_alloc, cur_w, cur_h,
			    g_min_image_count, 0);
			g_window_data.FrameIndex = 0;
			g_swapchain_rebuild = false;
		}
		if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
			ImGui_ImplGlfw_Sleep(10);
			continue;
		}

		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		term_draw_widget();

		ImGui::Render();
		ImDrawData *dd = ImGui::GetDrawData();
		bool minimized = (dd->DisplaySize.x <= 0.0f
		               || dd->DisplaySize.y <= 0.0f);
		wd->ClearValue.color.float32[0] = 0.05f;
		wd->ClearValue.color.float32[1] = 0.05f;
		wd->ClearValue.color.float32[2] = 0.05f;
		wd->ClearValue.color.float32[3] = 1.0f;
		if (!minimized) {
			frame_render(wd, dd);
			frame_present(wd);
		}
	}

	check_vk(vkDeviceWaitIdle(g_device));
	term_shutdown();
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	cleanup_window(&g_window_data);
	cleanup_vulkan();

	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
