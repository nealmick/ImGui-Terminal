/* See LICENSE for license details. */
/*
 * main.cpp — standalone shell hosting the imgui_win terminal widget.
 *
 * This file owns the GLFW window + OpenGL context. It exists as the
 * canonical demo of how to embed the terminal widget. Anyone wanting a
 * different backend (SDL+Vulkan, native+Metal, etc.) writes their own
 * main_*.cpp and links the same imgui_win.o.
 *
 * The widget itself (imgui_win.cpp) has zero GLFW or GL dependencies —
 * see readme for the rules.
 */

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

/* Public API of imgui_win.cpp (the widget). */
extern void term_init(int cols, int rows);
extern void term_draw_widget(void);
extern void term_shutdown(void);

int
main(int, char **)
{
	if (!glfwInit())
		return 1;

	const char *glsl_version = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

	GLFWwindow *window = glfwCreateWindow(800, 600,
	                                      "st-imgui (shell)",
	                                      nullptr, nullptr);
	if (!window) {
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	/* Retina / HiDPI: tell ImGui to track monitor DPI and re-rasterize
	 * fonts at the matching density. Without this on a 2× display,
	 * glyphs are rasterized at 16 physical pixels and bilinear-stretched
	 * to ~32 — visibly blurry. With it, ImGui asks the atlas to bake at
	 * 32 physical pixels for the same logical 16 size, and the GPU
	 * draws crisp glyphs.
	 *
	 * Requires the backend to declare ImGuiBackendFlags_RendererHasTextures
	 * — the OpenGL3 backend does (see imgui_impl_opengl3.cpp header
	 * comments). On non-Retina monitors this is a no-op (DPI scale = 1). */
	ImGui::GetIO().ConfigDpiScaleFonts = true;

	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	term_init(80, 24);

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		term_draw_widget();

		ImGui::Render();
		int w, h;
		glfwGetFramebufferSize(window, &w, &h);
		glViewport(0, 0, w, h);
		glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(window);
	}

	term_shutdown();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
