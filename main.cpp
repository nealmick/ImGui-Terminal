#define GL_SILENCE_DEPRECATION
#define GLEW_NO_GLU
#include <GL/glew.h>
#if defined(__APPLE__)
#define GL_GLEXT_PROTOTYPES 1
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <string>
#include <iostream>
#include <filesystem>
#include "terminal.h"

// Simplified font loading function based on ned/util/font.cpp
ImFont* LoadFont(const std::string& fontName, float fontSize) {
    ImGuiIO& io = ImGui::GetIO();
    std::string fontPath = "fonts/" + fontName + ".ttf";
    
    std::cout << "Loading font from: " << fontPath << std::endl;
    if (!std::filesystem::exists(fontPath)) {
        std::cerr << "Font does not exist: " << fontPath << std::endl;
        return io.Fonts->AddFontDefault();
    }

    static const ImWchar ranges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin Supplement
        0x2500, 0x257F,  // Box Drawing Characters
        0x2580, 0x259F,  // Block Elements
        0x25A0, 0x25FF,  // Geometric Shapes
        0x2600, 0x26FF,  // Miscellaneous Symbols
        0x2700, 0x27BF,  // Dingbats
        0x2900, 0x297F,  // Supplemental Arrows-B
        0x2B00, 0x2BFF,  // Miscellaneous Symbols and Arrows
        0x3000, 0x303F,  // CJK Symbols and Punctuation
        0xE000, 0xE0FF,  // Private Use Area
        0,
    };

    ImFontConfig config_main;
    config_main.MergeMode = false;
    config_main.GlyphRanges = ranges;

    // Clear existing fonts
    io.Fonts->Clear();

    ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSize, &config_main, ranges);

    // Merge DejaVu Sans for Braille support
    ImFontConfig config_braille;
    config_braille.MergeMode = true;
    static const ImWchar braille_ranges[] = { 0x2800, 0x28FF, 0 };
    std::string dejaVuPath = "fonts/DejaVuSans.ttf";
    if (std::filesystem::exists(dejaVuPath)) {
        io.Fonts->AddFontFromFileTTF(dejaVuPath.c_str(), fontSize, &config_braille, braille_ranges);
    }

    // Emoji font loading removed - Unicode ranges too large for ImWchar

    if (!font) {
        std::cerr << "Failed to load font: " << fontPath << std::endl;
        return io.Fonts->AddFontDefault();
    }

    std::cout << "Successfully loaded font: " << fontName << std::endl;
    return font;
}

void InitializeGLFW() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        exit(1);
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif
}

GLFWwindow* CreateWindow() {
    GLFWwindow* window = glfwCreateWindow(1200, 750, "Terminal", NULL, NULL);
    if (window == NULL) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        exit(1);
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    return window;
}

void InitializeImGui(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Configure ImGui to only allow window dragging from title bar
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Load font
    ImFont* font = LoadFont("SourceCodePro-Regular", 18.0f);
    if (!font) {
        std::cerr << "Failed to load font, using default font" << std::endl;
        font = ImGui::GetIO().Fonts->AddFontDefault();
    }

    std::cout << "Initializing terminal..." << std::endl;
    gTerminal.toggleVisibility(); // Make sure terminal is visible
}

int main() {
    // Initialize GLFW
    InitializeGLFW();
    
    // Create window
    GLFWwindow* window = CreateWindow();
    
    // Initialize GLEW
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (GLEW_OK != err) {
        std::cerr << "GLEW initialization failed: " << glewGetErrorString(err) << std::endl;
        glfwTerminate();
        return -1;
    }
    glGetError(); // Clear any error that GLEW init might have caused

    // Initialize ImGui
    InitializeImGui(window);

    while (!glfwWindowShouldClose(window)) {
        // Poll events
        glfwPollEvents();

        // Get framebuffer size
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Clear screen
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Set terminal to embedded mode and let it handle its own window
        gTerminal.setEmbedded(true);
        
        // Let the terminal render itself with its own window management
        gTerminal.render();

        // Complete ImGui frame
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Swap buffers
        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
