/* See LICENSE for license details. */
/*
	example_mac_metal.mm — alternative shell using Metal for rendering
	on macOS.

	Functionally equivalent to main_example_glfw_gl.cpp, but renders via
	Metal (Apple's GPU API) instead of OpenGL3. GLFW still owns windowing
	and input — only the renderer changes. Demonstrates the renderer-
	agnostic principle: imgui_win.cpp is unchanged, only this host shell
	differs.

	macOS only. The .mm extension is required because Metal is an
	Objective-C API.

	  Build: `make metal`   →   build/imgui_terminal_metal
*/

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "terminal.h"

int
main(int, char **)
{
	if (!glfwInit())
		return 1;

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	GLFWwindow *window = glfwCreateWindow(800, 600, "st-imgui (metal)", nullptr, nullptr);
	if (!window)
	{
		glfwTerminate();
		return 1;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGui::GetIO().ConfigDpiScaleFonts = true;

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	id<MTLCommandQueue> commandQueue = [device newCommandQueue];

	
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplMetal_Init(device);

	
	NSWindow *nswin = glfwGetCocoaWindow(window);
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	nswin.contentView.layer = layer;
	nswin.contentView.wantsLayer = YES;

	MTLRenderPassDescriptor *passDesc = [MTLRenderPassDescriptor new];

	Terminal term;
	term.init(80, 24, NULL);

	while (!glfwWindowShouldClose(window))
	{
		@autoreleasepool
		{
			glfwPollEvents();

			int w, h;
			glfwGetFramebufferSize(window, &w, &h);
			layer.drawableSize = CGSizeMake(w, h);
			id<CAMetalDrawable> drawable = [layer nextDrawable];

			id<MTLCommandBuffer> cmdBuf = [commandQueue commandBuffer];

			passDesc.colorAttachments[0].clearColor =
			    MTLClearColorMake(0.05, 0.05, 0.05, 1.0);
			passDesc.colorAttachments[0].texture = drawable.texture;
			passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
			passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

			id<MTLRenderCommandEncoder> enc =
			    [cmdBuf renderCommandEncoderWithDescriptor:passDesc];

			ImGui_ImplMetal_NewFrame(passDesc);
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();

			term.draw_widget("###st_term_widget");
			if (!term.is_alive())
				glfwSetWindowShouldClose(window, GLFW_TRUE);

			ImGui::Render();
			ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmdBuf, enc);

			[enc endEncoding];
			[cmdBuf presentDrawable:drawable];
			[cmdBuf commit];
		}
	}

	term.shutdown();
	ImGui_ImplMetal_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
