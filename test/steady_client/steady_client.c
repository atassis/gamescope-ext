// Minimal XCB+Vulkan test client for the external-upscaler ABI's steady-state tests. Unlike
// vkcube, this sets FIXED WM_NORMAL_HINTS (min==max) BEFORE mapping its window, so gamescope's
// handle_desktop_window() (steamcompmgr.cpp, stock upstream, commit 81d6554a) takes the "honor
// requested size" branch instead of forcing it to the output size -- the root cause found while
// investigating why vkcube couldn't hold a steady small render size through -F external or -F npu.
//
// Content: each frame is a flat vkCmdClearColorImage color encoding the frame counter in R/G (R =
// low byte, G = high byte, so frame = round(R*255) + round(G*255)*256) plus a slowly cycling blue
// channel (a visible "moving" component). No shaders/pipeline needed -- deliberately the simplest
// thing that is still both animated and machine-checkable from a screenshot.
//
// Usage: steady_client <width> <height> [frame_count, 0 = until SIGINT]; env STEADY_BRIGHT=1,
// STEADY_RESIZE=<frame>:<w>x<h> (resize the window and swapchain once, at that frame).
#define _GNU_SOURCE
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

static volatile sig_atomic_t g_bRun = 1;
static void on_sigint(int sig) { (void)sig; g_bRun = 0; }

static void die(const char *msg) { fprintf(stderr, "steady_client: %s\n", msg); exit(1); }

static void set_fixed_size(xcb_connection_t *xc, xcb_window_t win, uint32_t width, uint32_t height)
{
	xcb_size_hints_t hints;
	memset(&hints, 0, sizeof(hints));
	xcb_icccm_size_hints_set_min_size(&hints, (int32_t)width, (int32_t)height);
	xcb_icccm_size_hints_set_max_size(&hints, (int32_t)width, (int32_t)height);
	xcb_icccm_set_wm_normal_hints(xc, win, &hints);
}
static void trace(uint64_t id, const char *stage)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[upscale-trace] %" PRIu64 " %s %" PRIu64 "\n", id, stage, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

int main(int argc, char **argv) {
	if (argc < 3) die("usage: steady_client <width> <height> [frame_count]");
	uint32_t width = atoi(argv[1]), height = atoi(argv[2]);
	uint64_t frameLimit = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
	const int bright = getenv("STEADY_BRIGHT") && atoi(getenv("STEADY_BRIGHT")) == 1;
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	// --- XCB: window with fixed size hints set before map ---
	int screenNum;
	xcb_connection_t *xc = xcb_connect(NULL, &screenNum);
	if (xcb_connection_has_error(xc)) die("xcb_connect failed");
	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(xc)).data;
	xcb_window_t win = xcb_generate_id(xc);
	uint32_t mask = XCB_CW_EVENT_MASK;
	uint32_t values[] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY };
	xcb_create_window(xc, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, width, height, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

	set_fixed_size(xc, win, width, height);
	xcb_icccm_set_wm_name(xc, win, XCB_ATOM_STRING, 8, 12, "SteadyClient");

	xcb_map_window(xc, win);
	xcb_flush(xc);

	// --- Vulkan: instance, xcb surface (Gamescope WSI layer intercepts this), device, swapchain ---
	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "SteadyClient";
	appInfo.apiVersion = VK_API_VERSION_1_2;
	const char *instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &appInfo;
	ici.enabledExtensionCount = 2;
	ici.ppEnabledExtensionNames = instExts;
	VkInstance instance;
	if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) die("vkCreateInstance failed");

	VkXcbSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR };
	sci.connection = xc;
	sci.window = win;
	VkSurfaceKHR surface;
	if (vkCreateXcbSurfaceKHR(instance, &sci, NULL, &surface) != VK_SUCCESS) die("vkCreateXcbSurfaceKHR failed");

	uint32_t nPhys = 0;
	vkEnumeratePhysicalDevices(instance, &nPhys, NULL);
	if (!nPhys) die("no physical devices");
	VkPhysicalDevice *physDevs = malloc(sizeof(VkPhysicalDevice) * nPhys);
	vkEnumeratePhysicalDevices(instance, &nPhys, physDevs);
	VkPhysicalDevice physDev = physDevs[0];
	free(physDevs);

	uint32_t nQueueFamilies = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physDev, &nQueueFamilies, NULL);
	VkQueueFamilyProperties *qprops = malloc(sizeof(VkQueueFamilyProperties) * nQueueFamilies);
	vkGetPhysicalDeviceQueueFamilyProperties(physDev, &nQueueFamilies, qprops);
	uint32_t queueFamily = UINT32_MAX;
	for (uint32_t i = 0; i < nQueueFamilies; i++) {
		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physDev, i, surface, &present);
		if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { queueFamily = i; break; }
	}
	free(qprops);
	if (queueFamily == UINT32_MAX) die("no graphics+present queue family");

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = devExts;
	VkDevice device;
	if (vkCreateDevice(physDev, &dci, NULL, &device) != VK_SUCCESS) die("vkCreateDevice failed");
	VkQueue queue;
	vkGetDeviceQueue(device, queueFamily, 0, &queue);

	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);
	uint32_t nFormats = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &nFormats, NULL);
	VkSurfaceFormatKHR *formats = malloc(sizeof(VkSurfaceFormatKHR) * nFormats);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &nFormats, formats);
	VkSurfaceFormatKHR chosen = formats[0];
	free(formats);
	fprintf(stderr, "steady_client: currentExtent %ux%u (requested %ux%u)\n", caps.currentExtent.width, caps.currentExtent.height, width, height);

	VkSwapchainCreateInfoKHR scci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	scci.surface = surface;
	scci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
	scci.imageFormat = chosen.format;
	scci.imageColorSpace = chosen.colorSpace;
	scci.imageExtent = caps.currentExtent.width != 0xFFFFFFFF ? caps.currentExtent : (VkExtent2D){ width, height };
	scci.imageArrayLayers = 1;
	scci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	scci.preTransform = caps.currentTransform;
	scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	scci.clipped = VK_TRUE;
	VkSwapchainKHR swapchain;
	if (vkCreateSwapchainKHR(device, &scci, NULL, &swapchain) != VK_SUCCESS) die("vkCreateSwapchainKHR failed");

	uint32_t nImages = 0;
	vkGetSwapchainImagesKHR(device, swapchain, &nImages, NULL);
	VkImage *images = malloc(sizeof(VkImage) * nImages);
	vkGetSwapchainImagesKHR(device, swapchain, &nImages, images);
	fprintf(stderr, "steady_client: swapchain %u images, extent %ux%u\n", nImages, scci.imageExtent.width, scci.imageExtent.height);

	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = queueFamily;
	VkCommandPool cmdPool;
	vkCreateCommandPool(device, &cpci, NULL, &cmdPool);

	VkSemaphoreCreateInfo semCi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkSemaphore semAcquire, semRender;
	vkCreateSemaphore(device, &semCi, NULL, &semAcquire);
	vkCreateSemaphore(device, &semCi, NULL, &semRender);
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };
	VkFence fence;
	vkCreateFence(device, &fci, NULL, &fence);

	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbai.commandPool = cmdPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	vkAllocateCommandBuffers(device, &cbai, &cmd);

	// GAMESCOPE_UPSCALE_TRACE=1: gamescope's trace format, stages c_fence / c_acquired / c_presented.
	const int bTrace = getenv("GAMESCOPE_UPSCALE_TRACE") && atoi(getenv("GAMESCOPE_UPSCALE_TRACE")) == 1;
	uint64_t frame = 0, resizeAt = 0;
	uint32_t resizeW = 0, resizeH = 0;
	if (getenv("STEADY_RESIZE") && sscanf(getenv("STEADY_RESIZE"), "%" SCNu64 ":%ux%u", &resizeAt, &resizeW, &resizeH) != 3)
		die("STEADY_RESIZE must be <frame>:<w>x<h>");
	while (g_bRun && (!frameLimit || frame < frameLimit)) {
		xcb_generic_event_t *ev;
		while ((ev = xcb_poll_for_event(xc))) free(ev);

		if (resizeW && frame == resizeAt) {
			vkDeviceWaitIdle(device);
			set_fixed_size(xc, win, resizeW, resizeH);
			const uint32_t size[] = { resizeW, resizeH };
			xcb_configure_window(xc, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
			xcb_flush(xc);
			for (int i = 0; i < 100; i++) {
				vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);
				if (caps.currentExtent.width == resizeW && caps.currentExtent.height == resizeH)
					break;
				usleep(10000);
			}
			scci.imageExtent = caps.currentExtent.width != 0xFFFFFFFF ? caps.currentExtent : (VkExtent2D){ resizeW, resizeH };
			scci.oldSwapchain = swapchain;
			VkSwapchainKHR newSwapchain;
			if (vkCreateSwapchainKHR(device, &scci, NULL, &newSwapchain) != VK_SUCCESS) die("vkCreateSwapchainKHR (resize) failed");
			vkDestroySwapchainKHR(device, swapchain, NULL);
			swapchain = newSwapchain;
			free(images);
			vkGetSwapchainImagesKHR(device, swapchain, &nImages, NULL);
			images = malloc(sizeof(VkImage) * nImages);
			vkGetSwapchainImagesKHR(device, swapchain, &nImages, images);
			fprintf(stderr, "steady_client: resized at frame %" PRIu64 ": swapchain %u images, extent %ux%u\n", frame, nImages, scci.imageExtent.width, scci.imageExtent.height);
			resizeW = 0;
		}

		vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &fence);
		if (bTrace) trace(frame, "c_fence");

		uint32_t imgIdx;
		VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, semAcquire, VK_NULL_HANDLE, &imgIdx);
		if (ar == VK_ERROR_OUT_OF_DATE_KHR) { fprintf(stderr, "steady_client: swapchain out of date, exiting\n"); break; }
		if (bTrace) trace(frame, "c_acquired");

		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		vkBeginCommandBuffer(cmd, &cbbi);

		VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = images[imgIdx];
		toDst.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);

		// Encode the frame counter's low 16 bits in R/G (checkable by reading back one pixel);
		// blue cycles independently as the "moving pattern" component.
		VkClearColorValue clear;
		clear.float32[0] = (float)(frame & 0xFF) / 255.0f;
		clear.float32[1] = (float)((frame >> 8) & 0xFF) / 255.0f;
		// STEADY_BRIGHT=1: G >= 0.5, so luma never drops near black (for black-region checks).
		if (bright)
			clear.float32[1] = 0.5f + 0.5f * clear.float32[1];
		clear.float32[2] = 0.5f + 0.5f * sinf((float)frame * 0.05f);
		clear.float32[3] = 1.0f;
		vkCmdClearColorImage(cmd, images[imgIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &toDst.subresourceRange);

		VkImageMemoryBarrier toPresent = toDst;
		toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent);
		vkEndCommandBuffer(cmd);

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.waitSemaphoreCount = 1; si.pWaitSemaphores = &semAcquire; si.pWaitDstStageMask = &waitStage;
		si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
		si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semRender;
		vkQueueSubmit(queue, 1, &si, fence);

		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &semRender;
		pi.swapchainCount = 1; pi.pSwapchains = &swapchain; pi.pImageIndices = &imgIdx;
		VkResult pr = vkQueuePresentKHR(queue, &pi);
		if (pr == VK_ERROR_OUT_OF_DATE_KHR) { fprintf(stderr, "steady_client: present out of date, exiting\n"); break; }
		if (bTrace) trace(frame, "c_presented");

		frame++;
	}

	fprintf(stderr, "steady_client: exited after %" PRIu64 " frames\n", frame);
	vkDeviceWaitIdle(device);
	return 0;
}
